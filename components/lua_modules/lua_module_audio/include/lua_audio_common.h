/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_asrc.h"
#include "esp_asrc_types.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_ASRC_COMPLEXITY                 3
#define AUDIO_ASRC_TIMEOUT_MS                 1000
#define AUDIO_CODEC_VREG_FORMAT_MAGIC         0x7AC0
#define AUDIO_CODEC_VREG_FORMAT_SAMPLE_RATE   0x7AC1
#define AUDIO_CODEC_VREG_FORMAT_CHANNELS      0x7AC2
#define AUDIO_CODEC_VREG_FORMAT_BITS          0x7AC3
#define AUDIO_CODEC_VREG_FORMAT_MAGIC_VALUE   0x55414346

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint8_t bytes_per_frame;
} audio_format_t;

typedef struct {
    bool bypass;
    esp_asrc_handle_t handle;
    audio_format_t src;
    audio_format_t dst;
    uint16_t in_frame_bytes;
    uint16_t out_frame_bytes;
    uint8_t *in_buf;
    uint32_t in_buf_size;
    uint8_t *out_buf;
    uint32_t out_buf_size;
    esp_asrc_buffer_alignment_t align;
} audio_converter_t;

bool audio_format_equal(const audio_format_t *a, const audio_format_t *b);
esp_err_t audio_format_complete(audio_format_t *fmt);
void audio_format_log(char *buf, size_t len, const audio_format_t *fmt);

void audio_converter_destroy(audio_converter_t *converter);
esp_err_t audio_converter_create(audio_converter_t *converter,
                                 const audio_format_t *src,
                                 const audio_format_t *dst);
esp_err_t audio_converter_process(audio_converter_t *converter,
                                  const uint8_t *in,
                                  uint32_t in_bytes,
                                  uint8_t **out,
                                  uint32_t *out_bytes);

void audio_codec_refresh_actual_format(esp_codec_dev_handle_t codec_dev,
                                       audio_format_t *fmt,
                                       const char *role);
esp_err_t audio_codec_open_output(esp_codec_dev_handle_t codec_dev,
                                  audio_format_t *fmt,
                                  int volume);

#ifdef __cplusplus
}
#endif
