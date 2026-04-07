#include "tts_engine.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tts_engine_playback.h"

static const char *TAG = "tts_engine";

#define TTS_ENGINE_EVENT_WORK       BIT0
#define TTS_ENGINE_TASK_STACK_WORDS 6144
#define TTS_ENGINE_TASK_PRIORITY    5

typedef struct {
    uint32_t utterance_id;
    uint32_t session_id;
    char source[TTS_ENGINE_MAX_SOURCE_LEN];
    char text[TTS_ENGINE_MAX_TEXT_LEN + 1];
} tts_engine_slot_t;

typedef struct {
    bool initialized;
    bool started;
    bool provider_running;
    bool stop_requested;
    const tts_engine_provider_ops_t *provider_ops;
    uint32_t queue_capacity;
    uint32_t max_text_len;
    uint32_t provider_timeout_ms;
    uint32_t next_utterance_id;
    uint32_t next_session_id;
    size_t pending_count;
    tts_engine_slot_t pending[TTS_ENGINE_MAX_PENDING];
    bool active_present;
    tts_engine_slot_t active;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
    EventGroupHandle_t events;
    StaticEventGroup_t event_buffer;
    TaskHandle_t task_handle;
    StaticTask_t task_buffer;
    StackType_t task_stack[TTS_ENGINE_TASK_STACK_WORDS];
    uint32_t last_result_utterance_id;
    uint32_t last_result_session_id;
    tts_engine_result_t last_result;
    esp_err_t last_error;
    tts_engine_provider_stream_status_t last_provider_status;
} tts_engine_runtime_t;

static tts_engine_runtime_t s_engine = {
    .queue_capacity = TTS_ENGINE_MAX_PENDING,
    .max_text_len = TTS_ENGINE_MAX_TEXT_LEN,
    .provider_timeout_ms = 30000,
    .next_utterance_id = 1,
    .next_session_id = 1,
};

static void tts_engine_lock(void)
{
    xSemaphoreTake(s_engine.lock, portMAX_DELAY);
}

static void tts_engine_unlock(void)
{
    xSemaphoreGive(s_engine.lock);
}

static void tts_engine_copy_info(tts_engine_utterance_info_t *dst,
                                 const tts_engine_slot_t *src)
{
    if (!dst) {
        return;
    }

    memset(dst, 0, sizeof(*dst));
    if (!src || src->utterance_id == 0) {
        return;
    }

    dst->present = true;
    dst->utterance_id = src->utterance_id;
    dst->session_id = src->session_id;
    strlcpy(dst->source, src->source, sizeof(dst->source));
    strlcpy(dst->text, src->text, sizeof(dst->text));
}

static esp_err_t tts_engine_provider_event(void *user_ctx,
                                           const tts_engine_provider_event_t *event)
{
    tts_engine_slot_t active = {0};
    esp_err_t err = ESP_OK;

    (void)user_ctx;

    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }

    tts_engine_lock();
    if (event->status) {
        s_engine.last_provider_status = *event->status;
    }
    if (event->error != ESP_OK) {
        s_engine.last_error = event->error;
    }
    if (s_engine.active_present) {
        active = s_engine.active;
    }
    tts_engine_unlock();

    switch (event->type) {
    case TTS_ENGINE_PROVIDER_EVENT_AUDIO_CHUNK:
        err = tts_engine_playback_enqueue_audio(event->status, event->audio, event->audio_len);
        break;
    case TTS_ENGINE_PROVIDER_EVENT_COMPLETED:
        err = tts_engine_playback_complete(active.session_id);
        break;
    case TTS_ENGINE_PROVIDER_EVENT_ABORTED:
    case TTS_ENGINE_PROVIDER_EVENT_ERROR:
        err = tts_engine_playback_stop();
        break;
    case TTS_ENGINE_PROVIDER_EVENT_STARTED:
    case TTS_ENGINE_PROVIDER_EVENT_CONNECTED:
    default:
        break;
    }

    return err;
}

static void tts_engine_shift_right(size_t index)
{
    size_t i;

    for (i = s_engine.pending_count; i > index; i--) {
        s_engine.pending[i] = s_engine.pending[i - 1];
    }
}

static void tts_engine_shift_left(void)
{
    size_t i;

    if (s_engine.pending_count == 0) {
        return;
    }

    for (i = 1; i < s_engine.pending_count; i++) {
        s_engine.pending[i - 1] = s_engine.pending[i];
    }
    memset(&s_engine.pending[s_engine.pending_count - 1], 0, sizeof(s_engine.pending[0]));
}

