#include "tts_engine_playback.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "tts_engine_pb";

#define TTS_ENGINE_PLAYBACK_EVENT_WORK       BIT0
#define TTS_ENGINE_PLAYBACK_QUEUE_LEN        2
#define TTS_ENGINE_PLAYBACK_CHUNK_BYTES      1920
#define TTS_ENGINE_PLAYBACK_TASK_STACK_WORDS 4096
#define TTS_ENGINE_PLAYBACK_TASK_PRIORITY    5

typedef struct {
    uint32_t service_session_id;
    tts_engine_speaker_format_t format;
    size_t audio_len;
    uint8_t audio[TTS_ENGINE_PLAYBACK_CHUNK_BYTES];
} tts_engine_playback_chunk_t;

typedef struct {
    bool initialized;
    bool playback_enabled;
    bool playback_stop_requested;
    uint32_t close_after_session_id;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
    EventGroupHandle_t playback_events;
    StaticEventGroup_t playback_event_buffer;
    SemaphoreHandle_t playback_space;
    StaticSemaphore_t playback_space_buffer;
    TaskHandle_t playback_task_handle;
    StaticTask_t playback_task_buffer;
    StackType_t playback_task_stack[TTS_ENGINE_PLAYBACK_TASK_STACK_WORDS];
    tts_engine_speaker_t speaker;
    bool speaker_open;
    tts_engine_speaker_format_t opened_format;
    tts_engine_playback_chunk_t playback_queue[TTS_ENGINE_PLAYBACK_QUEUE_LEN];
    size_t playback_queue_read;
    size_t playback_queue_write;
    size_t playback_queue_count;
} tts_engine_playback_runtime_t;

static tts_engine_playback_runtime_t s_playback = {0};

static void tts_engine_playback_lock(void)
{
    xSemaphoreTake(s_playback.lock, portMAX_DELAY);
}

static void tts_engine_playback_unlock(void)
{
    xSemaphoreGive(s_playback.lock);
}

static bool tts_engine_playback_ready(void)
{
    return s_playback.playback_enabled &&
           s_playback.playback_events != NULL &&
           s_playback.playback_space != NULL &&
           s_playback.playback_task_handle != NULL;
}

static bool tts_engine_speaker_format_equal(const tts_engine_speaker_format_t *lhs,
                                            const tts_engine_speaker_format_t *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }

    return lhs->sample_rate_hz == rhs->sample_rate_hz &&
           lhs->channels == rhs->channels &&
           lhs->bits_per_sample == rhs->bits_per_sample &&
           lhs->codec_is_raw == rhs->codec_is_raw;
}

static tts_engine_speaker_format_t tts_engine_playback_make_format(
    const tts_engine_provider_stream_status_t *status)
{
    tts_engine_speaker_format_t format = {
        .sample_rate_hz = 16000,
        .channels = 1,
        .bits_per_sample = 16,
        .codec_is_raw = true,
    };

    if (!status) {
        return format;
    }

    if (status->sample_rate_hz != 0) {
        format.sample_rate_hz = status->sample_rate_hz;
    }
    if (status->channels != 0) {
        format.channels = status->channels;
    }
    if (status->codec[0] != '\0' && strcmp(status->codec, "raw") != 0) {
        format.codec_is_raw = false;
    }

    return format;
}

static void tts_engine_playback_signal(void)
{
    if (s_playback.playback_events != NULL) {
        xEventGroupSetBits(s_playback.playback_events, TTS_ENGINE_PLAYBACK_EVENT_WORK);
    }
}

static void tts_engine_playback_reset_queue_locked(void)
{
    s_playback.playback_queue_read = 0;
    s_playback.playback_queue_write = 0;
    s_playback.playback_queue_count = 0;
}

static void tts_engine_playback_release_slots(size_t slot_count)
{
    size_t i;

    if (s_playback.playback_space == NULL) {
        return;
    }

    for (i = 0; i < slot_count; i++) {
        xSemaphoreGive(s_playback.playback_space);
    }
}

