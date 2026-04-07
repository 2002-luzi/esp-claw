#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTS_ENGINE_PROVIDER_NAME_LEN       16
#define TTS_ENGINE_PROVIDER_CODEC_LEN      24
#define TTS_ENGINE_PROVIDER_SESSION_ID_LEN 64
#define TTS_ENGINE_PROVIDER_MESSAGE_LEN    128

typedef enum {
    TTS_ENGINE_PROVIDER_EVENT_STARTED = 0,
    TTS_ENGINE_PROVIDER_EVENT_CONNECTED,
    TTS_ENGINE_PROVIDER_EVENT_AUDIO_CHUNK,
    TTS_ENGINE_PROVIDER_EVENT_COMPLETED,
    TTS_ENGINE_PROVIDER_EVENT_ABORTED,
    TTS_ENGINE_PROVIDER_EVENT_ERROR,
} tts_engine_provider_event_type_t;

typedef struct {
    uint32_t service_session_id;
    int32_t connect_ms;
    int32_t first_audio_ms;
    int32_t complete_ms;
    uint32_t audio_bytes;
    int32_t server_code;
    int handshake_status;
    int transport_errno;
    esp_err_t transport_err;
    int tls_stack_err;
    bool completed;
    uint32_t sample_rate_hz;
    uint8_t channels;
    char provider_name[TTS_ENGINE_PROVIDER_NAME_LEN];
    char codec[TTS_ENGINE_PROVIDER_CODEC_LEN];
    char provider_session_id[TTS_ENGINE_PROVIDER_SESSION_ID_LEN];
    char provider_message[TTS_ENGINE_PROVIDER_MESSAGE_LEN];
} tts_engine_provider_stream_status_t;

typedef struct {
    tts_engine_provider_event_type_t type;
    const tts_engine_provider_stream_status_t *status;
    const uint8_t *audio;
    size_t audio_len;
    esp_err_t error;
} tts_engine_provider_event_t;

typedef esp_err_t (*tts_engine_provider_event_fn)(void *user_ctx,
                                                  const tts_engine_provider_event_t *event);

typedef struct {
    void *user_ctx;
    tts_engine_provider_event_fn on_event;
} tts_engine_provider_stream_listener_t;

typedef struct {
    const char *text;
    uint32_t timeout_ms;
    uint32_t service_session_id;
    const tts_engine_provider_stream_listener_t *listener;
} tts_engine_provider_request_t;

typedef struct {
    const char *provider_name;
    esp_err_t (*start)(const tts_engine_provider_request_t *request,
                       tts_engine_provider_stream_status_t *out_status);
    esp_err_t (*abort)(void);
} tts_engine_provider_ops_t;

#ifdef __cplusplus
}
#endif
