/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_tts.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lauxlib.h"
#include "settings_store.h"
#include "tts_provider.h"

#define LUA_MODULE_TTS_NAME         "tts"
#define TTS_DEFAULT_PROVIDER        "xiao_mimo"
#define TTS_DEFAULT_DEVICE          "audio_dac"
#define TTS_DEFAULT_VOLUME          80
#define TTS_DEFAULT_TIMEOUT_MS      120000
#define TTS_CONFIG_STR_LEN          320
#define TTS_SHORT_STR_LEN           64
#define TTS_CHUNK_MS                60

static const char *TAG = "lua_tts";

typedef struct {
    char provider[TTS_SHORT_STR_LEN];
    char audio_device[TTS_SHORT_STR_LEN];
    char api_key[TTS_CONFIG_STR_LEN];
    char base_url[TTS_CONFIG_STR_LEN];
    char model[TTS_SHORT_STR_LEN];
    char voice[TTS_SHORT_STR_LEN];
    char style[TTS_CONFIG_STR_LEN];
    uint32_t timeout_ms;
    int volume;
    bool initialized;
    bool override_audio;
    esp_codec_dev_handle_t codec_dev;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t bytes_per_sample;
} lua_tts_runtime_t;

typedef struct {
    esp_codec_dev_handle_t codec_dev;
    tts_audio_format_t src;
    tts_audio_format_t dst;
    uint8_t dst_bytes_per_sample;
    uint8_t *in_buf;
    size_t in_len;
    size_t in_cap;
    uint8_t *out_buf;
    size_t out_cap;
    size_t audio_bytes_written;
} lua_tts_audio_sink_t;

static lua_tts_runtime_t s_tts;
static SemaphoreHandle_t s_tts_mutex;
static portMUX_TYPE s_tts_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void lua_tts_lock(void)
{
    portENTER_CRITICAL(&s_tts_mutex_init_mux);
    if (s_tts_mutex == NULL) {
        s_tts_mutex = xSemaphoreCreateMutex();
    }
    portEXIT_CRITICAL(&s_tts_mutex_init_mux);
    if (s_tts_mutex) {
        xSemaphoreTake(s_tts_mutex, portMAX_DELAY);
    }
}

static void lua_tts_unlock(void)
{
    if (s_tts_mutex) {
        xSemaphoreGive(s_tts_mutex);
    }
}

static int lua_tts_push_err(lua_State *L, esp_err_t err, const char *msg)
{
    lua_pushnil(L);
    if (msg) {
        lua_pushfstring(L, "%s: %s", msg, esp_err_to_name(err));
    } else {
        lua_pushstring(L, esp_err_to_name(err));
    }
    return 2;
}

static void lua_tts_read_setting(const char *key, char *dst, size_t dst_size)
{
    if (dst && dst_size > 0) {
        char fallback[TTS_CONFIG_STR_LEN] = {0};
        strlcpy(fallback, dst, sizeof(fallback));
        (void)settings_store_get_string(key, dst, dst_size, fallback);
    }
}

static void lua_tts_load_defaults(lua_tts_runtime_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->provider, TTS_DEFAULT_PROVIDER, sizeof(cfg->provider));
    strlcpy(cfg->audio_device, TTS_DEFAULT_DEVICE, sizeof(cfg->audio_device));
    cfg->timeout_ms = TTS_DEFAULT_TIMEOUT_MS;
    cfg->volume = TTS_DEFAULT_VOLUME;

    lua_tts_read_setting("tts_provider", cfg->provider, sizeof(cfg->provider));
    lua_tts_read_setting("tts_device", cfg->audio_device, sizeof(cfg->audio_device));
    lua_tts_read_setting("tts_api_key", cfg->api_key, sizeof(cfg->api_key));
    lua_tts_read_setting("tts_base_url", cfg->base_url, sizeof(cfg->base_url));
    lua_tts_read_setting("tts_model", cfg->model, sizeof(cfg->model));
    lua_tts_read_setting("tts_voice", cfg->voice, sizeof(cfg->voice));
    lua_tts_read_setting("tts_style", cfg->style, sizeof(cfg->style));
}