static esp_err_t tts_engine_enqueue_internal(const char *text,
                                             const char *source,
                                             bool insert_front,
                                             uint32_t *utterance_id)
{
    const char *resolved_source = source && source[0] ? source : "system";
    tts_engine_slot_t *slot;
    size_t insert_index;

    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(text) > s_engine.max_text_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (strlen(resolved_source) >= TTS_ENGINE_MAX_SOURCE_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_engine.initialized || !s_engine.started || !s_engine.provider_ops) {
        return ESP_ERR_INVALID_STATE;
    }

    tts_engine_lock();
    if (s_engine.pending_count >= s_engine.queue_capacity) {
        tts_engine_unlock();
        return ESP_ERR_NO_MEM;
    }

    insert_index = insert_front ? 0 : s_engine.pending_count;
    if (insert_front && s_engine.pending_count > 0) {
        tts_engine_shift_right(0);
    }

    slot = &s_engine.pending[insert_index];
    memset(slot, 0, sizeof(*slot));
    slot->utterance_id = s_engine.next_utterance_id++;
    strlcpy(slot->source, resolved_source, sizeof(slot->source));
    strlcpy(slot->text, text, sizeof(slot->text));
    s_engine.pending_count++;
    if (utterance_id) {
        *utterance_id = slot->utterance_id;
    }
    tts_engine_unlock();

    xEventGroupSetBits(s_engine.events, TTS_ENGINE_EVENT_WORK);
    return ESP_OK;
}

static void tts_engine_task(void *arg)
{
    tts_engine_provider_stream_listener_t listener = {
        .user_ctx = NULL,
        .on_event = tts_engine_provider_event,
    };

    (void)arg;

    while (true) {
        tts_engine_slot_t active = {0};
        tts_engine_provider_stream_status_t status = {0};
        tts_engine_provider_request_t request = {0};
        tts_engine_result_t result;
        esp_err_t err;
        bool stop_requested;

        xEventGroupWaitBits(s_engine.events,
                            TTS_ENGINE_EVENT_WORK,
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);

        while (true) {
            tts_engine_lock();
            if (!s_engine.started || s_engine.pending_count == 0 || s_engine.active_present) {
                tts_engine_unlock();
                break;
            }

            active = s_engine.pending[0];
            tts_engine_shift_left();
            s_engine.pending_count--;
            active.session_id = s_engine.next_session_id++;
            s_engine.active = active;
            s_engine.active_present = true;
            s_engine.provider_running = true;
            s_engine.stop_requested = false;
            s_engine.last_error = ESP_OK;
            memset(&s_engine.last_provider_status, 0, sizeof(s_engine.last_provider_status));
            s_engine.last_provider_status.server_code = -1;
            tts_engine_unlock();

            request.text = active.text;
            request.timeout_ms = s_engine.provider_timeout_ms;
            request.service_session_id = active.session_id;
            request.listener = &listener;

            ESP_LOGI(TAG, "Starting utterance=%" PRIu32 " session=%" PRIu32,
                     active.utterance_id,
                     active.session_id);

            err = s_engine.provider_ops->start(&request, &status);

            tts_engine_lock();
            s_engine.provider_running = false;
            s_engine.last_provider_status = status;
            stop_requested = s_engine.stop_requested;
            if (err != ESP_OK) {
                s_engine.last_error = err;
            }
            result = stop_requested ? TTS_ENGINE_RESULT_ABORTED :
                (err == ESP_OK ? TTS_ENGINE_RESULT_COMPLETED : TTS_ENGINE_RESULT_FAILED);
            s_engine.last_result = result;
            s_engine.last_result_utterance_id = active.utterance_id;
            s_engine.last_result_session_id = active.session_id;
            s_engine.active_present = false;
            memset(&s_engine.active, 0, sizeof(s_engine.active));
            s_engine.stop_requested = false;
            tts_engine_unlock();
        }
    }
}

