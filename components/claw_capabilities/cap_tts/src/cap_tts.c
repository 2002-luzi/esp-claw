#include "cap_tts.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_cap.h"
#include "cJSON.h"
#include "esp_log.h"
#include "tts_engine.h"
#include "tts_engine_provider_xfyun.h"

static const char *TAG = "cap_tts";
static tts_engine_speaker_t s_tts_speaker = {0};

#define CAP_TTS_SPEAK_SCHEMA \
    "{" \
    "\"type\":\"object\"," \
    "\"properties\":{" \
    "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":256}," \
    "\"source\":{\"type\":\"string\",\"maxLength\":15}" \
    "}," \
    "\"required\":[\"text\"]," \
    "\"additionalProperties\":false" \
    "}"

#define CAP_TTS_CLEAR_SCHEMA "{\"type\":\"object\",\"additionalProperties\":false}"

#define CAP_TTS_STATUS_SCHEMA "{\"type\":\"object\",\"additionalProperties\":false}"

static bool cap_tts_string_is_empty(const char *value)
{
    return value == NULL || value[0] == '\0';
}

esp_err_t cap_tts_init_runtime(const cap_tts_runtime_config_t *config)
{
    tts_engine_provider_xfyun_config_t provider_config = {0};
    tts_engine_config_t runtime_config = {0};
    const tts_engine_speaker_t *speaker = NULL;
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cap_tts_string_is_empty(config->xfyun_app_id) ||
        cap_tts_string_is_empty(config->xfyun_api_key) ||
        cap_tts_string_is_empty(config->xfyun_api_secret) ||
        cap_tts_string_is_empty(config->xfyun_voice_name) ||
        cap_tts_string_is_empty(config->xfyun_audio_encoding) ||
        cap_tts_string_is_empty(config->xfyun_audio_format) ||
        cap_tts_string_is_empty(config->xfyun_text_encoding) ||
        cap_tts_string_is_empty(config->xfyun_websocket_uri) ||
        cap_tts_string_is_empty(config->xfyun_auth_host)) {
        ESP_LOGE(TAG, "Missing required XFYun TTS configuration");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_tts_speaker, 0, sizeof(s_tts_speaker));

    if (config->enable_speaker_output) {
        if (config->speaker_init == NULL) {
            ESP_LOGE(TAG, "Speaker output enabled but no speaker_init callback provided");
            return ESP_ERR_INVALID_ARG;
        }

        err = config->speaker_init(&s_tts_speaker);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize TTS speaker: %s", esp_err_to_name(err));
            return err;
        }

        speaker = &s_tts_speaker;
        ESP_LOGI(TAG, "TTS PCM playback enabled");
    } else {
        ESP_LOGW(TAG, "TTS PCM playback disabled by config");
    }

    provider_config.timeout_ms = config->timeout_ms ? config->timeout_ms : 30000;
    strlcpy(provider_config.app_id, config->xfyun_app_id, sizeof(provider_config.app_id));
    strlcpy(provider_config.api_key, config->xfyun_api_key, sizeof(provider_config.api_key));
    strlcpy(provider_config.api_secret,
            config->xfyun_api_secret,
            sizeof(provider_config.api_secret));
    strlcpy(provider_config.voice_name,
            config->xfyun_voice_name,
            sizeof(provider_config.voice_name));
    strlcpy(provider_config.audio_encoding,
            config->xfyun_audio_encoding,
            sizeof(provider_config.audio_encoding));
    strlcpy(provider_config.audio_format,
            config->xfyun_audio_format,
            sizeof(provider_config.audio_format));
    strlcpy(provider_config.text_encoding,
            config->xfyun_text_encoding,
            sizeof(provider_config.text_encoding));
    strlcpy(provider_config.websocket_uri,
            config->xfyun_websocket_uri,
            sizeof(provider_config.websocket_uri));
    strlcpy(provider_config.auth_host,
            config->xfyun_auth_host,
            sizeof(provider_config.auth_host));

    err = tts_engine_provider_xfyun_set_config(&provider_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure XFYun TTS provider: %s", esp_err_to_name(err));
        return err;
    }

    runtime_config.provider_ops = tts_engine_provider_xfyun_get_ops();
    runtime_config.queue_capacity = TTS_ENGINE_MAX_PENDING;
    runtime_config.max_text_len = TTS_ENGINE_MAX_TEXT_LEN;
    runtime_config.provider_timeout_ms = provider_config.timeout_ms;
    runtime_config.speaker = speaker;

    err = tts_engine_init(&runtime_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TTS runtime: %s", esp_err_to_name(err));
        return err;
    }

    err = tts_engine_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TTS runtime: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static const char *cap_tts_state_to_string(tts_engine_state_t state)
{
    switch (state) {
    case TTS_ENGINE_STATE_IDLE:
        return "idle";
    case TTS_ENGINE_STATE_ACTIVE:
        return "active";
    case TTS_ENGINE_STATE_STOPPING:
        return "stopping";
    case TTS_ENGINE_STATE_DISABLED:
        return "disabled";
    default:
        return "unknown";
    }
}

