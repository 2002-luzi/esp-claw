/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_session_mgr.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "claw_session_mgr";

#define CLAW_SESSION_MGR_MAP_DIRNAME  "chat_map"
#define CLAW_SESSION_MGR_ROOT_SIZE    160
#define CLAW_SESSION_MGR_MAP_ROOT_SIZE 192
#define CLAW_SESSION_MGR_DEFAULT_BASE "default_"

typedef struct {
    bool configured;
    char session_root_dir[CLAW_SESSION_MGR_ROOT_SIZE];
    char mapping_root_dir[CLAW_SESSION_MGR_MAP_ROOT_SIZE];
    SemaphoreHandle_t mutex;
    claw_session_mgr_delete_session_fn_t delete_session;
    void *delete_session_ctx;
} claw_session_mgr_state_t;

static claw_session_mgr_state_t s_session_mgr = {0};

static bool claw_session_mgr_is_alias_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' ||
           ch == '-';
}

bool claw_session_mgr_alias_is_valid(const char *alias)
{
    size_t len;

    if (!alias) {
        return false;
    }
    len = strlen(alias);
    if (len == 0 || len > CLAW_SESSION_MGR_ALIAS_MAX) {
        return false;
    }
    while (*alias) {
        if (!claw_session_mgr_is_alias_char(*alias++)) {
            return false;
        }
    }

    return true;
}

bool claw_session_mgr_is_configured(void)
{
    return s_session_mgr.configured && s_session_mgr.mutex;
}

static bool claw_session_mgr_alias_exists(const claw_session_mgr_alias_map_t *map, const char *alias)
{
    if (!map || !alias || !alias[0]) {
        return false;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        if (strcmp(map->sessions[i], alias) == 0) {
            return true;
        }
    }

    return false;
}

static uint32_t claw_session_mgr_hash(const char *text)
{
    uint32_t hash = 2166136261u;
    const unsigned char *ptr = (const unsigned char *)text;

    while (ptr && *ptr) {
        hash ^= *ptr++;
        hash *= 16777619u;
    }

    return hash;
}

static void claw_session_mgr_sanitize(const char *src, char *dst, size_t dst_size)
{
    size_t off = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && off + 1 < dst_size) {
        char ch = *src++;

        if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
            dst[off++] = ch;
        } else if (off == 0 || dst[off - 1] != '_') {
            dst[off++] = '_';
        }
    }
    if (off > 0 && dst[off - 1] == '_') {
        off--;
    }
    dst[off] = '\0';
}

