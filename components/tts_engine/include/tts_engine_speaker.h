#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
    bool codec_is_raw;
} tts_engine_speaker_format_t;

typedef esp_err_t (*tts_engine_speaker_open_fn)(void *user_ctx,
                                                const tts_engine_speaker_format_t *format);
typedef esp_err_t (*tts_engine_speaker_write_fn)(void *user_ctx,
                                                 const uint8_t *audio,
                                                 size_t audio_len);
typedef esp_err_t (*tts_engine_speaker_close_fn)(void *user_ctx);

typedef struct {
    void *user_ctx;
    tts_engine_speaker_open_fn open;
    tts_engine_speaker_write_fn write;
    tts_engine_speaker_close_fn close;
} tts_engine_speaker_t;

#ifdef __cplusplus
}
#endif
