#include "tts_engine_provider_xfyun.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "tts_provider_xfyun";

#define TTS_ENGINE_PROVIDER_XFYUN_EVENT_CONNECTED  BIT0
#define TTS_ENGINE_PROVIDER_XFYUN_EVENT_DONE       BIT1
#define TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED     BIT2
#define TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE   BIT3

#define TTS_ENGINE_PROVIDER_XFYUN_WS_BUFFER_SIZE   1024
#define TTS_ENGINE_PROVIDER_XFYUN_WS_STACK_SIZE    6144
#define TTS_ENGINE_PROVIDER_XFYUN_WS_TASK_PRIO     5
#define TTS_ENGINE_PROVIDER_XFYUN_VALUE_BUF_LEN    128
#define TTS_ENGINE_PROVIDER_XFYUN_KEY_BUF_LEN      24
#define TTS_ENGINE_PROVIDER_XFYUN_DATE_LEN         64
#define TTS_ENGINE_PROVIDER_XFYUN_HOST_BUF_LEN     128
#define TTS_ENGINE_PROVIDER_XFYUN_PATH_BUF_LEN     128
#define TTS_ENGINE_PROVIDER_XFYUN_REQUEST_PATH     "/v2/tts"
#define TTS_ENGINE_PROVIDER_XFYUN_AUDIO_CHUNK_MS   60U
#define TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN 4096U
#define TTS_ENGINE_PROVIDER_XFYUN_MIN_VALID_TIME   1704067200LL
#define TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS  15000U
#define TTS_ENGINE_PROVIDER_XFYUN_SNTP_WAIT_MS     1000U
#define TTS_ENGINE_PROVIDER_XFYUN_SNTP_SERVER      "pool.ntp.org"

typedef enum {
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE = 0,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_CODE,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_SID,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_MESSAGE,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_OBJECT,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_STATUS,
    TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_AUDIO,
} tts_engine_provider_xfyun_field_t;

typedef enum {
    TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE = 0,
    TTS_ENGINE_PROVIDER_XFYUN_TOKEN_KEY,
    TTS_ENGINE_PROVIDER_XFYUN_TOKEN_STRING,
    TTS_ENGINE_PROVIDER_XFYUN_TOKEN_NUMBER,
    TTS_ENGINE_PROVIDER_XFYUN_TOKEN_LITERAL,
} tts_engine_provider_xfyun_token_t;

typedef struct {
    int object_depth;
    bool in_data_object;
    bool expecting_key;
    bool expecting_value;
    bool key_escape;
    bool value_escape;
    bool string_is_audio;
    bool message_status_valid;
    int message_status;
    char base64_group[4];
    size_t base64_group_len;
    char key_buf[TTS_ENGINE_PROVIDER_XFYUN_KEY_BUF_LEN];
    size_t key_len;
    char value_buf[TTS_ENGINE_PROVIDER_XFYUN_VALUE_BUF_LEN];
    size_t value_len;
    tts_engine_provider_xfyun_token_t token;
    tts_engine_provider_xfyun_field_t pending_field;
    tts_engine_provider_xfyun_field_t active_field;
} tts_engine_provider_xfyun_parser_t;

typedef struct {
    EventGroupHandle_t event_group;
    esp_websocket_client_handle_t client;
    const tts_engine_provider_request_t *request;
    tts_engine_provider_stream_status_t *status;
    tts_engine_provider_xfyun_parser_t parser;
    uint8_t *audio_buffer;
    size_t audio_buffer_len;
    size_t audio_chunk_target_len;
    uint8_t audio_frame_bytes;
    int64_t connect_start_us;
    int64_t send_ts_us;
    esp_err_t last_err;
    bool done;
    bool abort_requested;
} tts_engine_provider_xfyun_stream_ctx_t;

typedef struct {
    tts_engine_provider_xfyun_config_t config;
    bool configured;
    bool sntp_initialized;
    bool sync_in_progress;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
    EventGroupHandle_t time_events;
    StaticEventGroup_t time_events_buffer;
    esp_err_t last_sync_err;
    uint8_t audio_buffer[TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN];
    tts_engine_provider_xfyun_stream_ctx_t *active_ctx;
} tts_engine_provider_xfyun_runtime_t;

static tts_engine_provider_xfyun_runtime_t s_tts_engine_provider_xfyun = {
    .last_sync_err = ESP_OK,
};

static SemaphoreHandle_t tts_engine_provider_xfyun_get_lock(void)
{
    if (s_tts_engine_provider_xfyun.lock == NULL) {
        s_tts_engine_provider_xfyun.lock =
            xSemaphoreCreateMutexStatic(&s_tts_engine_provider_xfyun.lock_buffer);
    }

    return s_tts_engine_provider_xfyun.lock;
}

static bool tts_engine_provider_xfyun_clock_is_valid(void)
{
    time_t now = time(NULL);

    return now >= (time_t)TTS_ENGINE_PROVIDER_XFYUN_MIN_VALID_TIME;
}

static esp_err_t tts_engine_provider_xfyun_time_init(void)
{
    SemaphoreHandle_t lock = tts_engine_provider_xfyun_get_lock();

    if (lock == NULL) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tts_engine_provider_xfyun.time_events == NULL) {
        s_tts_engine_provider_xfyun.time_events =
            xEventGroupCreateStatic(&s_tts_engine_provider_xfyun.time_events_buffer);
        if (s_tts_engine_provider_xfyun.time_events != NULL) {
            xEventGroupSetBits(s_tts_engine_provider_xfyun.time_events,
                               TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE);
        }
    }
    xSemaphoreGive(lock);

    return s_tts_engine_provider_xfyun.time_events != NULL ? ESP_OK : ESP_FAIL;
}

static esp_err_t tts_engine_provider_xfyun_prepare_sntp_locked(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(TTS_ENGINE_PROVIDER_XFYUN_SNTP_SERVER);
    esp_err_t err;

    if (s_tts_engine_provider_xfyun.sntp_initialized) {
        return ESP_OK;
    }

    config.start = false;
    err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP already initialized elsewhere, reusing it for xfyun TTS");
        s_tts_engine_provider_xfyun.sntp_initialized = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SNTP for xfyun TTS: %s", esp_err_to_name(err));
        return err;
    }

    s_tts_engine_provider_xfyun.sntp_initialized = true;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_wait_for_sync_completion(void)
{
    EventBits_t bits;

    bits = xEventGroupWaitBits(s_tts_engine_provider_xfyun.time_events,
                               TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS));
    if ((bits & TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for ongoing xfyun TTS time sync");
        return ESP_ERR_TIMEOUT;
    }
    if (tts_engine_provider_xfyun_clock_is_valid()) {
        return ESP_OK;
    }

    return s_tts_engine_provider_xfyun.last_sync_err != ESP_OK ?
               s_tts_engine_provider_xfyun.last_sync_err :
               ESP_ERR_TIMEOUT;
}