static esp_err_t claw_session_mgr_ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t claw_session_mgr_require_configured_locked(void)
{
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t claw_session_mgr_build_chat_key(uint32_t agent_id,
                                                 const char *source_channel,
                                                 const char *chat_id,
                                                 char *buf,
                                                 size_t buf_size)
{
    int written;

    if (!source_channel || !source_channel[0] || !chat_id || !chat_id[0] || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(buf, buf_size, "agent:%" PRIu32 ":chat:%s:%s", agent_id, source_channel, chat_id);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t claw_session_mgr_build_mapping_path(const char *chat_key, char *path, size_t path_size)
{
    char safe_key[40];
    uint32_t hash;
    int written;

    if (!chat_key || !chat_key[0] || !path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_session_mgr_sanitize(chat_key, safe_key, sizeof(safe_key));
    if (strlen(safe_key) > 24) {
        safe_key[24] = '\0';
    }
    hash = claw_session_mgr_hash(chat_key);
    written = snprintf(path,
                       path_size,
                       "%s/chat_%s_%08" PRIx32 ".json",
                       s_session_mgr.mapping_root_dir,
                       safe_key[0] ? safe_key : "default",
                       hash);
    if (written < 0 || (size_t)written >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t claw_session_mgr_validate_alias_map(const claw_session_mgr_alias_map_t *map)
{
    if (!map || !map->chat_key[0] ||
            !claw_session_mgr_alias_is_valid(map->current_alias) ||
            map->session_count == 0 ||
            map->session_count > CLAW_SESSION_MGR_MAX_SESSIONS) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        if (!claw_session_mgr_alias_is_valid(map->sessions[i])) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        for (size_t j = i + 1; j < map->session_count; j++) {
            if (strcmp(map->sessions[i], map->sessions[j]) == 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (!claw_session_mgr_alias_exists(map, map->current_alias)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t claw_session_mgr_write_mapping_locked(const claw_session_mgr_alias_map_t *map)
{
    char path[CLAW_SESSION_MGR_PATH_SIZE];
    cJSON *root = NULL;
    cJSON *sessions = NULL;
    char *json = NULL;
    FILE *file = NULL;
    esp_err_t err;

    err = claw_session_mgr_validate_alias_map(map);
    if (err != ESP_OK) {
        return err;
    }
    err = claw_session_mgr_build_mapping_path(map->chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    sessions = cJSON_CreateArray();
    if (!root || !sessions) {
        cJSON_Delete(root);
        cJSON_Delete(sessions);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(root, "chat_key", map->chat_key) ||
            !cJSON_AddStringToObject(root, "current_alias", map->current_alias)) {
        cJSON_Delete(root);
        cJSON_Delete(sessions);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < map->session_count; i++) {
        cJSON *item = cJSON_CreateString(map->sessions[i]);

        if (!item) {
            cJSON_Delete(root);
            cJSON_Delete(sessions);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(sessions, item);
    }
    cJSON_AddItemToObject(root, "sessions", sessions);
    sessions = NULL;

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    file = fopen(path, "wb");
    if (!file) {
        free(json);
        return ESP_FAIL;
    }
    if (fputs(json, file) < 0) {
        fclose(file);
        free(json);
        return ESP_FAIL;
    }
    fclose(file);
    free(json);
    return ESP_OK;
}

static esp_err_t claw_session_mgr_load_mapping_locked(const char *chat_key,
                                                      claw_session_mgr_alias_map_t *out_map)
{
    char path[CLAW_SESSION_MGR_PATH_SIZE];
    char *text = NULL;
    FILE *file = NULL;
    long size = 0;
    cJSON *root = NULL;
    cJSON *item = NULL;
    cJSON *sessions = NULL;
    esp_err_t err;

    if (!chat_key || !chat_key[0] || !out_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_map, 0, sizeof(*out_map));
    err = claw_session_mgr_build_mapping_path(chat_key, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    if (size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(text);
        return ESP_FAIL;
    }
    fclose(file);

    root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "chat_key");
    if (!cJSON_IsString(item) || !item->valuestring || strcmp(item->valuestring, chat_key) != 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(out_map->chat_key, item->valuestring, sizeof(out_map->chat_key));

    item = cJSON_GetObjectItemCaseSensitive(root, "current_alias");
    if (!cJSON_IsString(item) || !claw_session_mgr_alias_is_valid(item->valuestring)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(out_map->current_alias, item->valuestring, sizeof(out_map->current_alias));

    sessions = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    if (!cJSON_IsArray(sessions)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    out_map->session_count = (size_t)cJSON_GetArraySize(sessions);
    if (out_map->session_count == 0 || out_map->session_count > CLAW_SESSION_MGR_MAX_SESSIONS) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (size_t i = 0; i < out_map->session_count; i++) {
        item = cJSON_GetArrayItem(sessions, (int)i);
        if (!cJSON_IsString(item) || !claw_session_mgr_alias_is_valid(item->valuestring)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        strlcpy(out_map->sessions[i], item->valuestring, sizeof(out_map->sessions[i]));
    }
    cJSON_Delete(root);

    return claw_session_mgr_validate_alias_map(out_map);
}

static esp_err_t claw_session_mgr_init_mapping_locked(const char *chat_key,
                                                      claw_session_mgr_alias_map_t *out_map)
{
    if (!chat_key || !chat_key[0] || !out_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_map, 0, sizeof(*out_map));
    strlcpy(out_map->chat_key, chat_key, sizeof(out_map->chat_key));
    strlcpy(out_map->current_alias, "default_01", sizeof(out_map->current_alias));
    out_map->session_count = 1;
    strlcpy(out_map->sessions[0], "default_01", sizeof(out_map->sessions[0]));

    return claw_session_mgr_write_mapping_locked(out_map);
}

static esp_err_t claw_session_mgr_load_or_init_mapping_locked(const char *chat_key,
                                                              claw_session_mgr_alias_map_t *out_map)
{
    esp_err_t err;

    err = claw_session_mgr_load_mapping_locked(chat_key, out_map);
    if (err == ESP_ERR_NOT_FOUND) {
        return claw_session_mgr_init_mapping_locked(chat_key, out_map);
    }

    return err;
}

static esp_err_t claw_session_mgr_build_default_alias(const claw_session_mgr_alias_map_t *map,
                                                      char *alias,
                                                      size_t alias_size)
{
    uint32_t index;
    int written;

    if (!map || !alias || alias_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (index = 1; index <= CLAW_SESSION_MGR_MAX_SESSIONS; index++) {
        written = snprintf(alias, alias_size, "%s%02" PRIu32, CLAW_SESSION_MGR_DEFAULT_BASE, index);
        if (written < 0 || (size_t)written >= alias_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!claw_session_mgr_alias_exists(map, alias)) {
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

static esp_err_t claw_session_mgr_build_alias_session_id(const char *chat_key,
                                                         const char *alias,
                                                         char *buf,
                                                         size_t buf_size,
                                                         size_t *out_len)
{
    int written;

    if (!chat_key || !chat_key[0] || !alias || !alias[0] || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(buf, buf_size, "%s:%s", chat_key, alias);
    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_len) {
        *out_len = (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t claw_session_mgr_build_current_session_id_locked(uint32_t agent_id,
                                                                  const char *source_channel,
                                                                  const char *chat_id,
                                                                  char *buf,
                                                                  size_t buf_size,
                                                                  size_t *out_len)
{
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    claw_session_mgr_alias_map_t map;
    esp_err_t err;

    err = claw_session_mgr_build_chat_key(agent_id, source_channel, chat_id, chat_key, sizeof(chat_key));
    if (err != ESP_OK) {
        return err;
    }

    err = claw_session_mgr_load_or_init_mapping_locked(chat_key, &map);
    if (err != ESP_OK) {
        return err;
    }

    return claw_session_mgr_build_alias_session_id(chat_key, map.current_alias, buf, buf_size, out_len);
}

static const char *claw_session_mgr_event_key(const claw_session_build_context_t *ctx)
{
    if (!ctx) {
        return "";
    }
    return (ctx->message_id && ctx->message_id[0]) ? ctx->message_id : (ctx->event_id ? ctx->event_id : "");
}

static esp_err_t claw_session_mgr_write_default_session_id(const claw_session_build_context_t *ctx,
                                                           char *buf,
                                                           size_t buf_size,
                                                           size_t *out_len)
{
    const char *source_channel = ctx->source_channel ? ctx->source_channel : "";
    const char *chat_id = ctx->chat_id ? ctx->chat_id : "";
    const char *source_cap = ctx->source_cap ? ctx->source_cap : "";
    const char *event_id = ctx->event_id ? ctx->event_id : "";
    int written;

    switch (ctx->session_policy) {
    case CLAW_SESSION_POLICY_CHAT:
        written = snprintf(buf,
                           buf_size,
                           "agent:%" PRIu32 ":chat:%s:%s",
                           ctx->agent_id,
                           source_channel,
                           chat_id);
        break;
    case CLAW_SESSION_POLICY_TRIGGER:
        written = snprintf(buf,
                           buf_size,
                           "agent:%" PRIu32 ":trigger:%s:%s",
                           ctx->agent_id,
                           source_cap[0] ? source_cap : "system",
                           claw_session_mgr_event_key(ctx));
        break;
    case CLAW_SESSION_POLICY_GLOBAL:
        written = snprintf(buf,
                           buf_size,
                           "agent:%" PRIu32 ":global:%s",
                           ctx->agent_id,
                           source_cap[0] ? source_cap : "router");
        break;
    case CLAW_SESSION_POLICY_EPHEMERAL:
        written = snprintf(buf, buf_size, "agent:%" PRIu32 ":ephemeral:%s", ctx->agent_id, event_id);
        break;
    case CLAW_SESSION_POLICY_NOSAVE:
        buf[0] = '\0';
        if (out_len) {
            *out_len = 0;
        }
        return ESP_OK;
    default:
        written = snprintf(buf,
                           buf_size,
                           "agent:%" PRIu32 ":chat:%s:%s",
                           ctx->agent_id,
                           source_channel,
                           chat_id);
        break;
    }

    if (written < 0 || (size_t)written >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_len) {
        *out_len = (size_t)written;
    }

    return ESP_OK;
}

static bool claw_session_mgr_is_alias_aware_chat_context(const claw_session_build_context_t *ctx)
{
    return ctx &&
           ctx->session_policy == CLAW_SESSION_POLICY_CHAT &&
           ctx->source_channel && ctx->source_channel[0] &&
           ctx->chat_id && ctx->chat_id[0];
}

esp_err_t claw_session_mgr_set_session_root_dir(const char *session_root_dir)
{
    int written;
    SemaphoreHandle_t mutex = s_session_mgr.mutex;
    claw_session_mgr_delete_session_fn_t delete_session = s_session_mgr.delete_session;
    void *delete_session_ctx = s_session_mgr.delete_session_ctx;

    if (!session_root_dir || !session_root_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_session_mgr, 0, sizeof(s_session_mgr));
    s_session_mgr.mutex = mutex;
    s_session_mgr.delete_session = delete_session;
    s_session_mgr.delete_session_ctx = delete_session_ctx;
    strlcpy(s_session_mgr.session_root_dir, session_root_dir, sizeof(s_session_mgr.session_root_dir));
    written = snprintf(s_session_mgr.mapping_root_dir,
                       sizeof(s_session_mgr.mapping_root_dir),
                       "%s/%s",
                       session_root_dir,
                       CLAW_SESSION_MGR_MAP_DIRNAME);
    if (written < 0 || (size_t)written >= sizeof(s_session_mgr.mapping_root_dir)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_session_mgr.mutex) {
        s_session_mgr.mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_session_mgr.mutex) {
        return ESP_ERR_NO_MEM;
    }
    if (claw_session_mgr_ensure_dir(s_session_mgr.session_root_dir) != ESP_OK ||
            claw_session_mgr_ensure_dir(s_session_mgr.mapping_root_dir) != ESP_OK) {
        return ESP_FAIL;
    }

    s_session_mgr.configured = true;
    return ESP_OK;
}

esp_err_t claw_session_mgr_set_delete_session_handler(claw_session_mgr_delete_session_fn_t fn,
                                                      void *user_ctx)
{
    if (s_session_mgr.mutex) {
        xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    }
    s_session_mgr.delete_session = fn;
    s_session_mgr.delete_session_ctx = user_ctx;
    if (s_session_mgr.mutex) {
        xSemaphoreGiveRecursive(s_session_mgr.mutex);
    }

    return ESP_OK;
}

esp_err_t claw_session_mgr_build_session_id(const claw_session_build_context_t *ctx,
                                            char *buf,
                                            size_t buf_size,
                                            size_t *out_len)
{
    esp_err_t err;

    if (!ctx || !buf || buf_size == 0 || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    *out_len = 0;

    if (ctx->session_policy == CLAW_SESSION_POLICY_NOSAVE) {
        return ESP_OK;
    }

    if (!claw_session_mgr_is_alias_aware_chat_context(ctx) ||
            !s_session_mgr.configured ||
            !s_session_mgr.mutex) {
        return claw_session_mgr_write_default_session_id(ctx, buf, buf_size, out_len);
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = claw_session_mgr_build_current_session_id_locked(ctx->agent_id,
                                                           ctx->source_channel,
                                                           ctx->chat_id,
                                                           buf,
                                                           buf_size,
                                                           out_len);
    xSemaphoreGiveRecursive(s_session_mgr.mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "Falling back to default session id for agent=%" PRIu32 " chat=%s:%s: %s",
                 ctx->agent_id,
                 ctx->source_channel,
                 ctx->chat_id,
                 esp_err_to_name(err));
        return claw_session_mgr_write_default_session_id(ctx, buf, buf_size, out_len);
    }

    return ESP_OK;
}

esp_err_t claw_session_mgr_new_chat_session(uint32_t agent_id,
                                            const char *source_channel,
                                            const char *chat_id,
                                            const char *requested_alias,
                                            bool has_requested_alias,
                                            char *out_alias,
                                            size_t out_alias_size)
{
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    claw_session_mgr_alias_map_t map;
    char new_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1];
    esp_err_t err;

    if (has_requested_alias && !claw_session_mgr_alias_is_valid(requested_alias)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_alias && out_alias_size > 0) {
        out_alias[0] = '\0';
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = claw_session_mgr_require_configured_locked();
    if (err == ESP_OK) {
        err = claw_session_mgr_build_chat_key(agent_id, source_channel, chat_id, chat_key, sizeof(chat_key));
    }
    if (err == ESP_OK) {
        err = claw_session_mgr_load_mapping_locked(chat_key, &map);
        if (err == ESP_ERR_NOT_FOUND) {
            memset(&map, 0, sizeof(map));
            strlcpy(map.chat_key, chat_key, sizeof(map.chat_key));
            err = ESP_OK;
        }
    }
    if (err == ESP_OK && map.session_count >= CLAW_SESSION_MGR_MAX_SESSIONS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        if (has_requested_alias) {
            if (claw_session_mgr_alias_exists(&map, requested_alias)) {
                err = ESP_ERR_INVALID_STATE;
            } else {
                strlcpy(new_alias, requested_alias, sizeof(new_alias));
            }
        } else {
            err = claw_session_mgr_build_default_alias(&map, new_alias, sizeof(new_alias));
        }
    }
    if (err == ESP_OK) {
        strlcpy(map.sessions[map.session_count], new_alias, sizeof(map.sessions[map.session_count]));
        map.session_count++;
        strlcpy(map.current_alias, new_alias, sizeof(map.current_alias));
        err = claw_session_mgr_write_mapping_locked(&map);
    }
    xSemaphoreGiveRecursive(s_session_mgr.mutex);

    if (err == ESP_OK && out_alias && out_alias_size > 0) {
        strlcpy(out_alias, new_alias, out_alias_size);
    }

    return err;
}

esp_err_t claw_session_mgr_list_chat_sessions(uint32_t agent_id,
                                             const char *source_channel,
                                             const char *chat_id,
                                             claw_session_mgr_alias_map_t *out_map)
{
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    esp_err_t err;

    if (!out_map) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_map, 0, sizeof(*out_map));
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = claw_session_mgr_require_configured_locked();
    if (err == ESP_OK) {
        err = claw_session_mgr_build_chat_key(agent_id, source_channel, chat_id, chat_key, sizeof(chat_key));
    }
    if (err == ESP_OK) {
        err = claw_session_mgr_load_or_init_mapping_locked(chat_key, out_map);
    }
    xSemaphoreGiveRecursive(s_session_mgr.mutex);

    return err;
}

esp_err_t claw_session_mgr_switch_chat_session(uint32_t agent_id,
                                               const char *source_channel,
                                               const char *chat_id,
                                               const char *alias,
                                               char *out_alias,
                                               size_t out_alias_size)
{
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    claw_session_mgr_alias_map_t map;
    esp_err_t err;

    if (!claw_session_mgr_alias_is_valid(alias)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_alias && out_alias_size > 0) {
        out_alias[0] = '\0';
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = claw_session_mgr_require_configured_locked();
    if (err == ESP_OK) {
        err = claw_session_mgr_build_chat_key(agent_id, source_channel, chat_id, chat_key, sizeof(chat_key));
    }
    if (err == ESP_OK) {
        err = claw_session_mgr_load_or_init_mapping_locked(chat_key, &map);
    }
    if (err == ESP_OK && !claw_session_mgr_alias_exists(&map, alias)) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        strlcpy(map.current_alias, alias, sizeof(map.current_alias));
        err = claw_session_mgr_write_mapping_locked(&map);
    }
    xSemaphoreGiveRecursive(s_session_mgr.mutex);

    if (err == ESP_OK && out_alias && out_alias_size > 0) {
        strlcpy(out_alias, alias, out_alias_size);
    }

    return err;
}

esp_err_t claw_session_mgr_delete_chat_session(uint32_t agent_id,
                                               const char *source_channel,
                                               const char *chat_id,
                                               const char *alias,
                                               char *out_alias,
                                               size_t out_alias_size)
{
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    char session_id[CLAW_SESSION_MGR_ID_SIZE];
    claw_session_mgr_alias_map_t map;
    bool deleted_any = false;
    size_t alias_index = CLAW_SESSION_MGR_MAX_SESSIONS;
    esp_err_t err;

    if (!claw_session_mgr_alias_is_valid(alias)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_alias && out_alias_size > 0) {
        out_alias[0] = '\0';
    }
    if (!s_session_mgr.configured || !s_session_mgr.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(s_session_mgr.mutex, portMAX_DELAY);
    err = claw_session_mgr_require_configured_locked();
    if (err == ESP_OK) {
        err = claw_session_mgr_build_chat_key(agent_id, source_channel, chat_id, chat_key, sizeof(chat_key));
    }
    if (err == ESP_OK) {
        err = claw_session_mgr_load_mapping_locked(chat_key, &map);
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < map.session_count; i++) {
            if (strcmp(map.sessions[i], alias) == 0) {
                alias_index = i;
                break;
            }
        }
        if (alias_index == CLAW_SESSION_MGR_MAX_SESSIONS) {
            err = ESP_ERR_NOT_FOUND;
        } else if (strcmp(map.current_alias, alias) == 0) {
            err = ESP_ERR_INVALID_STATE;
        } else if (!s_session_mgr.delete_session) {
            err = ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (err == ESP_OK) {
        err = claw_session_mgr_build_alias_session_id(chat_key, alias, session_id, sizeof(session_id), NULL);
    }
    if (err == ESP_OK) {
        err = s_session_mgr.delete_session(session_id, &deleted_any, s_session_mgr.delete_session_ctx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Delete session history failed for %s: %s", session_id, esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        for (size_t i = alias_index; i + 1 < map.session_count; i++) {
            strlcpy(map.sessions[i], map.sessions[i + 1], sizeof(map.sessions[i]));
        }
        map.session_count--;
        map.sessions[map.session_count][0] = '\0';
        err = claw_session_mgr_write_mapping_locked(&map);
    }
    xSemaphoreGiveRecursive(s_session_mgr.mutex);

    if (err == ESP_OK) {
        if (out_alias && out_alias_size > 0) {
            strlcpy(out_alias, alias, out_alias_size);
        }
        ESP_LOGI(TAG,
                 "Deleted chat session %s alias=%s history_deleted=%s",
                 chat_key,
                 alias,
                 deleted_any ? "true" : "false");
    }

    return err;
}
