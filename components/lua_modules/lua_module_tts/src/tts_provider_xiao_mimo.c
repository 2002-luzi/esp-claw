/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "tts_provider.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#ifdef CONFIG_LUA_TTS_MEMORY_PROFILING
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "tts_mimo";

#define MIMO_DEFAULT_BASE_URL   "https://api.xiaomimimo.com/v1"
#define MIMO_CHAT_COMPLETIONS   "/chat/completions"
#define MIMO_DEFAULT_MODEL      "mimo-v2.5-tts"
#define MIMO_DEFAULT_VOICE      "mimo_default"
#define MIMO_DEFAULT_TIMEOUT_MS 120000
#define MIMO_READ_BUF_SIZE      512
#define MIMO_B64_QUARTET        4

#ifdef CONFIG_LUA_TTS_MEMORY_PROFILING
static void mimo_log_mem_checkpoint(const char *mark)
{
    ESP_LOGI(TAG,
             "[mem] %s free_8bit=%u min_8bit=%u largest_8bit=%u internal_free=%u psram_free=%u stack_hwm_words=%u",
             mark,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#define MIMO_MEM_CHECKPOINT(mark) mimo_log_mem_checkpoint(mark)
#else
#define MIMO_MEM_CHECKPOINT(mark) do { (void)(mark); } while (0)
#endif

static const tts_audio_format_t s_mimo_audio_format = {
    .sample_rate_hz = 24000,
    .channels = 1,
    .bits_per_sample = 16,
};

typedef enum {
    MIMO_SCAN_NORMAL = 0,
    MIMO_SCAN_IN_STRING,
    MIMO_SCAN_IN_AUDIO_OBJECT,
    MIMO_SCAN_IN_AUDIO_KEY,
    MIMO_SCAN_IN_AUDIO_STRING_VALUE,
    MIMO_SCAN_IN_DATA_VALUE,
} mimo_audio_scan_state_t;

typedef struct {
    tts_provider_stream_t *stream;
    mimo_audio_scan_state_t state;
    char key_buf[16];
    size_t key_len;
    bool escaped;
    bool expect_audio_object;
    bool audio_string_is_key;
    bool data_open;
    int audio_depth;
    char b64[MIMO_B64_QUARTET];
    size_t b64_len;
} mimo_audio_parser_t;

typedef struct {
    mimo_audio_parser_t audio;
    char prefix[5];
    size_t prefix_len;
    bool prefix_done;
    bool is_data_line;
    bool skip_first_payload_space;
    bool probe_done;
    char done_probe[6];
    size_t done_probe_len;
    bool done;
} mimo_sse_parser_t;

static esp_err_t mimo_build_url(const char *base_url, char *url, size_t url_size)
{
    const char *base = (base_url && base_url[0]) ? base_url : MIMO_DEFAULT_BASE_URL;
    size_t base_len = strlen(base);
    const char *suffix = MIMO_CHAT_COMPLETIONS;

    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }

    if (base_len + strlen(suffix) + 1 > url_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(url, base, base_len);
    strlcpy(url + base_len, suffix, url_size - base_len);
    return ESP_OK;
}

static char *mimo_build_json_body(const tts_provider_config_t *config, const char *text)
{
    const char *model = (config->model && config->model[0]) ? config->model : MIMO_DEFAULT_MODEL;
    const char *voice = (config->voice && config->voice[0]) ? config->voice : MIMO_DEFAULT_VOICE;
    cJSON *root = NULL;
    cJSON *audio = NULL;
    cJSON *messages = NULL;
    cJSON *style_message = NULL;
    cJSON *message = NULL;
    char *body = NULL;

    root = cJSON_CreateObject();
    audio = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    message = cJSON_CreateObject();
    if (!root || !audio || !messages || !message) {
        goto cleanup;
    }

    if (!cJSON_AddStringToObject(root, "model", model) ||
        !cJSON_AddBoolToObject(root, "stream", true) ||
        !cJSON_AddStringToObject(audio, "voice", voice) ||
        !cJSON_AddStringToObject(audio, "format", "pcm16") ||
        !cJSON_AddStringToObject(message, "role", "assistant") ||
        !cJSON_AddStringToObject(message, "content", text)) {
        goto cleanup;
    }

    if (!cJSON_AddItemToObject(root, "audio", audio)) {
        goto cleanup;
    }
    audio = NULL;

    if (config->style && config->style[0]) {
        style_message = cJSON_CreateObject();
        if (!style_message ||
            !cJSON_AddStringToObject(style_message, "role", "user") ||
            !cJSON_AddStringToObject(style_message, "content", config->style) ||
            !cJSON_AddItemToArray(messages, style_message)) {
            goto cleanup;
        }
        style_message = NULL;
    }

    if (!cJSON_AddItemToArray(messages, message)) {
        goto cleanup;
    }
    message = NULL;
    if (!cJSON_AddItemToObject(root, "messages", messages)) {
        goto cleanup;
    }
    messages = NULL;

    body = cJSON_PrintUnformatted(root);

cleanup:
    cJSON_Delete(message);
    cJSON_Delete(style_message);
    cJSON_Delete(messages);
    cJSON_Delete(audio);
    cJSON_Delete(root);
    return body;
}

static esp_err_t mimo_write_decoded_quartet(mimo_audio_parser_t *parser)
{
    uint8_t decoded[3] = {0};
    size_t olen = 0;
    int ret;

    ret = mbedtls_base64_decode(decoded, sizeof(decoded), &olen,
                                (const unsigned char *)parser->b64,
                                parser->b64_len);
    parser->b64_len = 0;
    if (ret != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (olen == 0) {
        return ESP_OK;
    }
    return parser->stream->write_pcm(parser->stream->write_ctx, decoded, olen);
}

static esp_err_t mimo_audio_parser_push_b64(mimo_audio_parser_t *parser, char c)
{
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
        return ESP_OK;
    }
    if (parser->b64_len >= sizeof(parser->b64)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    parser->b64[parser->b64_len++] = c;
    if (parser->b64_len == sizeof(parser->b64)) {
        return mimo_write_decoded_quartet(parser);
    }
    return ESP_OK;
}

static void mimo_audio_parser_key_reset(mimo_audio_parser_t *parser)
{
    parser->key_len = 0;
    parser->key_buf[0] = '\0';
}

static void mimo_audio_parser_key_append(mimo_audio_parser_t *parser, char c)
{
    if (parser->key_len + 1 < sizeof(parser->key_buf)) {
        parser->key_buf[parser->key_len++] = c;
        parser->key_buf[parser->key_len] = '\0';
    }
}

static esp_err_t mimo_audio_parser_feed(mimo_audio_parser_t *parser, const char *json, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = json[i];

        switch (parser->state) {
        case MIMO_SCAN_NORMAL:
            if (parser->expect_audio_object) {
                if (c == '{') {
                    parser->state = MIMO_SCAN_IN_AUDIO_OBJECT;
                    parser->audio_depth = 1;
                    parser->audio_string_is_key = true;
                    parser->expect_audio_object = false;
                } else if (c != ':' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    parser->expect_audio_object = false;
                }
            } else if (c == '"') {
                parser->state = MIMO_SCAN_IN_STRING;
                parser->escaped = false;
                mimo_audio_parser_key_reset(parser);
            }
            break;

        case MIMO_SCAN_IN_STRING:
            if (parser->escaped) {
                mimo_audio_parser_key_append(parser, c);
                parser->escaped = false;
            } else if (c == '\\') {
                parser->escaped = true;
            } else if (c == '"') {
                if (strcmp(parser->key_buf, "audio") == 0) {
                    parser->expect_audio_object = true;
                }
                parser->state = MIMO_SCAN_NORMAL;
            } else {
                mimo_audio_parser_key_append(parser, c);
            }
            break;

        case MIMO_SCAN_IN_AUDIO_OBJECT:
            if (c == '{') {
                parser->audio_depth++;
                parser->audio_string_is_key = true;
            } else if (c == '}') {
                parser->audio_depth--;
                if (parser->audio_depth <= 0) {
                    parser->state = MIMO_SCAN_NORMAL;
                    parser->audio_depth = 0;
                }
            } else if (c == ',') {
                parser->audio_string_is_key = true;
            } else if (c == ':') {
                if (parser->audio_string_is_key && strcmp(parser->key_buf, "data") == 0) {
                    parser->state = MIMO_SCAN_IN_DATA_VALUE;
                    parser->escaped = false;
                    parser->b64_len = 0;
                    parser->data_open = false;
                }
                parser->audio_string_is_key = false;
            } else if (c == '"') {
                parser->state = parser->audio_string_is_key ?
                                MIMO_SCAN_IN_AUDIO_KEY :
                                MIMO_SCAN_IN_AUDIO_STRING_VALUE;
                parser->escaped = false;
                if (parser->audio_string_is_key) {
                    mimo_audio_parser_key_reset(parser);
                }
            }
            break;

        case MIMO_SCAN_IN_AUDIO_KEY:
            if (parser->escaped) {
                mimo_audio_parser_key_append(parser, c);
                parser->escaped = false;
            } else if (c == '\\') {
                parser->escaped = true;
            } else if (c == '"') {
                parser->state = MIMO_SCAN_IN_AUDIO_OBJECT;
            } else {
                mimo_audio_parser_key_append(parser, c);
            }
            break;

        case MIMO_SCAN_IN_AUDIO_STRING_VALUE:
            if (parser->escaped) {
                parser->escaped = false;
            } else if (c == '\\') {
                parser->escaped = true;
            } else if (c == '"') {
                parser->state = MIMO_SCAN_IN_AUDIO_OBJECT;
            }
            break;

        case MIMO_SCAN_IN_DATA_VALUE:
            if (!parser->data_open) {
                if (c == '"') {
                    parser->data_open = true;
                } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            } else if (parser->escaped) {
                ESP_RETURN_ON_ERROR(mimo_audio_parser_push_b64(parser, c),
                                    TAG, "invalid base64 audio");
                parser->escaped = false;
            } else if (c == '\\') {
                parser->escaped = true;
            } else if (c == '"') {
                if (parser->b64_len != 0) {
                    ESP_RETURN_ON_ERROR(mimo_write_decoded_quartet(parser),
                                        TAG, "invalid base64 tail");
                }
                parser->state = MIMO_SCAN_IN_AUDIO_OBJECT;
                parser->audio_string_is_key = false;
                parser->data_open = false;
            } else {
                ESP_RETURN_ON_ERROR(mimo_audio_parser_push_b64(parser, c),
                                    TAG, "invalid base64 audio");
            }
            break;
        }
    }

    return ESP_OK;
}

static void mimo_sse_reset_line(mimo_sse_parser_t *parser)
{
    parser->prefix_len = 0;
    parser->prefix[0] = '\0';
    parser->prefix_done = false;
    parser->is_data_line = false;
    parser->skip_first_payload_space = false;
    parser->probe_done = false;
    parser->done_probe_len = 0;
}

static esp_err_t mimo_sse_flush_done_probe(mimo_sse_parser_t *parser)
{
    if (parser->done_probe_len == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(mimo_audio_parser_feed(&parser->audio,
                                               parser->done_probe,
                                               parser->done_probe_len),
                        TAG, "failed to parse buffered SSE payload");
    parser->done_probe_len = 0;
    return ESP_OK;
}

static esp_err_t mimo_sse_process_payload_char(mimo_sse_parser_t *parser, char c)
{
    static const char done_marker[] = "[DONE]";

    if (parser->probe_done) {
        if (parser->done_probe_len < sizeof(done_marker) - 1 &&
            c == done_marker[parser->done_probe_len]) {
            parser->done_probe[parser->done_probe_len++] = c;
            return ESP_OK;
        }

        parser->probe_done = false;
        ESP_RETURN_ON_ERROR(mimo_sse_flush_done_probe(parser),
                            TAG, "failed to flush SSE probe");
    }

    return mimo_audio_parser_feed(&parser->audio, &c, 1);
}

static esp_err_t mimo_sse_finish_line(mimo_sse_parser_t *parser)
{
    if (parser->is_data_line && parser->probe_done) {
        if (parser->done_probe_len == sizeof(parser->done_probe) &&
            memcmp(parser->done_probe, "[DONE]", sizeof(parser->done_probe)) == 0) {
            parser->done = true;
        } else {
            ESP_RETURN_ON_ERROR(mimo_sse_flush_done_probe(parser),
                                TAG, "failed to flush final SSE probe");
        }
    }

    mimo_sse_reset_line(parser);
    return ESP_OK;
}

static esp_err_t mimo_sse_feed_char(mimo_sse_parser_t *parser, char c)
{
    if (c == '\r') {
        return ESP_OK;
    }
    if (c == '\n') {
        return mimo_sse_finish_line(parser);
    }
    if (parser->done) {
        return ESP_OK;
    }

    if (!parser->prefix_done) {
        if (c == ':') {
            parser->prefix_done = true;
            parser->is_data_line = strcmp(parser->prefix, "data") == 0;
            parser->skip_first_payload_space = parser->is_data_line;
            parser->probe_done = parser->is_data_line;
            parser->done_probe_len = 0;
            return ESP_OK;
        }
        if (parser->prefix_len + 1 < sizeof(parser->prefix)) {
            parser->prefix[parser->prefix_len++] = c;
            parser->prefix[parser->prefix_len] = '\0';
        }
        return ESP_OK;
    }

    if (!parser->is_data_line) {
        return ESP_OK;
    }

    if (parser->skip_first_payload_space) {
        parser->skip_first_payload_space = false;
        if (c == ' ') {
            return ESP_OK;
        }
    }

    return mimo_sse_process_payload_char(parser, c);
}

static esp_err_t mimo_stream_response(esp_http_client_handle_t client, tts_provider_stream_t *stream)
{
    char read_buf[MIMO_READ_BUF_SIZE];
    mimo_sse_parser_t parser = {0};

    parser.audio.stream = stream;

    while (1) {
        int read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));
        if (read_len < 0) {
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        stream->http_bytes += (size_t)read_len;

        for (int i = 0; i < read_len; i++) {
            ESP_RETURN_ON_ERROR(mimo_sse_feed_char(&parser, read_buf[i]),
                                TAG, "failed to parse SSE stream");
        }
        if (parser.done) {
            break;
        }
    }

    if (!parser.done && (parser.prefix_len > 0 || parser.prefix_done)) {
        ESP_RETURN_ON_ERROR(mimo_sse_finish_line(&parser),
                            TAG, "failed to finish SSE stream");
    }

    return ESP_OK;
}

static esp_err_t mimo_play(const tts_provider_config_t *config,
                           const char *text,
                           tts_provider_stream_t *stream)
{
    char url[256] = {0};
    char *body = NULL;
    char auth[384] = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    size_t body_len = 0;
    int status;

    if (!config || !text || !text[0] || !stream || !stream->write_pcm) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->api_key || !config->api_key[0]) {
        ESP_LOGE(TAG, "missing MiMo API key");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(mimo_build_url(config->base_url, url, sizeof(url)),
                        TAG, "MiMo URL too long");
    body = mimo_build_json_body(config, text);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    body_len = strlen(body);
    if (body_len > INT_MAX) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    if (snprintf(auth, sizeof(auth), "Bearer %s", config->api_key) >= sizeof(auth)) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = (int)(config->timeout_ms ? config->timeout_ms : MIMO_DEFAULT_TIMEOUT_MS),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    client = esp_http_client_init(&http_config);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    MIMO_MEM_CHECKPOINT("after esp_http_client_init");

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");

    err = esp_http_client_open(client, (int)body_len);
    MIMO_MEM_CHECKPOINT("after esp_http_client_open");
    if (err == ESP_OK) {
        int written = esp_http_client_write(client, body, (int)body_len);
        if (written < 0 || written != (int)body_len) {
            err = ESP_FAIL;
        }
    }
    free(body);

    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }

    status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGE(TAG, "MiMo TTS HTTP status %d", status);
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        err = mimo_stream_response(client, stream);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

const tts_provider_t tts_provider_xiao_mimo = {
    .name = "xiao_mimo",
    .audio_format = &s_mimo_audio_format,
    .play = mimo_play,
};
