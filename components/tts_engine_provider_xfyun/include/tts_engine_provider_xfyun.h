#pragma once

#include "esp_err.h"
#include "tts_engine_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TTS_ENGINE_PROVIDER_XFYUN_APP_ID_LEN        32
#define TTS_ENGINE_PROVIDER_XFYUN_API_KEY_LEN       64
#define TTS_ENGINE_PROVIDER_XFYUN_API_SECRET_LEN    64
#define TTS_ENGINE_PROVIDER_XFYUN_VOICE_LEN         32
#define TTS_ENGINE_PROVIDER_XFYUN_AUDIO_ENCODING_LEN 24
#define TTS_ENGINE_PROVIDER_XFYUN_AUDIO_FORMAT_LEN  48
#define TTS_ENGINE_PROVIDER_XFYUN_TEXT_ENCODING_LEN 16
#define TTS_ENGINE_PROVIDER_XFYUN_URI_LEN           128
#define TTS_ENGINE_PROVIDER_XFYUN_HOST_LEN          64

typedef struct {
    char app_id[TTS_ENGINE_PROVIDER_XFYUN_APP_ID_LEN];
    char api_key[TTS_ENGINE_PROVIDER_XFYUN_API_KEY_LEN];
    char api_secret[TTS_ENGINE_PROVIDER_XFYUN_API_SECRET_LEN];
    char voice_name[TTS_ENGINE_PROVIDER_XFYUN_VOICE_LEN];
    char audio_encoding[TTS_ENGINE_PROVIDER_XFYUN_AUDIO_ENCODING_LEN];
    char audio_format[TTS_ENGINE_PROVIDER_XFYUN_AUDIO_FORMAT_LEN];
    char text_encoding[TTS_ENGINE_PROVIDER_XFYUN_TEXT_ENCODING_LEN];
    char websocket_uri[TTS_ENGINE_PROVIDER_XFYUN_URI_LEN];
    char auth_host[TTS_ENGINE_PROVIDER_XFYUN_HOST_LEN];
    uint32_t timeout_ms;
} tts_engine_provider_xfyun_config_t;

esp_err_t tts_engine_provider_xfyun_set_config(const tts_engine_provider_xfyun_config_t *config);
const tts_engine_provider_ops_t *tts_engine_provider_xfyun_get_ops(void);

#ifdef __cplusplus
}
#endif