esp_err_t tts_engine_init(const tts_engine_config_t *config)
{
    esp_err_t err;

    if (!config || !config->provider_ops || !config->provider_ops->start) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_engine.lock == NULL) {
        s_engine.lock = xSemaphoreCreateMutexStatic(&s_engine.lock_buffer);
    }
    if (s_engine.events == NULL) {
        s_engine.events = xEventGroupCreateStatic(&s_engine.event_buffer);
    }
    if (!s_engine.lock || !s_engine.events) {
        return ESP_FAIL;
    }

    err = tts_engine_playback_init(config->speaker);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init playback: %s", esp_err_to_name(err));
        return err;
    }

    s_engine.provider_ops = config->provider_ops;
    s_engine.queue_capacity = config->queue_capacity ? config->queue_capacity :
        TTS_ENGINE_MAX_PENDING;
    if (s_engine.queue_capacity > TTS_ENGINE_MAX_PENDING) {
        s_engine.queue_capacity = TTS_ENGINE_MAX_PENDING;
    }
    s_engine.max_text_len = config->max_text_len ? config->max_text_len :
        TTS_ENGINE_MAX_TEXT_LEN;
    if (s_engine.max_text_len > TTS_ENGINE_MAX_TEXT_LEN) {
        s_engine.max_text_len = TTS_ENGINE_MAX_TEXT_LEN;
    }
    s_engine.provider_timeout_ms = config->provider_timeout_ms ? config->provider_timeout_ms : 30000;
    s_engine.last_provider_status.server_code = -1;

    if (!s_engine.task_handle) {
        s_engine.task_handle = xTaskCreateStatic(tts_engine_task,
                                                 "tts_engine",
                                                 TTS_ENGINE_TASK_STACK_WORDS,
                                                 NULL,
                                                 TTS_ENGINE_TASK_PRIORITY,
                                                 s_engine.task_stack,
                                                 &s_engine.task_buffer);
        if (!s_engine.task_handle) {
            return ESP_FAIL;
        }
    }

    s_engine.initialized = true;
    return ESP_OK;
}

esp_err_t tts_engine_start(void)
{
    if (!s_engine.initialized || !s_engine.provider_ops) {
        return ESP_ERR_INVALID_STATE;
    }

    s_engine.started = true;
    xEventGroupSetBits(s_engine.events, TTS_ENGINE_EVENT_WORK);
    return ESP_OK;
}

esp_err_t tts_engine_stop(void)
{
    bool had_active = false;

    if (!s_engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_engine.started = false;
    tts_engine_clear_pending(NULL);
    return tts_engine_stop_current(&had_active);
}

bool tts_engine_is_ready(void)
{
    return s_engine.initialized && s_engine.started && s_engine.provider_ops != NULL;
}

esp_err_t tts_engine_speak(const char *text,
                           const char *source,
                           uint32_t *utterance_id)
{
    return tts_engine_enqueue_internal(text, source, false, utterance_id);
}

esp_err_t tts_engine_enqueue_front(const char *text,
                                   const char *source,
                                   uint32_t *utterance_id)
{
    return tts_engine_enqueue_internal(text, source, true, utterance_id);
}

esp_err_t tts_engine_clear_pending(size_t *cleared_count)
{
    size_t old_count;

    if (!s_engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    tts_engine_lock();
    old_count = s_engine.pending_count;
    memset(s_engine.pending, 0, sizeof(s_engine.pending));
    s_engine.pending_count = 0;
    tts_engine_unlock();

    if (cleared_count) {
        *cleared_count = old_count;
    }
    return ESP_OK;
}

esp_err_t tts_engine_stop_current(bool *had_active)
{
    bool active;

    if (!s_engine.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    tts_engine_lock();
    active = s_engine.active_present || s_engine.provider_running;
    if (active) {
        s_engine.stop_requested = true;
    }
    tts_engine_unlock();

    if (had_active) {
        *had_active = active;
    }
    if (!active) {
        return ESP_OK;
    }

    tts_engine_playback_stop();
    if (s_engine.provider_ops && s_engine.provider_ops->abort) {
        return s_engine.provider_ops->abort();
    }

    return ESP_OK;
}

esp_err_t tts_engine_get_status(tts_engine_status_t *status)
{
    if (!status || !s_engine.initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));

    tts_engine_lock();
    status->queue_capacity = s_engine.queue_capacity;
    status->max_text_len = s_engine.max_text_len;
    status->pending_count = s_engine.pending_count;
    status->provider_running = s_engine.provider_running;
    status->stop_requested = s_engine.stop_requested;
    status->last_result = s_engine.last_result;
    status->last_result_utterance_id = s_engine.last_result_utterance_id;
    status->last_result_session_id = s_engine.last_result_session_id;
    status->last_error = s_engine.last_error;
    status->last_provider_status = s_engine.last_provider_status;
    if (!s_engine.started) {
        status->state = TTS_ENGINE_STATE_DISABLED;
    } else if (s_engine.stop_requested) {
        status->state = TTS_ENGINE_STATE_STOPPING;
    } else if (s_engine.active_present) {
        status->state = TTS_ENGINE_STATE_ACTIVE;
    } else {
        status->state = TTS_ENGINE_STATE_IDLE;
    }
    tts_engine_copy_info(&status->active, s_engine.active_present ? &s_engine.active : NULL);
    tts_engine_copy_info(&status->next_pending,
                         s_engine.pending_count > 0 ? &s_engine.pending[0] : NULL);
    tts_engine_unlock();

    return ESP_OK;
}