static esp_err_t tts_engine_provider_xfyun_run_sntp_sync(void)
{
    uint32_t elapsed_ms = 0;
    esp_err_t err;

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP for xfyun TTS: %s", esp_err_to_name(err));
        return err;
    }

    while (elapsed_ms < TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS) {
        uint32_t wait_ms = TTS_ENGINE_PROVIDER_XFYUN_SNTP_WAIT_MS;

        if (wait_ms > (TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS - elapsed_ms)) {
            wait_ms = TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS - elapsed_ms;
        }

        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(wait_ms));
        if (tts_engine_provider_xfyun_clock_is_valid()) {
            return ESP_OK;
        }
        if (err != ESP_OK &&
            err != ESP_ERR_TIMEOUT &&
            err != ESP_ERR_NOT_FINISHED &&
            err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SNTP sync wait failed for xfyun TTS: %s", esp_err_to_name(err));
            return err;
        }

        elapsed_ms += wait_ms;
    }

    return tts_engine_provider_xfyun_clock_is_valid() ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t tts_engine_provider_xfyun_sync_time_if_needed(void)
{
    SemaphoreHandle_t lock = tts_engine_provider_xfyun_get_lock();
    esp_err_t err = ESP_OK;
    bool wait_for_existing = false;

    if (tts_engine_provider_xfyun_clock_is_valid()) {
        return ESP_OK;
    }

    err = tts_engine_provider_xfyun_time_init();
    if (err != ESP_OK) {
        return err;
    }
    if (lock == NULL || s_tts_engine_provider_xfyun.time_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (tts_engine_provider_xfyun_clock_is_valid()) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    if (s_tts_engine_provider_xfyun.sync_in_progress) {
        wait_for_existing = true;
    } else {
        err = tts_engine_provider_xfyun_prepare_sntp_locked();
        if (err == ESP_OK) {
            s_tts_engine_provider_xfyun.sync_in_progress = true;
            s_tts_engine_provider_xfyun.last_sync_err = ESP_ERR_TIMEOUT;
            xEventGroupClearBits(s_tts_engine_provider_xfyun.time_events,
                                 TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE);
        }
    }
    xSemaphoreGive(lock);

    if (wait_for_existing) {
        return tts_engine_provider_xfyun_wait_for_sync_completion();
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(TAG, "System clock looks unsynced, syncing time for xfyun TTS");
    err = tts_engine_provider_xfyun_run_sntp_sync();
    if (err == ESP_OK) {
        time_t now = time(NULL);

        ESP_LOGI(TAG, "xfyun TTS time sync ready, unix_time=%" PRIi64, (int64_t)now);
    } else {
        ESP_LOGE(TAG, "xfyun TTS time sync failed within %u ms: %s",
                 TTS_ENGINE_PROVIDER_XFYUN_SNTP_TIMEOUT_MS,
                 esp_err_to_name(err));
    }

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_tts_engine_provider_xfyun.sync_in_progress = false;
    s_tts_engine_provider_xfyun.last_sync_err = err;
    xEventGroupSetBits(s_tts_engine_provider_xfyun.time_events,
                       TTS_ENGINE_PROVIDER_XFYUN_TIME_SYNC_DONE);
    xSemaphoreGive(lock);
    return err;
}

static uint32_t tts_engine_provider_xfyun_parse_sample_rate_hz(const char *audio_format)
{
    const char *rate;

    if (!audio_format) {
        return 0;
    }

    rate = strstr(audio_format, "rate=");
    if (!rate) {
        return 0;
    }

    return (uint32_t)atoi(rate + 5);
}

static uint8_t tts_engine_provider_xfyun_parse_channels(const char *audio_format)
{
    const char *channels;
    int value;

    if (!audio_format) {
        return 1;
    }

    channels = strstr(audio_format, "channels=");
    if (!channels) {
        channels = strstr(audio_format, "channel=");
    }
    if (!channels) {
        return 1;
    }

    value = atoi(strchr(channels, '=') + 1);
    if (value <= 0 || value > UINT8_MAX) {
        return 1;
    }

    return (uint8_t)value;
}

static uint8_t tts_engine_provider_xfyun_parse_audio_frame_bytes(const char *audio_format, uint8_t channels)
{
    const char *linear_pcm;
    int bits_per_sample = 16;

    if (audio_format) {
        linear_pcm = strstr(audio_format, "audio/L");
        if (linear_pcm) {
            int parsed_bits = atoi(linear_pcm + strlen("audio/L"));

            if (parsed_bits > 0 && (parsed_bits % 8) == 0) {
                bits_per_sample = parsed_bits;
            }
        }
    }

    if (channels == 0) {
        channels = 1;
    }

    return (uint8_t)((bits_per_sample / 8) * channels);
}

static size_t tts_engine_provider_xfyun_compute_chunk_target_len(uint32_t sample_rate_hz, uint8_t frame_bytes)
{
    uint64_t target_len;

    if (sample_rate_hz == 0 || frame_bytes == 0) {
        return TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN;
    }

    target_len = ((uint64_t)sample_rate_hz * (uint64_t)frame_bytes *
                  (uint64_t)TTS_ENGINE_PROVIDER_XFYUN_AUDIO_CHUNK_MS + 999ULL) / 1000ULL;
    if (target_len == 0) {
        target_len = frame_bytes;
    }
    if (target_len > TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN) {
        target_len = TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN;
    }
    if (frame_bytes > 1) {
        target_len -= target_len % frame_bytes;
        if (target_len == 0) {
            target_len = frame_bytes;
        }
    }

    return (size_t)target_len;
}

static void tts_engine_provider_xfyun_init_status(tts_engine_provider_stream_status_t *status,
                                       uint32_t service_session_id,
                                       const tts_engine_provider_xfyun_config_t *config)
{
    memset(status, 0, sizeof(*status));
    status->service_session_id = service_session_id;
    status->connect_ms = -1;
    status->first_audio_ms = -1;
    status->complete_ms = -1;
    status->server_code = -1;
    status->transport_err = ESP_OK;
    status->channels = 1;
    strlcpy(status->provider_name, "xfyun", sizeof(status->provider_name));
    if (config) {
        strlcpy(status->codec, config->audio_encoding, sizeof(status->codec));
        status->sample_rate_hz = tts_engine_provider_xfyun_parse_sample_rate_hz(config->audio_format);
        status->channels = tts_engine_provider_xfyun_parse_channels(config->audio_format);
    }
}

static esp_err_t tts_engine_provider_xfyun_emit_event(tts_engine_provider_xfyun_stream_ctx_t *ctx,
                                           tts_engine_provider_event_type_t type,
                                           const uint8_t *audio,
                                           size_t audio_len,
                                           esp_err_t error)
{
    tts_engine_provider_event_t event = {
        .type = type,
        .status = ctx ? ctx->status : NULL,
        .audio = audio,
        .audio_len = audio_len,
        .error = error,
    };

    if (!ctx || !ctx->request || !ctx->request->listener || !ctx->request->listener->on_event) {
        return ESP_OK;
    }

    return ctx->request->listener->on_event(ctx->request->listener->user_ctx, &event);
}

static void tts_engine_provider_xfyun_set_error(tts_engine_provider_xfyun_stream_ctx_t *ctx, esp_err_t err)
{
    if (ctx->last_err == ESP_OK) {
        ctx->last_err = err;
    }
}

static esp_err_t tts_engine_provider_xfyun_format_date(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm gm_time = {0};

    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    now = time(NULL);
    if (now <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gmtime_r(&now, &gm_time) == NULL) {
        return ESP_FAIL;
    }
    if (strftime(buffer, buffer_size, "%a, %d %b %Y %H:%M:%S GMT", &gm_time) == 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_base64_encode(const uint8_t *input,
                                              size_t input_len,
                                              char **output)
{
    int ret;
    unsigned char *buffer = NULL;
    size_t out_len = 0;

    if (!input || !output) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_base64_encode(NULL, 0, &out_len, input, input_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || out_len == 0) {
        return ESP_FAIL;
    }

    buffer = calloc(1, out_len + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_encode(buffer, out_len, &out_len, input, input_len);
    if (ret != 0) {
        free(buffer);
        return ESP_FAIL;
    }

    buffer[out_len] = '\0';
    *output = (char *)buffer;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_url_encode(const char *input, char **output)
{
    char *buffer = NULL;
    char *dst;
    const unsigned char *src;
    size_t len;

    if (!input || !output) {
        return ESP_ERR_INVALID_ARG;
    }

    len = strlen(input);
    buffer = calloc(1, len * 3 + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    dst = buffer;
    for (src = (const unsigned char *)input; *src != '\0'; src++) {
        if ((*src >= '0' && *src <= '9') ||
            (*src >= 'A' && *src <= 'Z') ||
            (*src >= 'a' && *src <= 'z') ||
            *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            *dst++ = (char)*src;
            continue;
        }

        snprintf(dst, 4, "%%%02X", *src);
        dst += 3;
    }

    *dst = '\0';
    *output = buffer;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_parse_uri(const char *uri,
                                          char *host,
                                          size_t host_size,
                                          char *path,
                                          size_t path_size)
{
    const char *authority;
    const char *scheme_sep;
    const char *authority_end;
    const char *path_end;
    size_t host_len;
    size_t path_len;

    if (!uri || !host || host_size == 0 || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    scheme_sep = strstr(uri, "://");
    authority = scheme_sep ? (scheme_sep + 3) : uri;
    authority_end = authority;
    while (*authority_end != '\0' &&
           *authority_end != '/' &&
           *authority_end != '?' &&
           *authority_end != '#') {
        authority_end++;
    }
    if (authority_end == authority) {
        return ESP_ERR_INVALID_ARG;
    }

    host_len = (size_t)(authority_end - authority);
    if (host_len >= host_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(host, authority, host_len);
    host[host_len] = '\0';

    if (*authority_end != '/') {
        strlcpy(path, "/", path_size);
        return ESP_OK;
    }

    path_end = authority_end;
    while (*path_end != '\0' && *path_end != '?' && *path_end != '#') {
        path_end++;
    }

    path_len = (size_t)(path_end - authority_end);
    if (path_len == 0) {
        strlcpy(path, "/", path_size);
        return ESP_OK;
    }
    if (path_len >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(path, authority_end, path_len);
    path[path_len] = '\0';
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_resolve_auth_endpoint(const tts_engine_provider_xfyun_config_t *config,
                                                      char *host,
                                                      size_t host_size,
                                                      char *path,
                                                      size_t path_size)
{
    char uri_host[TTS_ENGINE_PROVIDER_XFYUN_HOST_BUF_LEN] = {0};
    char uri_path[TTS_ENGINE_PROVIDER_XFYUN_PATH_BUF_LEN] = {0};
    esp_err_t err;

    if (!config || !host || host_size == 0 || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tts_engine_provider_xfyun_parse_uri(config->websocket_uri,
                                   uri_host,
                                   sizeof(uri_host),
                                   uri_path,
                                   sizeof(uri_path));
    if (err != ESP_OK) {
        return err;
    }

    if (config->auth_host[0] != '\0' && strcmp(config->auth_host, uri_host) != 0) {
        ESP_LOGW(TAG,
                 "auth_host=%s mismatches websocket uri host=%s, auth may fail",
                 config->auth_host,
                 uri_host);
    }

    strlcpy(host,
            config->auth_host[0] ? config->auth_host : uri_host,
            host_size);
    strlcpy(path, uri_path[0] ? uri_path : TTS_ENGINE_PROVIDER_XFYUN_REQUEST_PATH, path_size);
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_build_url(const tts_engine_provider_xfyun_config_t *config,
                                          char **url_out)
{
    unsigned char signature_raw[32] = {0};
    const mbedtls_md_info_t *md_info;
    char date_header[TTS_ENGINE_PROVIDER_XFYUN_DATE_LEN] = {0};
    char auth_host[TTS_ENGINE_PROVIDER_XFYUN_HOST_BUF_LEN] = {0};
    char request_path[TTS_ENGINE_PROVIDER_XFYUN_PATH_BUF_LEN] = {0};
    char signature_origin[256] = {0};
    char authorization_origin[256] = {0};
    char *signature_b64 = NULL;
    char *authorization_b64 = NULL;
    char *authorization_encoded = NULL;
    char *date_encoded = NULL;
    char *host_encoded = NULL;
    char *url = NULL;
    esp_err_t err;
    int written;

    if (!config || !url_out) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tts_engine_provider_xfyun_format_date(date_header, sizeof(date_header));
    if (err != ESP_OK) {
        return err;
    }

    err = tts_engine_provider_xfyun_resolve_auth_endpoint(config,
                                               auth_host,
                                               sizeof(auth_host),
                                               request_path,
                                               sizeof(request_path));
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "Auth endpoint host=%s path=%s date=%s",
             auth_host,
             request_path,
             date_header);
    if (strstr(date_header, "1970") != NULL) {
        ESP_LOGW(TAG, "System clock looks unsynced, TTS auth may be rejected");
    }

    written = snprintf(signature_origin,
                       sizeof(signature_origin),
                       "host: %s\n"
                       "date: %s\n"
                       "GET %s HTTP/1.1",
                       auth_host,
                       date_header,
                       request_path);
    if (written <= 0 || (size_t)written >= sizeof(signature_origin)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Auth signature origin: %s", signature_origin);

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) {
        return ESP_FAIL;
    }

    if (mbedtls_md_hmac(md_info,
                        (const unsigned char *)config->api_secret,
                        strlen(config->api_secret),
                        (const unsigned char *)signature_origin,
                        strlen(signature_origin),
                        signature_raw) != 0) {
        return ESP_FAIL;
    }

    err = tts_engine_provider_xfyun_base64_encode(signature_raw, sizeof(signature_raw), &signature_b64);
    if (err != ESP_OK) {
        return err;
    }

    written = snprintf(authorization_origin,
                       sizeof(authorization_origin),
                       "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
                       config->api_key,
                       signature_b64);
    free(signature_b64);
    if (written <= 0 || (size_t)written >= sizeof(authorization_origin)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = tts_engine_provider_xfyun_base64_encode((const uint8_t *)authorization_origin,
                                       strlen(authorization_origin),
                                       &authorization_b64);
    if (err != ESP_OK) {
        return err;
    }

    err = tts_engine_provider_xfyun_url_encode(authorization_b64, &authorization_encoded);
    free(authorization_b64);
    if (err != ESP_OK) {
        return err;
    }

    err = tts_engine_provider_xfyun_url_encode(date_header, &date_encoded);
    if (err != ESP_OK) {
        free(authorization_encoded);
        return err;
    }

    err = tts_engine_provider_xfyun_url_encode(auth_host, &host_encoded);
    if (err != ESP_OK) {
        free(authorization_encoded);
        free(date_encoded);
        return err;
    }

    written = snprintf(NULL,
                       0,
                       "%s?authorization=%s&date=%s&host=%s",
                       config->websocket_uri,
                       authorization_encoded,
                       date_encoded,
                       host_encoded);
    if (written <= 0) {
        free(authorization_encoded);
        free(date_encoded);
        free(host_encoded);
        return ESP_FAIL;
    }

    url = calloc(1, (size_t)written + 1);
    if (!url) {
        free(authorization_encoded);
        free(date_encoded);
        free(host_encoded);
        return ESP_ERR_NO_MEM;
    }

    snprintf(url,
             (size_t)written + 1,
             "%s?authorization=%s&date=%s&host=%s",
             config->websocket_uri,
             authorization_encoded,
             date_encoded,
             host_encoded);

    free(authorization_encoded);
    free(date_encoded);
    free(host_encoded);
    *url_out = url;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_build_request(const tts_engine_provider_xfyun_config_t *config,
                                              const char *text,
                                              char **request_out)
{
    cJSON *root = NULL;
    cJSON *common = NULL;
    cJSON *business = NULL;
    cJSON *data = NULL;
    char *text_b64 = NULL;
    char *payload = NULL;
    esp_err_t err;

    if (!config || !text || !request_out) {
        return ESP_ERR_INVALID_ARG;
    }

    err = tts_engine_provider_xfyun_base64_encode((const uint8_t *)text, strlen(text), &text_b64);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    common = cJSON_CreateObject();
    business = cJSON_CreateObject();
    data = cJSON_CreateObject();
    if (!root || !common || !business || !data) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    cJSON_AddItemToObject(root, "common", common);
    cJSON_AddItemToObject(root, "business", business);
    cJSON_AddItemToObject(root, "data", data);

    cJSON_AddStringToObject(common, "app_id", config->app_id);
    cJSON_AddStringToObject(business, "aue", config->audio_encoding);
    cJSON_AddStringToObject(business, "auf", config->audio_format);
    cJSON_AddStringToObject(business, "vcn", config->voice_name);
    cJSON_AddStringToObject(business, "tte", config->text_encoding);
    cJSON_AddNumberToObject(data, "status", 2);
    cJSON_AddStringToObject(data, "text", text_b64);

    payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *request_out = payload;
    payload = NULL;
    err = ESP_OK;

cleanup:
    free(text_b64);
    free(payload);
    cJSON_Delete(root);
    return err;
}

static uint8_t tts_engine_provider_xfyun_base64_value(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (uint8_t)(value - 'A');
    }
    if (value >= 'a' && value <= 'z') {
        return (uint8_t)(value - 'a' + 26);
    }
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0' + 52);
    }
    if (value == '+') {
        return 62;
    }
    return 63;
}

static esp_err_t tts_engine_provider_xfyun_dispatch_audio(tts_engine_provider_xfyun_stream_ctx_t *ctx,
                                               const uint8_t *audio,
                                               size_t audio_len)
{
    size_t flush_len;

    if (!ctx || !audio || audio_len == 0) {
        return ESP_OK;
    }

    ctx->status->audio_bytes += (uint32_t)audio_len;
    if (ctx->status->first_audio_ms < 0 && ctx->send_ts_us > 0) {
        ctx->status->first_audio_ms =
            (int32_t)((esp_timer_get_time() - ctx->send_ts_us) / 1000LL);
        ESP_LOGI(TAG, "First decoded audio after %" PRIi32 " ms", ctx->status->first_audio_ms);
    }

    if (ctx->audio_buffer_len + audio_len > TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN) {
        flush_len = ctx->audio_buffer_len;
        if (ctx->audio_frame_bytes > 1) {
            flush_len -= flush_len % ctx->audio_frame_bytes;
        }
        if (flush_len == 0) {
            flush_len = ctx->audio_buffer_len;
        }
        if (flush_len > 0) {
            esp_err_t err = tts_engine_provider_xfyun_emit_event(ctx,
                                                      TTS_ENGINE_PROVIDER_EVENT_AUDIO_CHUNK,
                                                      ctx->audio_buffer,
                                                      flush_len,
                                                      ESP_OK);
            if (err != ESP_OK) {
                return err;
            }
            if (flush_len < ctx->audio_buffer_len) {
                memmove(ctx->audio_buffer,
                        ctx->audio_buffer + flush_len,
                        ctx->audio_buffer_len - flush_len);
            }
            ctx->audio_buffer_len -= flush_len;
        }
    }

    if (ctx->audio_buffer_len + audio_len > TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(ctx->audio_buffer + ctx->audio_buffer_len, audio, audio_len);
    ctx->audio_buffer_len += audio_len;

    if (ctx->audio_buffer_len < ctx->audio_chunk_target_len &&
        ctx->audio_buffer_len < TTS_ENGINE_PROVIDER_XFYUN_AUDIO_BUFFER_LEN) {
        return ESP_OK;
    }

    flush_len = ctx->audio_buffer_len;
    if (ctx->audio_frame_bytes > 1) {
        flush_len -= flush_len % ctx->audio_frame_bytes;
    }
    if (flush_len == 0) {
        return ESP_OK;
    }

    {
        esp_err_t err = tts_engine_provider_xfyun_emit_event(ctx,
                                                  TTS_ENGINE_PROVIDER_EVENT_AUDIO_CHUNK,
                                                  ctx->audio_buffer,
                                                  flush_len,
                                                  ESP_OK);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (flush_len < ctx->audio_buffer_len) {
        memmove(ctx->audio_buffer,
                ctx->audio_buffer + flush_len,
                ctx->audio_buffer_len - flush_len);
    }
    ctx->audio_buffer_len -= flush_len;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_flush_audio_buffer(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    esp_err_t err;

    if (!ctx || ctx->audio_buffer_len == 0) {
        return ESP_OK;
    }

    err = tts_engine_provider_xfyun_emit_event(ctx,
                                    TTS_ENGINE_PROVIDER_EVENT_AUDIO_CHUNK,
                                    ctx->audio_buffer,
                                    ctx->audio_buffer_len,
                                    ESP_OK);
    if (err != ESP_OK) {
        return err;
    }

    ctx->audio_buffer_len = 0;
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_flush_audio_group(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    uint8_t output[3];
    uint8_t values[4];
    size_t output_len = 3;
    size_t i;

    if (!ctx || ctx->parser.base64_group_len == 0) {
        return ESP_OK;
    }
    if (ctx->parser.base64_group_len != 4) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (i = 0; i < 4; i++) {
        if (ctx->parser.base64_group[i] == '=') {
            values[i] = 0;
        } else {
            values[i] = tts_engine_provider_xfyun_base64_value(ctx->parser.base64_group[i]);
        }
    }

    output[0] = (uint8_t)((values[0] << 2) | (values[1] >> 4));
    output[1] = (uint8_t)((values[1] << 4) | (values[2] >> 2));
    output[2] = (uint8_t)((values[2] << 6) | values[3]);

    if (ctx->parser.base64_group[3] == '=') {
        output_len--;
    }
    if (ctx->parser.base64_group[2] == '=') {
        output_len--;
    }

    ctx->parser.base64_group_len = 0;
    return tts_engine_provider_xfyun_dispatch_audio(ctx, output, output_len);
}

static esp_err_t tts_engine_provider_xfyun_process_audio_char(tts_engine_provider_xfyun_stream_ctx_t *ctx, char value)
{
    if (isspace((unsigned char)value)) {
        return ESP_OK;
    }

    ctx->parser.base64_group[ctx->parser.base64_group_len++] = value;
    if (ctx->parser.base64_group_len == 4) {
        return tts_engine_provider_xfyun_flush_audio_group(ctx);
    }

    return ESP_OK;
}

static void tts_engine_provider_xfyun_reset_parser(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    memset(&ctx->parser, 0, sizeof(ctx->parser));
}

static tts_engine_provider_xfyun_field_t tts_engine_provider_xfyun_map_field(const tts_engine_provider_xfyun_parser_t *parser,
                                                       const char *key)
{
    if (!parser || !key) {
        return TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
    }

    if (parser->object_depth == 1) {
        if (strcmp(key, "code") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_CODE;
        }
        if (strcmp(key, "sid") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_SID;
        }
        if (strcmp(key, "message") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_MESSAGE;
        }
        if (strcmp(key, "data") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_OBJECT;
        }
    } else if (parser->object_depth == 2 && parser->in_data_object) {
        if (strcmp(key, "status") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_STATUS;
        }
        if (strcmp(key, "audio") == 0) {
            return TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_AUDIO;
        }
    }

    return TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
}

static void tts_engine_provider_xfyun_commit_string_field(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    ctx->parser.value_buf[ctx->parser.value_len] = '\0';

    switch (ctx->parser.active_field) {
    case TTS_ENGINE_PROVIDER_XFYUN_FIELD_SID:
        strlcpy(ctx->status->provider_session_id,
                ctx->parser.value_buf,
                sizeof(ctx->status->provider_session_id));
        break;
    case TTS_ENGINE_PROVIDER_XFYUN_FIELD_MESSAGE:
        strlcpy(ctx->status->provider_message,
                ctx->parser.value_buf,
                sizeof(ctx->status->provider_message));
        break;
    default:
        break;
    }
}

static void tts_engine_provider_xfyun_commit_number_field(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    int value;

    ctx->parser.value_buf[ctx->parser.value_len] = '\0';
    value = atoi(ctx->parser.value_buf);

    switch (ctx->parser.active_field) {
    case TTS_ENGINE_PROVIDER_XFYUN_FIELD_CODE:
        ctx->status->server_code = value;
        break;
    case TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_STATUS:
        ctx->parser.message_status = value;
        ctx->parser.message_status_valid = true;
        break;
    default:
        break;
    }
}

static esp_err_t tts_engine_provider_xfyun_finalize_message(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    esp_err_t err;

    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->parser.string_is_audio && ctx->parser.base64_group_len != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx->status->server_code != 0) {
        ESP_LOGE(TAG,
                 "Provider error sid=%s code=%" PRIi32 " message=%s",
                 ctx->status->provider_session_id,
                 ctx->status->server_code,
                 ctx->status->provider_message[0] ? ctx->status->provider_message : "unknown");
        return ESP_FAIL;
    }

    if (ctx->parser.message_status_valid && ctx->parser.message_status == 2) {
        err = tts_engine_provider_xfyun_flush_audio_buffer(ctx);
        if (err != ESP_OK) {
            return err;
        }

        ctx->status->complete_ms = (int32_t)((esp_timer_get_time() - ctx->send_ts_us) / 1000LL);
        ctx->status->completed = true;
        ctx->done = true;
        err = tts_engine_provider_xfyun_emit_event(ctx, TTS_ENGINE_PROVIDER_EVENT_COMPLETED, NULL, 0, ESP_OK);
        if (err != ESP_OK) {
            return err;
        }
        xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_DONE);
    }

    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_parse_text_chunk(tts_engine_provider_xfyun_stream_ctx_t *ctx,
                                                 const char *data,
                                                 size_t data_len,
                                                 bool message_start,
                                                 bool message_end)
{
    size_t i = 0;
    esp_err_t err = ESP_OK;

    if (!ctx || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (message_start) {
        tts_engine_provider_xfyun_reset_parser(ctx);
    }

    while (i < data_len) {
        char ch = data[i];

        switch (ctx->parser.token) {
        case TTS_ENGINE_PROVIDER_XFYUN_TOKEN_KEY:
            if (ctx->parser.key_escape) {
                ctx->parser.key_escape = false;
                if (ctx->parser.key_len + 1 < sizeof(ctx->parser.key_buf)) {
                    ctx->parser.key_buf[ctx->parser.key_len++] = ch;
                }
                i++;
                continue;
            }
            if (ch == '\\') {
                ctx->parser.key_escape = true;
                i++;
                continue;
            }
            if (ch == '"') {
                ctx->parser.key_buf[ctx->parser.key_len] = '\0';
                ctx->parser.pending_field = tts_engine_provider_xfyun_map_field(&ctx->parser,
                                                                     ctx->parser.key_buf);
                ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE;
                ctx->parser.key_len = 0;
                ctx->parser.expecting_key = false;
                i++;
                continue;
            }
            if (ctx->parser.key_len + 1 < sizeof(ctx->parser.key_buf)) {
                ctx->parser.key_buf[ctx->parser.key_len++] = ch;
            }
            i++;
            continue;

        case TTS_ENGINE_PROVIDER_XFYUN_TOKEN_STRING:
            if (ctx->parser.value_escape) {
                ctx->parser.value_escape = false;
                if (ctx->parser.string_is_audio) {
                    err = tts_engine_provider_xfyun_process_audio_char(ctx, ch);
                    if (err != ESP_OK) {
                        return err;
                    }
                } else if (ctx->parser.value_len + 1 < sizeof(ctx->parser.value_buf)) {
                    ctx->parser.value_buf[ctx->parser.value_len++] = ch;
                }
                i++;
                continue;
            }
            if (ch == '\\') {
                ctx->parser.value_escape = true;
                i++;
                continue;
            }
            if (ch == '"') {
                if (!ctx->parser.string_is_audio) {
                    tts_engine_provider_xfyun_commit_string_field(ctx);
                } else if (ctx->parser.base64_group_len != 0) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE;
                ctx->parser.active_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
                ctx->parser.value_len = 0;
                ctx->parser.string_is_audio = false;
                i++;
                continue;
            }
            if (ctx->parser.string_is_audio) {
                err = tts_engine_provider_xfyun_process_audio_char(ctx, ch);
                if (err != ESP_OK) {
                    return err;
                }
            } else if (ctx->parser.value_len + 1 < sizeof(ctx->parser.value_buf)) {
                ctx->parser.value_buf[ctx->parser.value_len++] = ch;
            }
            i++;
            continue;

        case TTS_ENGINE_PROVIDER_XFYUN_TOKEN_NUMBER:
            if (ch == '-' || (ch >= '0' && ch <= '9')) {
                if (ctx->parser.value_len + 1 < sizeof(ctx->parser.value_buf)) {
                    ctx->parser.value_buf[ctx->parser.value_len++] = ch;
                }
                i++;
                continue;
            }
            tts_engine_provider_xfyun_commit_number_field(ctx);
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE;
            ctx->parser.active_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
            ctx->parser.value_len = 0;
            continue;

        case TTS_ENGINE_PROVIDER_XFYUN_TOKEN_LITERAL:
            if (isalpha((unsigned char)ch)) {
                i++;
                continue;
            }
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE;
            ctx->parser.active_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
            continue;

        case TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE:
        default:
            break;
        }

        if (isspace((unsigned char)ch)) {
            i++;
            continue;
        }
        if (ch == '{') {
            if (ctx->parser.pending_field == TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_OBJECT &&
                ctx->parser.object_depth == 1) {
                ctx->parser.in_data_object = true;
            }
            ctx->parser.object_depth++;
            ctx->parser.expecting_key = true;
            ctx->parser.expecting_value = false;
            ctx->parser.pending_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
            i++;
            continue;
        }
        if (ch == '}') {
            if (ctx->parser.in_data_object && ctx->parser.object_depth == 2) {
                ctx->parser.in_data_object = false;
            }
            if (ctx->parser.object_depth > 0) {
                ctx->parser.object_depth--;
            }
            ctx->parser.expecting_key = false;
            ctx->parser.expecting_value = false;
            ctx->parser.pending_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
            i++;
            continue;
        }
        if (ch == ',') {
            ctx->parser.expecting_key = true;
            ctx->parser.expecting_value = false;
            ctx->parser.pending_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
            i++;
            continue;
        }
        if (ch == ':') {
            ctx->parser.expecting_value = true;
            ctx->parser.expecting_key = false;
            i++;
            continue;
        }
        if (ch == '"' && ctx->parser.expecting_key) {
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_KEY;
            ctx->parser.key_len = 0;
            ctx->parser.key_escape = false;
            i++;
            continue;
        }
        if (ch == '"' && ctx->parser.expecting_value) {
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_STRING;
            ctx->parser.active_field = ctx->parser.pending_field;
            ctx->parser.value_len = 0;
            ctx->parser.value_escape = false;
            ctx->parser.string_is_audio =
                (ctx->parser.active_field == TTS_ENGINE_PROVIDER_XFYUN_FIELD_DATA_AUDIO);
            ctx->parser.expecting_value = false;
            i++;
            continue;
        }
        if ((ch == '-' || (ch >= '0' && ch <= '9')) && ctx->parser.expecting_value) {
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_NUMBER;
            ctx->parser.active_field = ctx->parser.pending_field;
            ctx->parser.value_len = 0;
            ctx->parser.expecting_value = false;
            continue;
        }
        if (isalpha((unsigned char)ch) && ctx->parser.expecting_value) {
            ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_LITERAL;
            ctx->parser.active_field = ctx->parser.pending_field;
            ctx->parser.expecting_value = false;
            i++;
            continue;
        }

        i++;
    }

    if (ctx->parser.token == TTS_ENGINE_PROVIDER_XFYUN_TOKEN_NUMBER) {
        tts_engine_provider_xfyun_commit_number_field(ctx);
        ctx->parser.token = TTS_ENGINE_PROVIDER_XFYUN_TOKEN_IDLE;
        ctx->parser.active_field = TTS_ENGINE_PROVIDER_XFYUN_FIELD_NONE;
        ctx->parser.value_len = 0;
    }

    if (message_end) {
        return tts_engine_provider_xfyun_finalize_message(ctx);
    }

    return ESP_OK;
}

static void tts_engine_provider_xfyun_ws_event_handler(void *handler_args,
                                            esp_event_base_t base,
                                            int32_t event_id,
                                            void *event_data)
{
    tts_engine_provider_xfyun_stream_ctx_t *ctx = (tts_engine_provider_xfyun_stream_ctx_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_err_t err;

    (void)base;

    if (!ctx || !data) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ctx->status->connect_ms =
            (int32_t)((esp_timer_get_time() - ctx->connect_start_us) / 1000LL);
        err = tts_engine_provider_xfyun_emit_event(ctx, TTS_ENGINE_PROVIDER_EVENT_CONNECTED, NULL, 0, ESP_OK);
        if (err != ESP_OK) {
            tts_engine_provider_xfyun_set_error(ctx, err);
            xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED);
            break;
        }
        xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_CONNECTED);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != 0x1) {
            break;
        }
        err = tts_engine_provider_xfyun_parse_text_chunk(
            ctx,
            data->data_ptr,
            (size_t)data->data_len,
            data->payload_offset == 0,
            ((size_t)data->payload_offset + (size_t)data->data_len) ==
                (size_t)(data->payload_len > 0 ? data->payload_len : data->data_len));
        if (err != ESP_OK) {
            tts_engine_provider_xfyun_set_error(ctx, err);
            xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ctx->status->handshake_status = data->error_handle.esp_ws_handshake_status_code;
        ctx->status->transport_errno = data->error_handle.esp_transport_sock_errno;
        ctx->status->transport_err = data->error_handle.esp_tls_last_esp_err;
        ctx->status->tls_stack_err = data->error_handle.esp_tls_stack_err;
        ESP_LOGE(TAG,
                 "WebSocket error handshake=%d transport_err=%s tls_stack=%d errno=%d",
                 ctx->status->handshake_status,
                 esp_err_to_name(ctx->status->transport_err),
                 ctx->status->tls_stack_err,
                 ctx->status->transport_errno);
        tts_engine_provider_xfyun_set_error(ctx, ESP_FAIL);
        xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (!ctx->done) {
            if (ctx->status->handshake_status == 0) {
                ctx->status->handshake_status = data->error_handle.esp_ws_handshake_status_code;
            }
            if (ctx->status->transport_errno == 0) {
                ctx->status->transport_errno = data->error_handle.esp_transport_sock_errno;
            }
            if (ctx->status->transport_err == ESP_OK) {
                ctx->status->transport_err = data->error_handle.esp_tls_last_esp_err;
            }
            if (ctx->status->tls_stack_err == 0) {
                ctx->status->tls_stack_err = data->error_handle.esp_tls_stack_err;
            }
            tts_engine_provider_xfyun_set_error(ctx, ctx->abort_requested ? ESP_ERR_INVALID_STATE : ESP_FAIL);
            xEventGroupSetBits(ctx->event_group, TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED);
        }
        break;

    default:
        break;
    }
}

static void tts_engine_provider_xfyun_cleanup(tts_engine_provider_xfyun_stream_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->client) {
        esp_websocket_unregister_events(ctx->client,
                                        WEBSOCKET_EVENT_ANY,
                                        tts_engine_provider_xfyun_ws_event_handler);
        if (esp_websocket_client_is_connected(ctx->client)) {
            esp_websocket_client_close(ctx->client, pdMS_TO_TICKS(2000));
        } else {
            esp_websocket_client_stop(ctx->client);
        }
        esp_websocket_client_destroy(ctx->client);
        ctx->client = NULL;
    }
}

static esp_err_t tts_engine_provider_xfyun_start(const tts_engine_provider_request_t *request,
                                      tts_engine_provider_stream_status_t *out_status)
{
    StaticEventGroup_t event_group_buffer;
    tts_engine_provider_xfyun_stream_ctx_t ctx = {0};
    tts_engine_provider_xfyun_config_t config;
    esp_websocket_client_config_t ws_config = {0};
    EventBits_t bits;
    SemaphoreHandle_t lock;
    char *url = NULL;
    char *payload = NULL;
    esp_err_t err = ESP_OK;
    int sent;

    if (!request || !request->text || !request->text[0] || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    lock = tts_engine_provider_xfyun_get_lock();
    if (!lock) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_tts_engine_provider_xfyun.configured) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    config = s_tts_engine_provider_xfyun.config;
    xSemaphoreGive(lock);

    tts_engine_provider_xfyun_init_status(out_status, request->service_session_id, &config);

    err = tts_engine_provider_xfyun_sync_time_if_needed();
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = tts_engine_provider_xfyun_build_url(&config, &url);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = tts_engine_provider_xfyun_build_request(&config, request->text, &payload);
    if (err != ESP_OK) {
        goto cleanup;
    }

    ctx.event_group = xEventGroupCreateStatic(&event_group_buffer);
    ctx.request = request;
    ctx.status = out_status;
    ctx.audio_buffer = s_tts_engine_provider_xfyun.audio_buffer;
    ctx.last_err = ESP_OK;
    ctx.audio_frame_bytes = tts_engine_provider_xfyun_parse_audio_frame_bytes(config.audio_format,
                                                                   out_status->channels);
    ctx.audio_chunk_target_len = tts_engine_provider_xfyun_compute_chunk_target_len(out_status->sample_rate_hz,
                                                                         ctx.audio_frame_bytes);

    ESP_LOGI(TAG,
             "Audio chunk target=%u ms -> %u bytes (rate=%" PRIu32 ", ch=%u, frame=%u)",
             TTS_ENGINE_PROVIDER_XFYUN_AUDIO_CHUNK_MS,
             (unsigned)ctx.audio_chunk_target_len,
             out_status->sample_rate_hz,
             out_status->channels,
             ctx.audio_frame_bytes);

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }
    s_tts_engine_provider_xfyun.active_ctx = &ctx;
    xSemaphoreGive(lock);

    err = tts_engine_provider_xfyun_emit_event(&ctx, TTS_ENGINE_PROVIDER_EVENT_STARTED, NULL, 0, ESP_OK);
    if (err != ESP_OK) {
        goto cleanup;
    }

    ws_config.uri = url;
    ws_config.disable_auto_reconnect = true;
    ws_config.crt_bundle_attach = esp_crt_bundle_attach;
    ws_config.network_timeout_ms = 10000;
    ws_config.reconnect_timeout_ms = 0;
    ws_config.buffer_size = TTS_ENGINE_PROVIDER_XFYUN_WS_BUFFER_SIZE;
    ws_config.task_stack = TTS_ENGINE_PROVIDER_XFYUN_WS_STACK_SIZE;
    ws_config.task_prio = TTS_ENGINE_PROVIDER_XFYUN_WS_TASK_PRIO;
    ctx.client = esp_websocket_client_init(&ws_config);
    if (!ctx.client) {
        err = ESP_FAIL;
        goto cleanup;
    }

    err = esp_websocket_register_events(ctx.client,
                                        WEBSOCKET_EVENT_ANY,
                                        tts_engine_provider_xfyun_ws_event_handler,
                                        &ctx);
    if (err != ESP_OK) {
        goto cleanup;
    }

    ctx.connect_start_us = esp_timer_get_time();
    err = esp_websocket_client_start(ctx.client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(ctx.event_group,
                               TTS_ENGINE_PROVIDER_XFYUN_EVENT_CONNECTED | TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(config.timeout_ms ? config.timeout_ms : request->timeout_ms));
    if ((bits & TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED) != 0) {
        err = (ctx.last_err != ESP_OK) ? ctx.last_err : ESP_FAIL;
        goto cleanup;
    }
    if ((bits & TTS_ENGINE_PROVIDER_XFYUN_EVENT_CONNECTED) == 0) {
        err = ctx.abort_requested ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    ctx.send_ts_us = esp_timer_get_time();
    sent = esp_websocket_client_send_text(ctx.client, payload, strlen(payload), pdMS_TO_TICKS(2000));
    if (sent < 0 || sent != (int)strlen(payload)) {
        err = ESP_FAIL;
        goto cleanup;
    }

    bits = xEventGroupWaitBits(ctx.event_group,
                               TTS_ENGINE_PROVIDER_XFYUN_EVENT_DONE | TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(config.timeout_ms ? config.timeout_ms : request->timeout_ms));
    if ((bits & TTS_ENGINE_PROVIDER_XFYUN_EVENT_FAILED) != 0) {
        err = (ctx.last_err != ESP_OK) ? ctx.last_err : ESP_FAIL;
        goto cleanup;
    }
    if ((bits & TTS_ENGINE_PROVIDER_XFYUN_EVENT_DONE) == 0) {
        err = ctx.abort_requested ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto cleanup;
    }

cleanup:
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "TTS start failed err=%s handshake=%d transport_err=%s tls_stack=%d errno=%d server_code=%" PRIi32 " provider_msg=%s",
                 esp_err_to_name(err),
                 out_status->handshake_status,
                 esp_err_to_name(out_status->transport_err),
                 out_status->tls_stack_err,
                 out_status->transport_errno,
                 out_status->server_code,
                 out_status->provider_message[0] ? out_status->provider_message : "-");
        if (ctx.abort_requested) {
            tts_engine_provider_xfyun_emit_event(&ctx, TTS_ENGINE_PROVIDER_EVENT_ABORTED, NULL, 0, err);
        } else {
            tts_engine_provider_xfyun_emit_event(&ctx, TTS_ENGINE_PROVIDER_EVENT_ERROR, NULL, 0, err);
        }
    }
    tts_engine_provider_xfyun_cleanup(&ctx);
    free(url);
    free(payload);

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_tts_engine_provider_xfyun.active_ctx == &ctx) {
            s_tts_engine_provider_xfyun.active_ctx = NULL;
        }
        xSemaphoreGive(lock);
    }

    return err;
}

esp_err_t tts_engine_provider_xfyun_set_config(const tts_engine_provider_xfyun_config_t *config)
{
    SemaphoreHandle_t lock = tts_engine_provider_xfyun_get_lock();
    esp_err_t err;

    if (!config || !lock) {
        return ESP_ERR_INVALID_ARG;
    }
    err = tts_engine_provider_xfyun_time_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_tts_engine_provider_xfyun.config = *config;
    s_tts_engine_provider_xfyun.configured = true;
    xSemaphoreGive(lock);
    return ESP_OK;
}

static esp_err_t tts_engine_provider_xfyun_abort(void)
{
    tts_engine_provider_xfyun_stream_ctx_t *ctx = NULL;
    SemaphoreHandle_t lock = tts_engine_provider_xfyun_get_lock();

    if (!lock) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ctx = s_tts_engine_provider_xfyun.active_ctx;
    if (ctx) {
        ctx->abort_requested = true;
    }
    xSemaphoreGive(lock);

    if (!ctx || !ctx->client) {
        return ESP_OK;
    }

    if (esp_websocket_client_is_connected(ctx->client)) {
        return esp_websocket_client_close(ctx->client, pdMS_TO_TICKS(200));
    }

    return esp_websocket_client_stop(ctx->client);
}

static const tts_engine_provider_ops_t s_tts_engine_provider_xfyun_ops = {
    .provider_name = "xfyun",
    .start = tts_engine_provider_xfyun_start,
    .abort = tts_engine_provider_xfyun_abort,
};

const tts_engine_provider_ops_t *tts_engine_provider_xfyun_get_ops(void)
{
    return &s_tts_engine_provider_xfyun_ops;
}