static esp_err_t tts_engine_playback_close_speaker(void)
{
    esp_err_t ret;

    if (!s_playback.speaker_open || s_playback.speaker.close == NULL) {
        return ESP_OK;
    }

    ret = s_playback.speaker.close(s_playback.speaker.user_ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Speaker close failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_playback.speaker_open = false;
    memset(&s_playback.opened_format, 0, sizeof(s_playback.opened_format));
    return ESP_OK;
}

static esp_err_t tts_engine_playback_open_speaker_if_needed(
    const tts_engine_speaker_format_t *format)
{
    esp_err_t ret;

    if (!format || s_playback.speaker.open == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!format->codec_is_raw) {
        ESP_LOGW(TAG, "Skip unsupported non-raw codec");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_playback.speaker_open &&
        tts_engine_speaker_format_equal(&s_playback.opened_format, format)) {
        return ESP_OK;
    }

    if (s_playback.speaker_open) {
        ret = tts_engine_playback_close_speaker();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    ret = s_playback.speaker.open(s_playback.speaker.user_ctx, format);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Speaker open failed rate=%" PRIu32 " ch=%u bits=%u err=%s",
                 format->sample_rate_hz,
                 format->channels,
                 format->bits_per_sample,
                 esp_err_to_name(ret));
        return ret;
    }

    s_playback.opened_format = *format;
    s_playback.speaker_open = true;
    ESP_LOGI(TAG,
             "Speaker opened rate=%" PRIu32 " ch=%u bits=%u",
             format->sample_rate_hz,
             format->channels,
             format->bits_per_sample);
    return ESP_OK;
}

