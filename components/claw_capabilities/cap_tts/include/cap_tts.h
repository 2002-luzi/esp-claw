#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tts_engine_speaker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*cap_tts_speaker_init_fn)(tts_engine_speaker_t *speaker);

typedef struct {
    const char *xfyun_app_id;
    const char *xfyun_api_key;
    const char *xfyun_api_secret;
    const char *xfyun_voice_name;
    const char *xfyun_audio_encoding;
    const char *xfyun_audio_format;
    const char *xfyun_text_encoding;
    const char *xfyun_websocket_uri;
    const char *xfyun_auth_host;
    uint32_t timeout_ms;
    bool enable_speaker_output;
    cap_tts_speaker_init_fn speaker_init;
} cap_tts_runtime_config_t;

esp_err_t cap_tts_init_runtime(const cap_tts_runtime_config_t *config);
esp_err_t cap_tts_register_group(void);

#ifdef __cplusplus
}
#endif