static esp_err_t lua_tts_copy_field(lua_State *L, int table_idx,
                                    const char *field,
                                    char *dst,
                                    size_t dst_size)
{
    esp_err_t err = ESP_OK;

    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        size_t len = 0;
        const char *value = NULL;
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return ESP_ERR_INVALID_ARG;
        }
        value = lua_tolstring(L, -1, &len);
        if (len >= dst_size) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(dst, value, len);
            dst[len] = '\0';
        }
    }
    lua_pop(L, 1);
    return err;
}

static esp_err_t lua_tts_u32_field(lua_State *L, int table_idx,
                                   const char *field,
                                   uint32_t *out)
{
    esp_err_t err = ESP_OK;

    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        lua_Integer value = 0;
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return ESP_ERR_INVALID_ARG;
        }
        value = lua_tointeger(L, -1);
        if (value <= 0 || value > UINT32_MAX) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            *out = (uint32_t)value;
        }
    }
    lua_pop(L, 1);
    return err;
}

static esp_err_t lua_tts_int_field(lua_State *L, int table_idx,
                                   const char *field,
                                   int *out,
                                   int min_value,
                                   int max_value)
{
    esp_err_t err = ESP_OK;

    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        lua_Integer value = 0;
        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return ESP_ERR_INVALID_ARG;
        }
        value = lua_tointeger(L, -1);
        if (value < min_value || value > max_value) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            *out = (int)value;
        }
    }
    lua_pop(L, 1);
    return err;
}

static esp_err_t lua_tts_apply_opts(lua_State *L, int opts_idx, lua_tts_runtime_t *cfg)
{
    if (opts_idx == 0 || lua_isnoneornil(L, opts_idx)) {
        return ESP_OK;
    }
    if (!lua_istable(L, opts_idx)) {
        return ESP_ERR_INVALID_ARG;
    }
    opts_idx = lua_absindex(L, opts_idx);

    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "provider", cfg->provider, sizeof(cfg->provider)),
                        TAG, "provider too long");
    lua_getfield(L, opts_idx, "audio_device");
    if (!lua_isnil(L, -1)) {
        cfg->override_audio = true;
    }
    lua_pop(L, 1);
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "audio_device", cfg->audio_device, sizeof(cfg->audio_device)),
                        TAG, "audio_device too long");
    lua_getfield(L, opts_idx, "device");
    if (!lua_isnil(L, -1)) {
        cfg->override_audio = true;
    }
    lua_pop(L, 1);
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "device", cfg->audio_device, sizeof(cfg->audio_device)),
                        TAG, "device too long");
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "api_key", cfg->api_key, sizeof(cfg->api_key)),
                        TAG, "api_key too long");
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "base_url", cfg->base_url, sizeof(cfg->base_url)),
                        TAG, "base_url too long");
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "model", cfg->model, sizeof(cfg->model)),
                        TAG, "model too long");
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "voice", cfg->voice, sizeof(cfg->voice)),
                        TAG, "voice too long");
    ESP_RETURN_ON_ERROR(lua_tts_copy_field(L, opts_idx, "style", cfg->style, sizeof(cfg->style)),
                        TAG, "style too long");
    ESP_RETURN_ON_ERROR(lua_tts_u32_field(L, opts_idx, "timeout_ms", &cfg->timeout_ms),
                        TAG, "invalid timeout_ms");
    lua_getfield(L, opts_idx, "volume");
    if (!lua_isnil(L, -1)) {
        cfg->override_audio = true;
    }
    lua_pop(L, 1);
    ESP_RETURN_ON_ERROR(lua_tts_int_field(L, opts_idx, "volume", &cfg->volume, 0, 100),
                        TAG, "invalid volume");
    return ESP_OK;
}

