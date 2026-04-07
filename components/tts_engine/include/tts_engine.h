#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tts_engine_provider.h"
#include "tts_engine_speaker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTS_ENGINE_MAX_PENDING    4
#define TTS_ENGINE_MAX_TEXT_LEN   256
#define TTS_ENGINE_MAX_SOURCE_LEN 16

typedef enum {
    TTS_ENGINE_STATE_IDLE = 0,
    TTS_ENGINE_STATE_ACTIVE,
    TTS_ENGINE_STATE_STOPPING,
    TTS_ENGINE_STATE_DISABLED,
} tts_engine_state_t;

typedef enum {
    TTS_ENGINE_RESULT_NONE = 0,
    TTS_ENGINE_RESULT_COMPLETED,
    TTS_ENGINE_RESULT_ABORTED,
    TTS_ENGINE_RESULT_FAILED,
} tts_engine_result_t;

typedef struct {
    bool present;
    uint32_t utterance_id;
    uint32_t session_id;
    char source[TTS_ENGINE_MAX_SOURCE_LEN];
    char text[TTS_ENGINE_MAX_TEXT_LEN + 1];
} tts_engine_utterance_info_t;

typedef struct {
    const tts_engine_provider_ops_t *provider_ops;
    uint32_t queue_capacity;
    uint32_t max_text_len;
    uint32_t provider_timeout_ms;
    const tts_engine_speaker_t *speaker;
} tts_engine_config_t;

typedef struct {
    tts_engine_state_t state;
    uint32_t queue_capacity;
    uint32_t max_text_len;
    size_t pending_count;
    bool provider_running;
    bool stop_requested;
    tts_engine_utterance_info_t active;
    tts_engine_utterance_info_t next_pending;
    uint32_t last_result_utterance_id;
    uint32_t last_result_session_id;
    tts_engine_result_t last_result;
    esp_err_t last_error;
    tts_engine_provider_stream_status_t last_provider_status;
} tts_engine_status_t;

esp_err_t tts_engine_init(const tts_engine_config_t *config);
esp_err_t tts_engine_start(void);
esp_err_t tts_engine_stop(void);
bool tts_engine_is_ready(void);

esp_err_t tts_engine_speak(const char *text,
                           const char *source,
                           uint32_t *utterance_id);
esp_err_t tts_engine_enqueue_front(const char *text,
                                   const char *source,
                                   uint32_t *utterance_id);
esp_err_t tts_engine_clear_pending(size_t *cleared_count);
esp_err_t tts_engine_stop_current(bool *had_active);
esp_err_t tts_engine_get_status(tts_engine_status_t *status);

#ifdef __cplusplus
}
#endif