static const char *cap_tts_result_to_string(tts_engine_result_t result)
{
    switch (result) {
    case TTS_ENGINE_RESULT_COMPLETED:
        return "completed";
    case TTS_ENGINE_RESULT_ABORTED:
        return "aborted";
    case TTS_ENGINE_RESULT_FAILED:
        return "failed";
    case TTS_ENGINE_RESULT_NONE:
    default:
        return "none";
    }
}

static const char *cap_tts_default_source(const claw_cap_call_context_t *ctx)
{
    if (!ctx) {
        return "system";
    }

    switch (ctx->caller) {
    case CLAW_CAP_CALLER_AGENT:
        return "agent";
    case CLAW_CAP_CALLER_CONSOLE:
        return "console";
    case CLAW_CAP_CALLER_SYSTEM:
    default:
        return "system";
    }
}

static esp_err_t cap_tts_render_status_json(char *output,
                                            size_t output_size,
                                            const tts_engine_status_t *status)
{
    cJSON *root = NULL;
    cJSON *active = NULL;
    cJSON *next_pending = NULL;
    char *json = NULL;
    esp_err_t err = ESP_OK;

    if (!output || output_size == 0 || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", cap_tts_state_to_string(status->state));
    cJSON_AddNumberToObject(root, "pending_count", (double)status->pending_count);
    cJSON_AddNumberToObject(root, "queue_capacity", (double)status->queue_capacity);
    cJSON_AddNumberToObject(root, "max_text_len", (double)status->max_text_len);
    cJSON_AddBoolToObject(root, "provider_running", status->provider_running);
    cJSON_AddBoolToObject(root, "stop_requested", status->stop_requested);
    cJSON_AddNumberToObject(root, "last_result_utterance_id",
                            (double)status->last_result_utterance_id);
    cJSON_AddNumberToObject(root, "last_result_session_id",
                            (double)status->last_result_session_id);
    cJSON_AddStringToObject(root, "last_result",
                            cap_tts_result_to_string(status->last_result));
    cJSON_AddStringToObject(root, "last_error", esp_err_to_name(status->last_error));

    active = cJSON_CreateObject();
    next_pending = cJSON_CreateObject();
    if (!active || !next_pending) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    cJSON_AddBoolToObject(active, "present", status->active.present);
    if (status->active.present) {
        cJSON_AddNumberToObject(active, "utterance_id", (double)status->active.utterance_id);
        cJSON_AddNumberToObject(active, "session_id", (double)status->active.session_id);
        cJSON_AddStringToObject(active, "source", status->active.source);
        cJSON_AddStringToObject(active, "text", status->active.text);
    }

    cJSON_AddBoolToObject(next_pending, "present", status->next_pending.present);
    if (status->next_pending.present) {
        cJSON_AddNumberToObject(next_pending, "utterance_id",
                                (double)status->next_pending.utterance_id);
        cJSON_AddNumberToObject(next_pending, "session_id",
                                (double)status->next_pending.session_id);
        cJSON_AddStringToObject(next_pending, "source", status->next_pending.source);
        cJSON_AddStringToObject(next_pending, "text", status->next_pending.text);
    }

    cJSON_AddItemToObject(root, "active", active);
    cJSON_AddItemToObject(root, "next_pending", next_pending);
    active = NULL;
    next_pending = NULL;

    {
        cJSON *provider = cJSON_CreateObject();

        if (!provider) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        cJSON_AddStringToObject(provider, "name", status->last_provider_status.provider_name);
        cJSON_AddStringToObject(provider, "session_id",
                                status->last_provider_status.provider_session_id);
        cJSON_AddStringToObject(provider, "message",
                                status->last_provider_status.provider_message);
        cJSON_AddStringToObject(provider, "codec", status->last_provider_status.codec);
        cJSON_AddNumberToObject(provider, "sample_rate_hz",
                                (double)status->last_provider_status.sample_rate_hz);
        cJSON_AddNumberToObject(provider, "channels",
                                (double)status->last_provider_status.channels);
        cJSON_AddNumberToObject(provider, "audio_bytes",
                                (double)status->last_provider_status.audio_bytes);
        cJSON_AddNumberToObject(provider, "connect_ms",
                                (double)status->last_provider_status.connect_ms);
        cJSON_AddNumberToObject(provider, "first_audio_ms",
                                (double)status->last_provider_status.first_audio_ms);
        cJSON_AddNumberToObject(provider, "complete_ms",
                                (double)status->last_provider_status.complete_ms);
        cJSON_AddNumberToObject(provider, "server_code",
                                (double)status->last_provider_status.server_code);
        cJSON_AddNumberToObject(provider, "handshake_status",
                                (double)status->last_provider_status.handshake_status);
        cJSON_AddNumberToObject(provider, "transport_errno",
                                (double)status->last_provider_status.transport_errno);
        cJSON_AddStringToObject(provider,
                                "transport_err",
                                esp_err_to_name(status->last_provider_status.transport_err));
        cJSON_AddBoolToObject(provider, "completed", status->last_provider_status.completed);
        cJSON_AddItemToObject(root, "provider", provider);
    }

    json = cJSON_PrintUnformatted(root);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    strlcpy(output, json, output_size);

cleanup:
    free(json);
    cJSON_Delete(active);
    cJSON_Delete(next_pending);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_tts_parse_text_request(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *text,
                                            size_t text_size,
                                            char *source,
                                            size_t source_size)
{
    cJSON *root = NULL;
    cJSON *text_item = NULL;
    cJSON *source_item = NULL;
    tts_engine_status_t status = {0};
    const char *resolved_source;
    esp_err_t err = ESP_OK;

    if (!input_json || !text || !source || text_size == 0 || source_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tts_engine_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json);
    if (!root || !cJSON_IsObject(root)) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring[0]) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (strlen(text_item->valuestring) > status.max_text_len) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    source_item = cJSON_GetObjectItemCaseSensitive(root, "source");
    resolved_source = cJSON_IsString(source_item) && source_item->valuestring[0] ?
        source_item->valuestring : cap_tts_default_source(ctx);
    if (strlen(resolved_source) >= source_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    strlcpy(text, text_item->valuestring, text_size);
    strlcpy(source, resolved_source, source_size);

cleanup:
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_tts_execute_enqueue_common(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size,
                                                bool insert_front)
{
    tts_engine_status_t status = {0};
    char text[TTS_ENGINE_MAX_TEXT_LEN + 1] = {0};
    char source[TTS_ENGINE_MAX_SOURCE_LEN] = {0};
    uint32_t utterance_id = 0;
    esp_err_t err;

    err = cap_tts_parse_text_request(input_json, ctx, text, sizeof(text), source, sizeof(source));
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_SIZE) {
            snprintf(output,
                     output_size,
                     "{\"ok\":false,\"error\":\"text_too_long_or_source_invalid\"}");
        } else {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid_request\"}");
        }
        return err;
    }

    err = insert_front ?
        tts_engine_enqueue_front(text, source, &utterance_id) :
        tts_engine_speak(text, source, &utterance_id);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NO_MEM) {
            esp_err_t status_err = tts_engine_get_status(&status);

            if (status_err == ESP_OK && status.pending_count >= status.queue_capacity) {
                snprintf(output,
                         output_size,
                         "{\"ok\":false,\"error\":\"queue_full\",\"detail\":\"pending queue capacity exceeded\"}");
            } else {
                snprintf(output,
                         output_size,
                         "{\"ok\":false,\"error\":\"%s\"}",
                         esp_err_to_name(err));
            }
        } else {
            snprintf(output,
                     output_size,
                     "{\"ok\":false,\"error\":\"%s\"}",
                     esp_err_to_name(err));
        }
        return err;
    }

    err = tts_engine_get_status(&status);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output,
             output_size,
             "{\"ok\":true,\"utterance_id\":%" PRIu32 ",\"operation\":\"%s\",\"pending_count\":%u,\"state\":\"%s\"}",
             utterance_id,
             insert_front ? "enqueue_front" : "speak",
             (unsigned)status.pending_count,
             cap_tts_state_to_string(status.state));
    return ESP_OK;
}