#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
static esp_err_t lua_tts_get_i2s_audio_format(const periph_i2s_config_t *i2s_cfg,
                                              uint32_t *sample_rate,
                                              uint8_t *channels,
                                              uint8_t *bits_per_sample)
{
    if (i2s_cfg == NULL || sample_rate == NULL || channels == NULL || bits_per_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (i2s_cfg->mode == I2S_COMM_MODE_STD) {
        *sample_rate = i2s_cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        *channels = (i2s_cfg->i2s_cfg.std.slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.std.slot_cfg.data_bit_width;
        return ESP_OK;
    }

#if CONFIG_SOC_I2S_SUPPORTS_TDM
    if (i2s_cfg->mode == I2S_COMM_MODE_TDM) {
        *sample_rate = i2s_cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        *channels = (uint8_t)i2s_cfg->i2s_cfg.tdm.slot_cfg.total_slot;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.tdm.slot_cfg.data_bit_width;
        return ESP_OK;
    }
#endif

#if CONFIG_SOC_I2S_SUPPORTS_PDM_TX
    if (i2s_cfg->mode == I2S_COMM_MODE_PDM && (i2s_cfg->direction & I2S_DIR_TX)) {
        *sample_rate = i2s_cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        *channels = (i2s_cfg->i2s_cfg.pdm_tx.slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.pdm_tx.slot_cfg.data_bit_width;
        return ESP_OK;
    }
#endif

    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static esp_err_t lua_tts_get_board_audio_output(lua_tts_runtime_t *cfg)
{
#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    void *handle = NULL;
    void *config = NULL;
    void *periph_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(cfg->audio_device, &handle);
    if (err != ESP_OK) {
        err = esp_board_manager_init_device_by_name(cfg->audio_device);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_board_manager_get_device_handle(cfg->audio_device, &handle);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = esp_board_manager_get_device_config(cfg->audio_device, &config);
    if (err != ESP_OK) {
        return err;
    }

    dev_audio_codec_handles_t *codec_handles = (dev_audio_codec_handles_t *)handle;
    dev_audio_codec_config_t *codec_cfg = (dev_audio_codec_config_t *)config;
    if (codec_handles == NULL || codec_cfg == NULL || codec_handles->codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!codec_cfg->dac_enabled || codec_cfg->i2s_cfg.name == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = esp_board_manager_get_periph_config(codec_cfg->i2s_cfg.name, &periph_config);
    if (err != ESP_OK) {
        return err;
    }

    err = lua_tts_get_i2s_audio_format((const periph_i2s_config_t *)periph_config,
                                       &cfg->sample_rate_hz,
                                       &cfg->channels,
                                       &cfg->bits_per_sample);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg->sample_rate_hz == 0 || cfg->channels == 0 ||
        (cfg->bits_per_sample != 16 && cfg->bits_per_sample != 32)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cfg->bytes_per_sample = (uint8_t)(cfg->bits_per_sample / 8);
    cfg->codec_dev = codec_handles->codec_dev;
    return ESP_OK;
#else
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t lua_tts_open_codec(lua_tts_runtime_t *cfg)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = cfg->sample_rate_hz,
        .channel = cfg->channels,
        .bits_per_sample = cfg->bits_per_sample,
    };

    int ret = esp_codec_dev_open(cfg->codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    ret = esp_codec_dev_set_out_vol(cfg->codec_dev, cfg->volume);
    if (ret != ESP_CODEC_DEV_OK && ret != ESP_CODEC_DEV_NOT_SUPPORT) {
        esp_codec_dev_close(cfg->codec_dev);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void lua_tts_runtime_close_locked(void)
{
    if (s_tts.initialized && s_tts.codec_dev) {
        esp_codec_dev_close(s_tts.codec_dev);
    }
    memset(&s_tts, 0, sizeof(s_tts));
}

static esp_err_t lua_tts_runtime_init_locked(lua_State *L, int opts_idx)
{
    lua_tts_runtime_t cfg = {0};
    const tts_provider_t *provider = NULL;
    esp_err_t err;

    lua_tts_load_defaults(&cfg);
    ESP_RETURN_ON_ERROR(lua_tts_apply_opts(L, opts_idx, &cfg), TAG, "invalid TTS options");

    provider = tts_provider_find(cfg.provider);
    if (!provider) {
        return ESP_ERR_NOT_FOUND;
    }

    err = lua_tts_get_board_audio_output(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    lua_tts_runtime_close_locked();
    err = lua_tts_open_codec(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    cfg.initialized = true;
    cfg.override_audio = false;
    s_tts = cfg;
    ESP_LOGI(TAG, "TTS initialized provider=%s device=%s output=%" PRIu32 "Hz/%uch/%ubit",
             s_tts.provider,
             s_tts.audio_device,
             s_tts.sample_rate_hz,
             s_tts.channels,
             s_tts.bits_per_sample);
    return ESP_OK;
}

static uint32_t lua_tts_chunk_bytes(uint32_t sample_rate_hz,
                                    uint8_t channels,
                                    uint8_t bytes_per_sample)
{
    return (uint32_t)(((uint64_t)sample_rate_hz * channels * bytes_per_sample * TTS_CHUNK_MS) / 1000U);
}

static int16_t lua_tts_read_i16le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void lua_tts_write_sample(uint8_t *dst, uint8_t channels, uint8_t bytes_per_sample, int16_t sample)
{
    for (uint8_t ch = 0; ch < channels; ch++) {
        if (bytes_per_sample == 2) {
            dst[0] = (uint8_t)(sample & 0xFF);
            dst[1] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);
            dst += 2;
        } else {
            int32_t sample32 = (int32_t)sample << 16;
            dst[0] = (uint8_t)(sample32 & 0xFF);
            dst[1] = (uint8_t)((sample32 >> 8) & 0xFF);
            dst[2] = (uint8_t)((sample32 >> 16) & 0xFF);
            dst[3] = (uint8_t)((sample32 >> 24) & 0xFF);
            dst += 4;
        }
    }
}

static esp_err_t lua_tts_convert_24k_mono_to_board(lua_tts_audio_sink_t *sink,
                                                   const uint8_t *pcm,
                                                   size_t pcm_len,
                                                   uint8_t **out,
                                                   size_t *out_len)
{
    size_t in_frames = pcm_len / 2;
    size_t out_frames = 0;
    uint8_t frame_bytes = sink->dst.channels * sink->dst_bytes_per_sample;

    if (pcm_len == 0 || (pcm_len % 2) != 0) {
        *out = NULL;
        *out_len = 0;
        return pcm_len == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    if (sink->dst.sample_rate_hz == sink->src.sample_rate_hz) {
        out_frames = in_frames;
    } else if (sink->dst.sample_rate_hz == sink->src.sample_rate_hz * 2) {
        out_frames = in_frames * 2;
    } else if (sink->src.sample_rate_hz == 24000 && sink->dst.sample_rate_hz == 16000) {
        out_frames = (in_frames * 2) / 3;
    } else {
        out_frames = (size_t)(((uint64_t)in_frames * sink->dst.sample_rate_hz) / sink->src.sample_rate_hz);
    }

    if (out_frames == 0) {
        *out = NULL;
        *out_len = 0;
        return ESP_OK;
    }
    if (out_frames * frame_bytes > sink->out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t j = 0; j < out_frames; j++) {
        size_t src_index;
        int16_t sample;

        if (sink->dst.sample_rate_hz == sink->src.sample_rate_hz) {
            src_index = j;
        } else if (sink->dst.sample_rate_hz == sink->src.sample_rate_hz * 2) {
            src_index = j / 2;
        } else if (sink->src.sample_rate_hz == 24000 && sink->dst.sample_rate_hz == 16000) {
            src_index = (j * 3) / 2;
        } else {
            src_index = (size_t)(((uint64_t)j * sink->src.sample_rate_hz) / sink->dst.sample_rate_hz);
        }
        if (src_index >= in_frames) {
            src_index = in_frames - 1;
        }
        sample = lua_tts_read_i16le(pcm + src_index * 2);
        lua_tts_write_sample(sink->out_buf + j * frame_bytes,
                             sink->dst.channels,
                             sink->dst_bytes_per_sample,
                             sample);
    }

    *out = sink->out_buf;
    *out_len = out_frames * frame_bytes;
    return ESP_OK;
}

static esp_err_t lua_tts_sink_flush(lua_tts_audio_sink_t *sink)
{
    uint8_t *out = NULL;
    size_t out_len = 0;

    if (sink->in_len == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(lua_tts_convert_24k_mono_to_board(sink, sink->in_buf, sink->in_len, &out, &out_len),
                        TAG, "failed to convert TTS PCM");
    sink->in_len = 0;
    if (out_len == 0) {
        return ESP_OK;
    }
    if (esp_codec_dev_write(sink->codec_dev, out, (int)out_len) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    sink->audio_bytes_written += out_len;
    return ESP_OK;
}

static esp_err_t lua_tts_sink_write(void *ctx, const uint8_t *pcm, size_t len)
{
    lua_tts_audio_sink_t *sink = (lua_tts_audio_sink_t *)ctx;
    size_t offset = 0;

    if (!sink || (!pcm && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (offset < len) {
        size_t room = sink->in_cap - sink->in_len;
        size_t copy = len - offset;
        if (copy > room) {
            copy = room;
        }

        memcpy(sink->in_buf + sink->in_len, pcm + offset, copy);
        sink->in_len += copy;
        offset += copy;

        if (sink->in_len == sink->in_cap) {
            ESP_RETURN_ON_ERROR(lua_tts_sink_flush(sink), TAG, "failed to write TTS PCM");
        }
    }

    return ESP_OK;
}

static esp_err_t lua_tts_sink_init(lua_tts_audio_sink_t *sink,
                                   const lua_tts_runtime_t *runtime,
                                   const tts_audio_format_t *src)
{
    uint32_t in_cap = 0;
    uint32_t out_cap = 0;

    if (!sink || !runtime || !src || src->sample_rate_hz != 24000 ||
        src->channels != 1 || src->bits_per_sample != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(sink, 0, sizeof(*sink));
    sink->codec_dev = runtime->codec_dev;
    sink->src = *src;
    sink->dst.sample_rate_hz = runtime->sample_rate_hz;
    sink->dst.channels = runtime->channels;
    sink->dst.bits_per_sample = runtime->bits_per_sample;
    sink->dst_bytes_per_sample = runtime->bytes_per_sample;

    in_cap = lua_tts_chunk_bytes(src->sample_rate_hz, src->channels, src->bits_per_sample / 8);
    out_cap = lua_tts_chunk_bytes(runtime->sample_rate_hz, runtime->channels, runtime->bytes_per_sample);
    if (in_cap == 0 || out_cap == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    sink->in_buf = malloc(in_cap);
    sink->out_buf = malloc(out_cap);
    if (!sink->in_buf || !sink->out_buf) {
        free(sink->in_buf);
        free(sink->out_buf);
        memset(sink, 0, sizeof(*sink));
        return ESP_ERR_NO_MEM;
    }
    sink->in_cap = in_cap;
    sink->out_cap = out_cap;
    return ESP_OK;
}

static void lua_tts_sink_deinit(lua_tts_audio_sink_t *sink)
{
    if (!sink) {
        return;
    }
    free(sink->in_buf);
    free(sink->out_buf);
    memset(sink, 0, sizeof(*sink));
}

static esp_err_t lua_tts_build_effective_config(lua_State *L,
                                                int opts_idx,
                                                lua_tts_runtime_t *cfg)
{
    *cfg = s_tts;
    cfg->override_audio = false;
    return lua_tts_apply_opts(L, opts_idx, cfg);
}

static int lua_tts_init(lua_State *L)
{
    esp_err_t err;

    lua_tts_lock();
    err = lua_tts_runtime_init_locked(L, lua_isnoneornil(L, 1) ? 0 : 1);
    lua_tts_unlock();

    if (err != ESP_OK) {
        return lua_tts_push_err(L, err, "tts init");
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_tts_close(lua_State *L)
{
    (void)L;
    lua_tts_lock();
    lua_tts_runtime_close_locked();
    lua_tts_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_tts_play(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    int opts_idx = lua_isnoneornil(L, 2) ? 0 : 2;
    lua_tts_runtime_t cfg = {0};
    const tts_provider_t *provider = NULL;
    lua_tts_audio_sink_t sink = {0};
    tts_provider_stream_t stream = {0};
    esp_err_t err;
    bool was_initialized;

    if (text[0] == '\0') {
        return lua_tts_push_err(L, ESP_ERR_INVALID_ARG, "tts play: text is empty");
    }

    lua_tts_lock();
    was_initialized = s_tts.initialized;
    if (!s_tts.initialized) {
        err = lua_tts_runtime_init_locked(L, opts_idx);
        if (err != ESP_OK) {
            lua_tts_unlock();
            return lua_tts_push_err(L, err, "tts init");
        }
    }

    err = lua_tts_build_effective_config(L, opts_idx, &cfg);
    if (err != ESP_OK) {
        lua_tts_unlock();
        return lua_tts_push_err(L, err, "tts play options");
    }
    if (cfg.override_audio && was_initialized) {
        lua_tts_unlock();
        return lua_tts_push_err(L, ESP_ERR_INVALID_ARG,
                                "tts play: use tts.init() to change audio_device or volume");
    }
    if (cfg.api_key[0] == '\0') {
        lua_tts_unlock();
        return lua_tts_push_err(L, ESP_ERR_INVALID_ARG, "tts play: api_key is required");
    }

    provider = tts_provider_find(cfg.provider);
    if (!provider) {
        lua_tts_unlock();
        return lua_tts_push_err(L, ESP_ERR_NOT_FOUND, "tts play provider");
    }

    err = lua_tts_sink_init(&sink, &s_tts, provider->audio_format);
    if (err != ESP_OK) {
        lua_tts_unlock();
        return lua_tts_push_err(L, err, "tts audio sink");
    }

    tts_provider_config_t provider_cfg = {
        .api_key = cfg.api_key,
        .base_url = cfg.base_url,
        .model = cfg.model,
        .voice = cfg.voice,
        .style = cfg.style,
        .timeout_ms = cfg.timeout_ms,
    };
    stream.write_pcm = lua_tts_sink_write;
    stream.write_ctx = &sink;

    err = provider->play(&provider_cfg, text, &stream);
    if (err == ESP_OK) {
        err = lua_tts_sink_flush(&sink);
    }

    size_t audio_bytes = sink.audio_bytes_written;
    size_t http_bytes = stream.http_bytes;
    lua_tts_sink_deinit(&sink);
    lua_tts_unlock();

    if (err != ESP_OK) {
        return lua_tts_push_err(L, err, "tts play");
    }

    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, (lua_Integer)audio_bytes);
    lua_setfield(L, -2, "audio_bytes");
    lua_pushinteger(L, (lua_Integer)http_bytes);
    lua_setfield(L, -2, "http_bytes");
    return 1;
}

static int lua_tts_status(lua_State *L)
{
    lua_tts_lock();
    lua_newtable(L);
    lua_pushboolean(L, s_tts.initialized);
    lua_setfield(L, -2, "initialized");
    lua_pushstring(L, s_tts.provider);
    lua_setfield(L, -2, "provider");
    lua_pushstring(L, s_tts.audio_device);
    lua_setfield(L, -2, "audio_device");
    lua_pushinteger(L, (lua_Integer)s_tts.sample_rate_hz);
    lua_setfield(L, -2, "sample_rate_hz");
    lua_pushinteger(L, (lua_Integer)s_tts.channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, (lua_Integer)s_tts.bits_per_sample);
    lua_setfield(L, -2, "bits_per_sample");
    lua_pushinteger(L, (lua_Integer)s_tts.volume);
    lua_setfield(L, -2, "volume");
    lua_tts_unlock();
    return 1;
}

static esp_err_t lua_tts_save_string_opt(lua_State *L, int table_idx,
                                         const char *field,
                                         const char *key)
{
    lua_getfield(L, table_idx, field);
    if (!lua_isnil(L, -1)) {
        esp_err_t err;
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return ESP_ERR_INVALID_ARG;
        }
        err = settings_store_set_string(key, lua_tostring(L, -1));
        lua_pop(L, 1);
        return err;
    }
    lua_pop(L, 1);
    return ESP_OK;
}

static int lua_tts_configure(lua_State *L)
{
    esp_err_t err;
    int opts_idx = 1;

    if (!lua_istable(L, opts_idx)) {
        return lua_tts_push_err(L, ESP_ERR_INVALID_ARG, "tts configure: opts table required");
    }

    opts_idx = lua_absindex(L, opts_idx);
    lua_tts_lock();
    err = lua_tts_save_string_opt(L, opts_idx, "provider", "tts_provider");
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "audio_device", "tts_device");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "device", "tts_device");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "api_key", "tts_api_key");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "base_url", "tts_base_url");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "model", "tts_model");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "voice", "tts_voice");
    }
    if (err == ESP_OK) {
        err = lua_tts_save_string_opt(L, opts_idx, "style", "tts_style");
    }
    lua_tts_unlock();

    if (err != ESP_OK) {
        return lua_tts_push_err(L, err, "tts configure");
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_tts(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"init",      lua_tts_init},
        {"play",      lua_tts_play},
        {"close",     lua_tts_close},
        {"status",    lua_tts_status},
        {"configure", lua_tts_configure},
        {"tts_init",  lua_tts_init},
        {"tts_play",  lua_tts_play},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_tts_register(void)
{
    return cap_lua_register_module(LUA_MODULE_TTS_NAME, luaopen_tts);
}
