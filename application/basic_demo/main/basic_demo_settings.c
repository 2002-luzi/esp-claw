/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "basic_demo_settings.h"

#include <string.h>
#include "nvs.h"

static const char *BASIC_DEMO_SETTINGS_NAMESPACE = "basic_demo";

typedef struct {
    const char *key;
    const char *default_value;
    char *buffer;
    size_t buffer_size;
} basic_demo_settings_field_t;

static void settings_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strlcpy(dst, src, dst_size);
}

static void settings_load_defaults(basic_demo_settings_t *settings)
{
    settings_safe_copy(settings->wifi_ssid, sizeof(settings->wifi_ssid), BASIC_DEMO_WIFI_SSID);
    settings_safe_copy(settings->wifi_password, sizeof(settings->wifi_password), BASIC_DEMO_WIFI_PASSWORD);
    settings_safe_copy(settings->llm_api_key, sizeof(settings->llm_api_key), BASIC_DEMO_LLM_API_KEY);
    settings_safe_copy(settings->llm_backend_type, sizeof(settings->llm_backend_type), BASIC_DEMO_LLM_BACKEND_TYPE);
    settings_safe_copy(settings->llm_profile, sizeof(settings->llm_profile), BASIC_DEMO_LLM_PROFILE);
    settings_safe_copy(settings->llm_model, sizeof(settings->llm_model), BASIC_DEMO_LLM_MODEL);
    settings_safe_copy(settings->llm_base_url, sizeof(settings->llm_base_url), BASIC_DEMO_LLM_BASE_URL);
    settings_safe_copy(settings->llm_auth_type, sizeof(settings->llm_auth_type), BASIC_DEMO_LLM_AUTH_TYPE);
    settings_safe_copy(settings->llm_timeout_ms, sizeof(settings->llm_timeout_ms), BASIC_DEMO_LLM_TIMEOUT_MS);
    settings_safe_copy(settings->qq_app_id, sizeof(settings->qq_app_id), BASIC_DEMO_QQ_APP_ID);
    settings_safe_copy(settings->qq_app_secret, sizeof(settings->qq_app_secret), BASIC_DEMO_QQ_APP_SECRET);
    settings_safe_copy(settings->tg_bot_token, sizeof(settings->tg_bot_token), BASIC_DEMO_TG_BOT_TOKEN);
    settings_safe_copy(settings->wechat_token, sizeof(settings->wechat_token), BASIC_DEMO_WECHAT_TOKEN);
    settings_safe_copy(settings->wechat_base_url, sizeof(settings->wechat_base_url), BASIC_DEMO_WECHAT_BASE_URL);
    settings_safe_copy(settings->wechat_cdn_base_url, sizeof(settings->wechat_cdn_base_url), BASIC_DEMO_WECHAT_CDN_BASE_URL);
    settings_safe_copy(settings->wechat_account_id, sizeof(settings->wechat_account_id), BASIC_DEMO_WECHAT_ACCOUNT_ID);
    settings_safe_copy(settings->search_brave_key, sizeof(settings->search_brave_key), BASIC_DEMO_SEARCH_BRAVE_KEY);
    settings_safe_copy(settings->search_tavily_key, sizeof(settings->search_tavily_key), BASIC_DEMO_SEARCH_TAVILY_KEY);
    settings_safe_copy(settings->time_timezone, sizeof(settings->time_timezone), BASIC_DEMO_TIME_TIMEZONE);
    settings_safe_copy(settings->tts_xfyun_app_id, sizeof(settings->tts_xfyun_app_id), BASIC_DEMO_TTS_XFYUN_APP_ID);
    settings_safe_copy(settings->tts_xfyun_api_key, sizeof(settings->tts_xfyun_api_key), BASIC_DEMO_TTS_XFYUN_API_KEY);
    settings_safe_copy(settings->tts_xfyun_api_secret, sizeof(settings->tts_xfyun_api_secret), BASIC_DEMO_TTS_XFYUN_API_SECRET);
    settings_safe_copy(settings->tts_xfyun_voice_name, sizeof(settings->tts_xfyun_voice_name), BASIC_DEMO_TTS_XFYUN_VOICE_NAME);
    settings_safe_copy(settings->tts_xfyun_audio_encoding, sizeof(settings->tts_xfyun_audio_encoding), BASIC_DEMO_TTS_XFYUN_AUDIO_ENCODING);
    settings_safe_copy(settings->tts_xfyun_audio_format, sizeof(settings->tts_xfyun_audio_format), BASIC_DEMO_TTS_XFYUN_AUDIO_FORMAT);
    settings_safe_copy(settings->tts_xfyun_text_encoding, sizeof(settings->tts_xfyun_text_encoding), BASIC_DEMO_TTS_XFYUN_TEXT_ENCODING);
    settings_safe_copy(settings->tts_xfyun_websocket_uri, sizeof(settings->tts_xfyun_websocket_uri), BASIC_DEMO_TTS_XFYUN_WEBSOCKET_URI);
    settings_safe_copy(settings->tts_xfyun_auth_host, sizeof(settings->tts_xfyun_auth_host), BASIC_DEMO_TTS_XFYUN_AUTH_HOST);
    settings_safe_copy(settings->tts_timeout_ms, sizeof(settings->tts_timeout_ms), BASIC_DEMO_TTS_TIMEOUT_MS);
}

