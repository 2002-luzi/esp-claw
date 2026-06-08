/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_tts.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "lua_audio_common.h"
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
    audio_format_t output_fmt;
} lua_tts_runtime_t;

typedef struct {
    esp_codec_dev_handle_t codec_dev;
    audio_converter_t converter;
    audio_format_t src;
    uint8_t *in_buf;
    size_t in_len;
    size_t in_cap;
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

static void lua_tts_read_u32_setting(const char *key, uint32_t *out)
{
    char value[TTS_SHORT_STR_LEN] = {0};
    char fallback[TTS_SHORT_STR_LEN] = {0};
    unsigned long parsed;

    if (!key || !out) {
        return;
    }

    snprintf(fallback, sizeof(fallback), "%" PRIu32, *out);
    if (settings_store_get_string(key, value, sizeof(value), fallback) != ESP_OK || value[0] == '\0') {
        return;
    }

    parsed = strtoul(value, NULL, 10);
    if (parsed > 0 && parsed <= UINT32_MAX) {
        *out = (uint32_t)parsed;
    }
}

static void lua_tts_read_int_setting(const char *key, int *out, int min_value, int max_value)
{
    char value[TTS_SHORT_STR_LEN] = {0};
    char fallback[TTS_SHORT_STR_LEN] = {0};
    long parsed;

    if (!key || !out) {
        return;
    }

    snprintf(fallback, sizeof(fallback), "%d", *out);
    if (settings_store_get_string(key, value, sizeof(value), fallback) != ESP_OK || value[0] == '\0') {
        return;
    }

    parsed = strtol(value, NULL, 10);
    if (parsed >= min_value && parsed <= max_value) {
        *out = (int)parsed;
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
    lua_tts_read_setting("tts_api_key", cfg->api_key, sizeof(cfg->api_key));
    lua_tts_read_setting("tts_base_url", cfg->base_url, sizeof(cfg->base_url));
    lua_tts_read_setting("tts_model", cfg->model, sizeof(cfg->model));
    lua_tts_read_setting("tts_voice", cfg->voice, sizeof(cfg->voice));
    lua_tts_read_u32_setting("tts_timeout_ms", &cfg->timeout_ms);
}

static bool lua_tts_has_field(lua_State *L, int table_idx, const char *field)
{
    bool found;

    lua_getfield(L, table_idx, field);
    found = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return found;
}

static const char *lua_tts_find_config_field(lua_State *L, int table_idx)
{
    static const char *const fields[] = {
        "provider",
        "api_key",
        "base_url",
        "model",
        "voice",
        "appid",
        "app_id",
        "access_token",
        "token",
        "cluster",
        "speaker",
        "resource_id",
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (lua_tts_has_field(L, table_idx, fields[i])) {
            return fields[i];
        }
    }

    return NULL;
}

static esp_err_t lua_tts_reject_config_fields(lua_State *L, int table_idx)
{
    const char *field = lua_tts_find_config_field(L, table_idx);

    if (field) {
        ESP_LOGE(TAG, "TTS option '%s' must be configured in device settings", field);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static int lua_tts_push_config_field_err(lua_State *L, int opts_idx, const char *prefix)
{
    const char *field = NULL;

    if (opts_idx == 0 || lua_isnoneornil(L, opts_idx)) {
        return 0;
    }
    if (!lua_istable(L, opts_idx)) {
        return 0;
    }

    opts_idx = lua_absindex(L, opts_idx);
    field = lua_tts_find_config_field(L, opts_idx);
    if (!field) {
        return 0;
    }

    lua_pushnil(L);
    lua_pushfstring(L,
                    "%s: option '%s' must be configured in device settings",
                    prefix ? prefix : "tts",
                    field);
    return 2;
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

    ESP_RETURN_ON_ERROR(lua_tts_reject_config_fields(L, opts_idx),
                        TAG, "TTS provider options must be configured in device settings");
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
                                       &cfg->output_fmt.sample_rate,
                                       &cfg->output_fmt.channels,
                                       &cfg->output_fmt.bits);
    if (err != ESP_OK) {
        return err;
    }
    if (audio_format_complete(&cfg->output_fmt) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cfg->codec_dev = codec_handles->codec_dev;
    return ESP_OK;
#else
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t lua_tts_open_codec(lua_tts_runtime_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_codec_open_output(cfg->codec_dev, &cfg->output_fmt, cfg->volume);
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
             s_tts.output_fmt.sample_rate,
             s_tts.output_fmt.channels,
             s_tts.output_fmt.bits);
    return ESP_OK;
}

static uint32_t lua_tts_chunk_bytes(uint32_t sample_rate_hz,
                                    uint8_t bytes_per_frame)
{
    return (uint32_t)(((uint64_t)sample_rate_hz * bytes_per_frame * TTS_CHUNK_MS) / 1000U);
}

static esp_err_t lua_tts_sink_flush(lua_tts_audio_sink_t *sink)
{
    uint8_t *out = NULL;
    uint32_t out_len = 0;

    if (sink->in_len == 0) {
        return ESP_OK;
    }
    if (sink->in_len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(audio_converter_process(&sink->converter, sink->in_buf, (uint32_t)sink->in_len, &out, &out_len),
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
    esp_err_t err;

    if (!sink || !runtime || !src) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!runtime->codec_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(sink, 0, sizeof(*sink));
    sink->codec_dev = runtime->codec_dev;
    sink->src.sample_rate = src->sample_rate_hz;
    sink->src.channels = src->channels;
    sink->src.bits = src->bits_per_sample;
    if (audio_format_complete(&sink->src) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    in_cap = lua_tts_chunk_bytes(sink->src.sample_rate, sink->src.bytes_per_frame);
    if (in_cap == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    sink->in_buf = malloc(in_cap);
    if (!sink->in_buf) {
        memset(sink, 0, sizeof(*sink));
        return ESP_ERR_NO_MEM;
    }
    err = audio_converter_create(&sink->converter, &sink->src, &runtime->output_fmt);
    if (err != ESP_OK) {
        free(sink->in_buf);
        memset(sink, 0, sizeof(*sink));
        return err;
    }
    sink->in_cap = in_cap;
    return ESP_OK;
}

static void lua_tts_sink_deinit(lua_tts_audio_sink_t *sink)
{
    if (!sink) {
        return;
    }
    free(sink->in_buf);
    audio_converter_destroy(&sink->converter);
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
    int opts_idx = lua_isnoneornil(L, 1) ? 0 : 1;
    int field_err = lua_tts_push_config_field_err(L, opts_idx, "tts init");

    if (field_err) {
        return field_err;
    }

    lua_tts_lock();
    err = lua_tts_runtime_init_locked(L, opts_idx);
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
    int field_err;

    if (text[0] == '\0') {
        return lua_tts_push_err(L, ESP_ERR_INVALID_ARG, "tts play: text is empty");
    }
    field_err = lua_tts_push_config_field_err(L, opts_idx, "tts play");
    if (field_err) {
        return field_err;
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
    lua_pushinteger(L, (lua_Integer)s_tts.output_fmt.sample_rate);
    lua_setfield(L, -2, "sample_rate_hz");
    lua_pushinteger(L, (lua_Integer)s_tts.output_fmt.channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, (lua_Integer)s_tts.output_fmt.bits);
    lua_setfield(L, -2, "bits_per_sample");
    lua_pushinteger(L, (lua_Integer)s_tts.volume);
    lua_setfield(L, -2, "volume");
    lua_tts_unlock();
    return 1;
}

int luaopen_tts(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"init",      lua_tts_init},
        {"play",      lua_tts_play},
        {"close",     lua_tts_close},
        {"status",    lua_tts_status},
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
