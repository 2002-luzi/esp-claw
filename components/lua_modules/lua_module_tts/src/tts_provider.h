/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*tts_pcm_write_fn_t)(void *ctx, const uint8_t *pcm, size_t len);

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} tts_audio_format_t;

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    const char *voice;
    const char *style;
    uint32_t timeout_ms;
} tts_provider_config_t;

typedef struct {
    tts_pcm_write_fn_t write_pcm;
    void *write_ctx;
    size_t http_bytes;
} tts_provider_stream_t;

typedef struct {
    const char *name;
    const tts_audio_format_t *audio_format;
    esp_err_t (*play)(const tts_provider_config_t *config,
                      const char *text,
                      tts_provider_stream_t *stream);
} tts_provider_t;

const tts_provider_t *tts_provider_find(const char *name);

#ifdef __cplusplus
}
#endif