static esp_err_t settings_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(BASIC_DEMO_SETTINGS_NAMESPACE, mode, handle);
}

esp_err_t basic_demo_settings_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = settings_open(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = settings_open(NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            nvs_close(handle);
        }
    }

    return err;
}

esp_err_t basic_demo_settings_load(basic_demo_settings_t *settings)
{
    char legacy_provider[32] = {0};

    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(settings, 0, sizeof(*settings));
    settings_load_defaults(settings);

    nvs_handle_t handle;
    esp_err_t err = settings_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    basic_demo_settings_field_t fields[] = {
        { "wifi_ssid", settings->wifi_ssid, settings->wifi_ssid, sizeof(settings->wifi_ssid) },
        { "wifi_password", settings->wifi_password, settings->wifi_password, sizeof(settings->wifi_password) },
        { "llm_api_key", settings->llm_api_key, settings->llm_api_key, sizeof(settings->llm_api_key) },
        { "llm_backend_type", settings->llm_backend_type, settings->llm_backend_type, sizeof(settings->llm_backend_type) },
        { "llm_profile", settings->llm_profile, settings->llm_profile, sizeof(settings->llm_profile) },
        { "llm_model", settings->llm_model, settings->llm_model, sizeof(settings->llm_model) },
        { "llm_base_url", settings->llm_base_url, settings->llm_base_url, sizeof(settings->llm_base_url) },
        { "llm_auth_type", settings->llm_auth_type, settings->llm_auth_type, sizeof(settings->llm_auth_type) },
        { "llm_timeout_ms", settings->llm_timeout_ms, settings->llm_timeout_ms, sizeof(settings->llm_timeout_ms) },
        { "qq_app_id", settings->qq_app_id, settings->qq_app_id, sizeof(settings->qq_app_id) },
        { "qq_app_secret", settings->qq_app_secret, settings->qq_app_secret, sizeof(settings->qq_app_secret) },
        { "tg_bot_token", settings->tg_bot_token, settings->tg_bot_token, sizeof(settings->tg_bot_token) },
        { "wechat_token", settings->wechat_token, settings->wechat_token, sizeof(settings->wechat_token) },
        { "wechat_base_url", settings->wechat_base_url, settings->wechat_base_url, sizeof(settings->wechat_base_url) },
        { "wechat_cdn_base_url", settings->wechat_cdn_base_url, settings->wechat_cdn_base_url, sizeof(settings->wechat_cdn_base_url) },
        { "wechat_account_id", settings->wechat_account_id, settings->wechat_account_id, sizeof(settings->wechat_account_id) },
        { "search_brave_key", settings->search_brave_key, settings->search_brave_key, sizeof(settings->search_brave_key) },
        { "search_tavily_key", settings->search_tavily_key, settings->search_tavily_key, sizeof(settings->search_tavily_key) },
        { "time_timezone", settings->time_timezone, settings->time_timezone, sizeof(settings->time_timezone) },
        { "tts_xfyun_app_id", settings->tts_xfyun_app_id, settings->tts_xfyun_app_id, sizeof(settings->tts_xfyun_app_id) },
        { "tts_xfyun_api_key", settings->tts_xfyun_api_key, settings->tts_xfyun_api_key, sizeof(settings->tts_xfyun_api_key) },
        { "tts_xfyun_api_secret", settings->tts_xfyun_api_secret, settings->tts_xfyun_api_secret, sizeof(settings->tts_xfyun_api_secret) },
        { "tts_xfyun_voice_name", settings->tts_xfyun_voice_name, settings->tts_xfyun_voice_name, sizeof(settings->tts_xfyun_voice_name) },
        { "tts_xfyun_audio_encoding", settings->tts_xfyun_audio_encoding, settings->tts_xfyun_audio_encoding, sizeof(settings->tts_xfyun_audio_encoding) },
        { "tts_xfyun_audio_format", settings->tts_xfyun_audio_format, settings->tts_xfyun_audio_format, sizeof(settings->tts_xfyun_audio_format) },
        { "tts_xfyun_text_encoding", settings->tts_xfyun_text_encoding, settings->tts_xfyun_text_encoding, sizeof(settings->tts_xfyun_text_encoding) },
        { "tts_xfyun_websocket_uri", settings->tts_xfyun_websocket_uri, settings->tts_xfyun_websocket_uri, sizeof(settings->tts_xfyun_websocket_uri) },
        { "tts_xfyun_auth_host", settings->tts_xfyun_auth_host, settings->tts_xfyun_auth_host, sizeof(settings->tts_xfyun_auth_host) },
        { "tts_timeout_ms", settings->tts_timeout_ms, settings->tts_timeout_ms, sizeof(settings->tts_timeout_ms) },
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        size_t required_size = fields[i].buffer_size;
        if (nvs_get_str(handle, fields[i].key, fields[i].buffer, &required_size) != ESP_OK) {
            settings_safe_copy(fields[i].buffer, fields[i].buffer_size, fields[i].default_value);
        }
    }

    {
        size_t required_size = sizeof(legacy_provider);
        if (nvs_get_str(handle, "llm_provider", legacy_provider, &required_size) == ESP_OK &&
                settings->llm_profile[0] == '\0') {
            if (strcmp(legacy_provider, "qwen") == 0) {
                settings_safe_copy(settings->llm_profile, sizeof(settings->llm_profile), "qwen_compatible");
            } else if (strcmp(legacy_provider, "openai") == 0) {
                settings_safe_copy(settings->llm_profile, sizeof(settings->llm_profile), "openai");
            } else {
                settings_safe_copy(settings->llm_profile, sizeof(settings->llm_profile), legacy_provider);
            }
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t basic_demo_settings_save(const basic_demo_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    basic_demo_settings_field_t fields[] = {
        { "wifi_ssid", NULL, (char *)settings->wifi_ssid, sizeof(settings->wifi_ssid) },
        { "wifi_password", NULL, (char *)settings->wifi_password, sizeof(settings->wifi_password) },
        { "llm_api_key", NULL, (char *)settings->llm_api_key, sizeof(settings->llm_api_key) },
        { "llm_backend_type", NULL, (char *)settings->llm_backend_type, sizeof(settings->llm_backend_type) },
        { "llm_profile", NULL, (char *)settings->llm_profile, sizeof(settings->llm_profile) },
        { "llm_model", NULL, (char *)settings->llm_model, sizeof(settings->llm_model) },
        { "llm_base_url", NULL, (char *)settings->llm_base_url, sizeof(settings->llm_base_url) },
        { "llm_auth_type", NULL, (char *)settings->llm_auth_type, sizeof(settings->llm_auth_type) },
        { "llm_timeout_ms", NULL, (char *)settings->llm_timeout_ms, sizeof(settings->llm_timeout_ms) },
        { "qq_app_id", NULL, (char *)settings->qq_app_id, sizeof(settings->qq_app_id) },
        { "qq_app_secret", NULL, (char *)settings->qq_app_secret, sizeof(settings->qq_app_secret) },
        { "tg_bot_token", NULL, (char *)settings->tg_bot_token, sizeof(settings->tg_bot_token) },
        { "wechat_token", NULL, (char *)settings->wechat_token, sizeof(settings->wechat_token) },
        { "wechat_base_url", NULL, (char *)settings->wechat_base_url, sizeof(settings->wechat_base_url) },
        { "wechat_cdn_base_url", NULL, (char *)settings->wechat_cdn_base_url, sizeof(settings->wechat_cdn_base_url) },
        { "wechat_account_id", NULL, (char *)settings->wechat_account_id, sizeof(settings->wechat_account_id) },
        { "search_brave_key", NULL, (char *)settings->search_brave_key, sizeof(settings->search_brave_key) },
        { "search_tavily_key", NULL, (char *)settings->search_tavily_key, sizeof(settings->search_tavily_key) },
        { "time_timezone", NULL, (char *)settings->time_timezone, sizeof(settings->time_timezone) },
        { "tts_xfyun_app_id", NULL, (char *)settings->tts_xfyun_app_id, sizeof(settings->tts_xfyun_app_id) },
        { "tts_xfyun_api_key", NULL, (char *)settings->tts_xfyun_api_key, sizeof(settings->tts_xfyun_api_key) },
        { "tts_xfyun_api_secret", NULL, (char *)settings->tts_xfyun_api_secret, sizeof(settings->tts_xfyun_api_secret) },
        { "tts_xfyun_voice_name", NULL, (char *)settings->tts_xfyun_voice_name, sizeof(settings->tts_xfyun_voice_name) },
        { "tts_xfyun_audio_encoding", NULL, (char *)settings->tts_xfyun_audio_encoding, sizeof(settings->tts_xfyun_audio_encoding) },
        { "tts_xfyun_audio_format", NULL, (char *)settings->tts_xfyun_audio_format, sizeof(settings->tts_xfyun_audio_format) },
        { "tts_xfyun_text_encoding", NULL, (char *)settings->tts_xfyun_text_encoding, sizeof(settings->tts_xfyun_text_encoding) },
        { "tts_xfyun_websocket_uri", NULL, (char *)settings->tts_xfyun_websocket_uri, sizeof(settings->tts_xfyun_websocket_uri) },
        { "tts_xfyun_auth_host", NULL, (char *)settings->tts_xfyun_auth_host, sizeof(settings->tts_xfyun_auth_host) },
        { "tts_timeout_ms", NULL, (char *)settings->tts_timeout_ms, sizeof(settings->tts_timeout_ms) },
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        err = nvs_set_str(handle, fields[i].key, fields[i].buffer);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