static void tts_engine_playback_task(void *arg)
{
    (void)arg;

    while (true) {
        xEventGroupWaitBits(s_playback.playback_events,
                            TTS_ENGINE_PLAYBACK_EVENT_WORK,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        while (true) {
            tts_engine_playback_chunk_t *chunk = NULL;
            bool stop_requested = false;
            bool close_before_next = false;
            bool have_chunk = false;
            esp_err_t ret;

            tts_engine_playback_lock();
            if (s_playback.playback_stop_requested) {
                s_playback.playback_stop_requested = false;
                stop_requested = true;
            } else if (s_playback.close_after_session_id != 0) {
                if (s_playback.playback_queue_count == 0) {
                    s_playback.close_after_session_id = 0;
                    close_before_next = true;
                } else {
                    tts_engine_playback_chunk_t *next_chunk =
                        &s_playback.playback_queue[s_playback.playback_queue_read];

                    if (next_chunk->service_session_id != s_playback.close_after_session_id) {
                        s_playback.close_after_session_id = 0;
                        close_before_next = true;
                    }
                }
            }

            if (!stop_requested && !close_before_next && s_playback.playback_queue_count > 0) {
                chunk = &s_playback.playback_queue[s_playback.playback_queue_read];
                s_playback.playback_queue_read =
                    (s_playback.playback_queue_read + 1) % TTS_ENGINE_PLAYBACK_QUEUE_LEN;
                s_playback.playback_queue_count--;
                have_chunk = true;
            }
            tts_engine_playback_unlock();

            if (stop_requested) {
                tts_engine_playback_close_speaker();
                continue;
            }
            if (close_before_next) {
                tts_engine_playback_close_speaker();
                continue;
            }
            if (!have_chunk) {
                break;
            }

            ret = tts_engine_playback_open_speaker_if_needed(&chunk->format);
            if (ret == ESP_OK) {
                ret = s_playback.speaker.write(s_playback.speaker.user_ctx,
                                               chunk->audio,
                                               chunk->audio_len);
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Speaker write failed: %s", esp_err_to_name(ret));
                tts_engine_playback_close_speaker();
            }
            xSemaphoreGive(s_playback.playback_space);
        }
    }
}

esp_err_t tts_engine_playback_init(const tts_engine_speaker_t *speaker)
{
    if (s_playback.lock == NULL) {
        s_playback.lock = xSemaphoreCreateMutexStatic(&s_playback.lock_buffer);
    }
    if (s_playback.lock == NULL) {
        return ESP_FAIL;
    }

    tts_engine_playback_lock();
    memset(&s_playback.speaker, 0, sizeof(s_playback.speaker));
    if (speaker) {
        s_playback.speaker = *speaker;
    }
    s_playback.playback_enabled =
        s_playback.speaker.open != NULL &&
        s_playback.speaker.write != NULL &&
        s_playback.speaker.close != NULL;
    s_playback.playback_stop_requested = false;
    s_playback.close_after_session_id = 0;
    s_playback.speaker_open = false;
    memset(&s_playback.opened_format, 0, sizeof(s_playback.opened_format));
    tts_engine_playback_reset_queue_locked();
    tts_engine_playback_unlock();

    if (!s_playback.playback_enabled) {
        if (speaker != NULL) {
            ESP_LOGW(TAG, "Speaker adapter missing callbacks, playback disabled");
        }
        s_playback.initialized = true;
        return ESP_OK;
    }

    if (s_playback.playback_events == NULL) {
        s_playback.playback_events =
            xEventGroupCreateStatic(&s_playback.playback_event_buffer);
        if (s_playback.playback_events == NULL) {
            ESP_LOGE(TAG, "Failed to create playback event group");
            return ESP_FAIL;
        }
    }
    if (s_playback.playback_space == NULL) {
        s_playback.playback_space =
            xSemaphoreCreateCountingStatic(TTS_ENGINE_PLAYBACK_QUEUE_LEN,
                                           TTS_ENGINE_PLAYBACK_QUEUE_LEN,
                                           &s_playback.playback_space_buffer);
        if (s_playback.playback_space == NULL) {
            ESP_LOGE(TAG, "Failed to create playback space semaphore");
            return ESP_FAIL;
        }
    }
    if (s_playback.playback_task_handle == NULL) {
        s_playback.playback_task_handle = xTaskCreateStatic(tts_engine_playback_task,
                                                            "tts_playback",
                                                            TTS_ENGINE_PLAYBACK_TASK_STACK_WORDS,
                                                            NULL,
                                                            TTS_ENGINE_PLAYBACK_TASK_PRIORITY,
                                                            s_playback.playback_task_stack,
                                                            &s_playback.playback_task_buffer);
        if (s_playback.playback_task_handle == NULL) {
            ESP_LOGE(TAG, "Failed to create playback task");
            return ESP_FAIL;
        }
    }

    s_playback.initialized = true;
    return ESP_OK;
}

esp_err_t tts_engine_playback_enqueue_audio(const tts_engine_provider_stream_status_t *status,
                                            const uint8_t *audio,
                                            size_t audio_len)
{
    tts_engine_speaker_format_t format;
    size_t offset = 0;

    if (!tts_engine_playback_ready() || !audio || audio_len == 0) {
        return ESP_OK;
    }

    format = tts_engine_playback_make_format(status);

    while (offset < audio_len) {
        tts_engine_playback_chunk_t *chunk;
        size_t chunk_len = audio_len - offset;

        if (chunk_len > TTS_ENGINE_PLAYBACK_CHUNK_BYTES) {
            chunk_len = TTS_ENGINE_PLAYBACK_CHUNK_BYTES;
        }

        if (xSemaphoreTake(s_playback.playback_space, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to wait for playback queue space");
            return ESP_FAIL;
        }

        tts_engine_playback_lock();
        if (s_playback.playback_queue_count >= TTS_ENGINE_PLAYBACK_QUEUE_LEN) {
            tts_engine_playback_unlock();
            xSemaphoreGive(s_playback.playback_space);
            ESP_LOGE(TAG, "Playback queue accounting mismatch");
            return ESP_FAIL;
        }

        chunk = &s_playback.playback_queue[s_playback.playback_queue_write];
        memset(chunk, 0, sizeof(*chunk));
        chunk->service_session_id = status ? status->service_session_id : 0;
        chunk->format = format;
        chunk->audio_len = chunk_len;
        memcpy(chunk->audio, audio + offset, chunk_len);
        s_playback.playback_queue_write =
            (s_playback.playback_queue_write + 1) % TTS_ENGINE_PLAYBACK_QUEUE_LEN;
        s_playback.playback_queue_count++;
        tts_engine_playback_unlock();

        offset += chunk_len;
    }

    tts_engine_playback_signal();
    return ESP_OK;
}

esp_err_t tts_engine_playback_complete(uint32_t session_id)
{
    if (!tts_engine_playback_ready() || session_id == 0) {
        return ESP_OK;
    }

    tts_engine_playback_lock();
    s_playback.close_after_session_id = session_id;
    tts_engine_playback_unlock();

    tts_engine_playback_signal();
    return ESP_OK;
}

esp_err_t tts_engine_playback_stop(void)
{
    size_t released_slots = 0;

    if (!tts_engine_playback_ready()) {
        return ESP_OK;
    }

    tts_engine_playback_lock();
    released_slots = s_playback.playback_queue_count;
    tts_engine_playback_reset_queue_locked();
    s_playback.close_after_session_id = 0;
    s_playback.playback_stop_requested = true;
    tts_engine_playback_unlock();

    tts_engine_playback_release_slots(released_slots);
    tts_engine_playback_signal();
    return ESP_OK;
}