static esp_err_t cap_tts_execute_speak(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size)
{
    return cap_tts_execute_enqueue_common(input_json, ctx, output, output_size, false);
}

static esp_err_t cap_tts_execute_enqueue_front(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    return cap_tts_execute_enqueue_common(input_json, ctx, output, output_size, true);
}

static esp_err_t cap_tts_execute_clear_pending(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    size_t cleared = 0;
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = tts_engine_clear_pending(&cleared);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true,\"cleared\":%u}", (unsigned)cleared);
    return ESP_OK;
}

static esp_err_t cap_tts_execute_stop_current(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    bool had_active = false;
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = tts_engine_stop_current(&had_active);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true,\"had_active\":%s}", had_active ? "true" : "false");
    return ESP_OK;
}

static esp_err_t cap_tts_execute_status(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    tts_engine_status_t status = {0};
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = tts_engine_get_status(&status);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    return cap_tts_render_status_json(output, output_size, &status);
}

static esp_err_t cap_tts_group_start(void)
{
    return tts_engine_start();
}

static esp_err_t cap_tts_group_stop(void)
{
    return tts_engine_stop();
}

static const claw_cap_descriptor_t s_tts_descriptors[] = {
    {
        .id = "tts_speak",
        .name = "tts_speak",
        .family = "tts",
        .description = "Append a bounded TTS utterance to the tail queue or start it immediately if idle.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_TTS_SPEAK_SCHEMA,
        .execute = cap_tts_execute_speak,
    },
    {
        .id = "tts_enqueue_front",
        .name = "tts_enqueue_front",
        .family = "tts",
        .description = "Insert a bounded TTS utterance at the front of the pending queue without interrupting the current utterance.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_TTS_SPEAK_SCHEMA,
        .execute = cap_tts_execute_enqueue_front,
    },
    {
        .id = "tts_clear_pending",
        .name = "tts_clear_pending",
        .family = "tts",
        .description = "Clear pending TTS utterances while leaving the current active utterance unchanged.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_TTS_CLEAR_SCHEMA,
        .execute = cap_tts_execute_clear_pending,
    },
    {
        .id = "tts_stop_current",
        .name = "tts_stop_current",
        .family = "tts",
        .description = "Explicitly stop the current active TTS utterance and advance to the next pending item if one exists.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_TTS_CLEAR_SCHEMA,
        .execute = cap_tts_execute_stop_current,
    },
    {
        .id = "tts_status",
        .name = "tts_status",
        .family = "tts",
        .description = "Return the current TTS runtime state, queue depth, active utterance summary, and recent provider metrics.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_TTS_STATUS_SCHEMA,
        .execute = cap_tts_execute_status,
    },
};

static const claw_cap_group_t s_tts_group = {
    .group_id = "cap_tts",
    .descriptors = s_tts_descriptors,
    .descriptor_count = sizeof(s_tts_descriptors) / sizeof(s_tts_descriptors[0]),
    .group_start = cap_tts_group_start,
    .group_stop = cap_tts_group_stop,
};

esp_err_t cap_tts_register_group(void)
{
    if (claw_cap_group_exists(s_tts_group.group_id)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Registering cap_tts group");
    return claw_cap_register_group(&s_tts_group);
}
