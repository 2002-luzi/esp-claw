/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"
#include "claw_task.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "claw_memory";

#define CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN 4
#define CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE (6 * 1024)
#define CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY 5
#define CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS pdMS_TO_TICKS(60000)

typedef struct claw_memory_pending_summary {
    char *session_id;
    char *summary_list;
    struct claw_memory_pending_summary *next;
} claw_memory_pending_summary_t;

typedef struct claw_memory_async_extract_job {
    uint32_t request_id;
    char *session_id;
    char *user_text;
    char *llm_text;
    claw_memory_message_intent_t message_intent;
    TickType_t created_ticks;
    TickType_t completed_ticks;
    esp_err_t result;
    bool completed;
    SemaphoreHandle_t done_sem;
    struct claw_memory_async_extract_job *next;
} claw_memory_async_extract_job_t;

typedef struct claw_memory_request_state {
    uint32_t request_id;
    bool manual_write;
    struct claw_memory_request_state *next;
} claw_memory_request_state_t;

typedef struct {
    bool enabled;
    QueueHandle_t queue;
    SemaphoreHandle_t lock;
    TaskHandle_t task_handle;
    claw_llm_runtime_t *runtime;
    claw_memory_async_extract_job_t *jobs;
} claw_memory_async_extract_state_t;

static claw_memory_pending_summary_t *s_pending_summaries = NULL;
static claw_memory_async_extract_state_t s_async_extract = {0};
static claw_memory_request_state_t *s_request_states = NULL;

typedef struct {
    uint32_t offset;
    uint32_t length;
} claw_memory_session_index_entry_t;

#define CLAW_MEMORY_SESSION_HEADER_FIXED_BYTES \
    ((sizeof(uint32_t) * 4) + \
     (sizeof(claw_memory_session_index_entry_t) * CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS) + \
     (sizeof(uint8_t) * CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS) + \
     (sizeof(uint8_t) * CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS))

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_slots;
    uint32_t total_records;
    claw_memory_session_index_entry_t entries[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
    uint8_t record_types[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
    uint8_t backend_formats[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
    uint8_t reserved[CLAW_MEMORY_SESSION_RAW_HEADER_SIZE -
                     CLAW_MEMORY_SESSION_HEADER_FIXED_BYTES];
} claw_memory_session_header_t;

_Static_assert(sizeof(claw_memory_session_header_t) == CLAW_MEMORY_SESSION_RAW_HEADER_SIZE,
               "session history raw header size must remain fixed");
_Static_assert(CLAW_MEMORY_SESSION_HEADER_FIXED_BYTES <= CLAW_MEMORY_SESSION_RAW_HEADER_SIZE,
               "session history raw header must fit fixed metadata");
_Static_assert(CLAW_MEMORY_SESSION_HEADER_SIZE ==
               (((CLAW_MEMORY_SESSION_RAW_HEADER_SIZE + 2) / 3) * 4) + 1,
               "session history file header must fit base64 header plus newline");

static claw_memory_pending_summary_t *claw_memory_find_pending_summary(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

static esp_err_t claw_memory_pending_summary_append(const char *session_id, const char *summary_list)
{
    claw_memory_pending_summary_t *node = NULL;

    if (!session_id || !session_id[0] || !summary_list || !summary_list[0]) {
        return ESP_OK;
    }

    node = claw_memory_find_pending_summary(session_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->session_id = dup_printf("%s", session_id);
        if (!node->session_id) {
            free(node);
            return ESP_ERR_NO_MEM;
        }
        node->next = s_pending_summaries;
        s_pending_summaries = node;
    }

    return line_list_merge_unique(&node->summary_list, summary_list);
}

static char *claw_memory_pending_summary_take_summary_list(const char *session_id)
{
    claw_memory_pending_summary_t *node = s_pending_summaries;
    claw_memory_pending_summary_t *prev = NULL;
    char *summary_list = NULL;

    if (!session_id || !session_id[0]) {
        return NULL;
    }

    while (node) {
        if (node->session_id && strcmp(node->session_id, session_id) == 0) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return NULL;
    }

    summary_list = node->summary_list;
    node->summary_list = NULL;
    if (prev) {
        prev->next = node->next;
    } else {
        s_pending_summaries = node->next;
    }
    free(node->session_id);
    free(node);
    return summary_list;
}

static claw_memory_request_state_t *claw_memory_find_request_state(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;

    while (node) {
        if (node->request_id == request_id) {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

esp_err_t claw_memory_request_mark_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = NULL;

    if (request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    node = claw_memory_find_request_state(request_id);
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) {
            return ESP_ERR_NO_MEM;
        }
        node->request_id = request_id;
        node->next = s_request_states;
        s_request_states = node;
    }

    node->manual_write = true;
    return ESP_OK;
}

static bool claw_memory_request_take_manual_write(uint32_t request_id)
{
    claw_memory_request_state_t *node = s_request_states;
    claw_memory_request_state_t *prev = NULL;
    bool manual_write = false;

    if (request_id == 0) {
        return false;
    }

    while (node) {
        if (node->request_id == request_id) {
            break;
        }
        prev = node;
        node = node->next;
    }
    if (!node) {
        return false;
    }

    manual_write = node->manual_write;
    if (prev) {
        prev->next = node->next;
    } else {
        s_request_states = node->next;
    }
    free(node);
    return manual_write;
}

static claw_memory_async_extract_job_t *claw_memory_async_extract_find_job_locked(uint32_t request_id)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    while (job) {
        if (job->request_id == request_id) {
            return job;
        }
        job = job->next;
    }
    return NULL;
}

static void claw_memory_async_extract_free_job(claw_memory_async_extract_job_t *job)
{
    if (!job) {
        return;
    }
    if (job->done_sem) {
        vSemaphoreDelete(job->done_sem);
    }
    free(job->session_id);
    free(job->user_text);
    free(job->llm_text);
    free(job);
}

static void claw_memory_async_extract_sweep_locked(TickType_t now_ticks)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;
    claw_memory_async_extract_job_t *prev = NULL;

    while (job) {
        claw_memory_async_extract_job_t *next = job->next;
        bool expired = job->completed &&
                       (now_ticks - job->completed_ticks) >= CLAW_MEMORY_ASYNC_EXTRACT_SWEEP_TICKS;

        if (expired) {
            if (prev) {
                prev->next = next;
            } else {
                s_async_extract.jobs = next;
            }
            claw_memory_async_extract_free_job(job);
        } else {
            prev = job;
        }
        job = next;
    }
}

static void claw_memory_async_extract_task(void *arg)
{
    (void)arg;

    while (true) {
        claw_memory_async_extract_job_t *job = NULL;
        char *llm_text = NULL;
        claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;
        esp_err_t err;

        if (xQueueReceive(s_async_extract.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!job) {
            continue;
        }

        err = claw_memory_auto_extract_prepare_with_runtime(s_async_extract.runtime,
                                                            job->user_text,
                                                            &message_intent,
                                                            &llm_text);

        if (s_async_extract.lock && xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            if (job->llm_text) {
                free(job->llm_text);
            }
            job->llm_text = llm_text;
            job->message_intent = message_intent;
            job->result = err;
            job->completed = true;
            job->completed_ticks = xTaskGetTickCount();
            xSemaphoreGive(s_async_extract.lock);
        } else {
            free(llm_text);
        }

        if (job->done_sem) {
            xSemaphoreGive(job->done_sem);
        }
    }
}

static char *claw_memory_async_extract_take_summary_list(const claw_core_request_t *request,
                                                        bool apply_result)
{
    claw_memory_async_extract_job_t *job = NULL;
    claw_memory_async_extract_job_t *prev = NULL;
    SemaphoreHandle_t done_sem = NULL;
    char *llm_text = NULL;
    char *summary_list = NULL;
    claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;

    if (!request || !request->request_id || !s_async_extract.enabled || !s_async_extract.lock) {
        return NULL;
    }

    while (true) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }

        prev = NULL;
        job = s_async_extract.jobs;
        while (job) {
            if (job->request_id == request->request_id) {
                break;
            }
            prev = job;
            job = job->next;
        }

        if (!job) {
            claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
            xSemaphoreGive(s_async_extract.lock);
            return NULL;
        }

        if (job->completed) {
            llm_text = job->llm_text;
            job->llm_text = NULL;
            message_intent = job->message_intent;
            if (prev) {
                prev->next = job->next;
            } else {
                s_async_extract.jobs = job->next;
            }
            xSemaphoreGive(s_async_extract.lock);
            claw_memory_async_extract_free_job(job);
            if (!apply_result) {
                free(llm_text);
                return NULL;
            }
            if (claw_memory_auto_extract_apply_result(llm_text,
                                                      message_intent,
                                                      &summary_list) != ESP_OK) {
                free(llm_text);
                free(summary_list);
                return NULL;
            }
            free(llm_text);
            return summary_list;
        }

        done_sem = job->done_sem;
        xSemaphoreGive(s_async_extract.lock);
        ESP_LOGI(TAG, "stage note provider waiting request=%" PRIu32, request->request_id);
        if (!done_sem || xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
            return NULL;
        }
    }
}

static void claw_memory_async_extract_deinit(void)
{
    claw_memory_async_extract_job_t *job = s_async_extract.jobs;

    s_async_extract.jobs = NULL;
    while (job) {
        claw_memory_async_extract_job_t *next = job->next;

        claw_memory_async_extract_free_job(job);
        job = next;
    }
    if (s_async_extract.task_handle) {
        claw_task_delete(s_async_extract.task_handle);
        s_async_extract.task_handle = NULL;
    }
    if (s_async_extract.queue) {
        vQueueDelete(s_async_extract.queue);
        s_async_extract.queue = NULL;
    }
    if (s_async_extract.lock) {
        vSemaphoreDelete(s_async_extract.lock);
        s_async_extract.lock = NULL;
    }
    if (s_async_extract.runtime) {
        claw_llm_runtime_deinit(s_async_extract.runtime);
        s_async_extract.runtime = NULL;
    }
    s_async_extract.enabled = false;
}

esp_err_t claw_memory_async_extract_init(const claw_memory_config_t *config)
{
    BaseType_t task_result;
    const claw_memory_llm_config_t *llm = NULL;
    char *error_message = NULL;
    esp_err_t err;

    claw_memory_async_extract_deinit();

    if (!config || !config->enable_async_extract_stage_note) {
        return ESP_OK;
    }
    llm = &config->llm;

    if (!llm->api_key || !llm->api_key[0] ||
        !llm->model || !llm->model[0] ||
        !llm->backend_type || !llm->backend_type[0]) {
        ESP_LOGI(TAG, "Async memory extract disabled: LLM config incomplete");
        return ESP_OK;
    }

    err = claw_llm_runtime_init(&s_async_extract.runtime,
                                &(claw_llm_runtime_config_t) {
                                    .api_key = llm->api_key,
                                    .backend_type = llm->backend_type,
                                    .model = llm->model,
                                    .base_url = llm->base_url,
                                    .auth_type = llm->auth_type,
                                    .max_tokens_field = llm->max_tokens_field,
                                    .timeout_ms = llm->timeout_ms,
                                    .max_tokens = llm->max_tokens,
                                    .image_max_bytes = llm->image_max_bytes,
                                    .supports_tools = llm->supports_tools,
                                    .supports_vision = llm->supports_vision,
                                    .image_remote_url_only = llm->image_remote_url_only,
                                },
                                &error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to init async memory extract runtime: %s",
                 error_message ? error_message : esp_err_to_name(err));
        free(error_message);
        claw_memory_async_extract_deinit();
        return err;
    }
    free(error_message);

    s_async_extract.lock = xSemaphoreCreateMutex();
    s_async_extract.queue = xQueueCreate(CLAW_MEMORY_ASYNC_EXTRACT_QUEUE_LEN,
                                         sizeof(claw_memory_async_extract_job_t *));
    if (!s_async_extract.lock || !s_async_extract.queue) {
        claw_memory_async_extract_deinit();
        return ESP_ERR_NO_MEM;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = "claw_mem_extract",
                                        .stack_size = CLAW_MEMORY_ASYNC_EXTRACT_STACK_SIZE,
                                        .priority = CLAW_MEMORY_ASYNC_EXTRACT_PRIORITY,
                                        .core_id = tskNO_AFFINITY,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_memory_async_extract_task,
                                    NULL,
                                    &s_async_extract.task_handle);
    if (task_result != pdPASS) {
        claw_memory_async_extract_deinit();
        return ESP_FAIL;
    }

    s_async_extract.enabled = true;
    ESP_LOGI(TAG, "Async memory extract worker ready");
    return ESP_OK;
}

esp_err_t claw_memory_async_extract_ensure_started(const claw_core_request_t *request)
{
    claw_memory_async_extract_job_t *job = NULL;

    if (!request || !request->request_id || !request->session_id || !request->session_id[0] ||
        !request->user_text || !request->user_text[0] || !s_async_extract.enabled) {
        return ESP_OK;
    }
    if (!s_async_extract.lock || !s_async_extract.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    claw_memory_async_extract_sweep_locked(xTaskGetTickCount());
    if (claw_memory_async_extract_find_job_locked(request->request_id)) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_OK;
    }

    job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(s_async_extract.lock);
        return ESP_ERR_NO_MEM;
    }

    job->request_id = request->request_id;
    job->session_id = dup_printf("%s", request->session_id);
    job->user_text = dup_printf("%s", request->user_text);
    job->done_sem = xSemaphoreCreateBinary();
    job->created_ticks = xTaskGetTickCount();
    if (!job->session_id || !job->user_text || !job->done_sem) {
        xSemaphoreGive(s_async_extract.lock);
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_NO_MEM;
    }

    job->next = s_async_extract.jobs;
    s_async_extract.jobs = job;
    xSemaphoreGive(s_async_extract.lock);

    if (xQueueSend(s_async_extract.queue, &job, 0) != pdTRUE) {
        if (xSemaphoreTake(s_async_extract.lock, portMAX_DELAY) == pdTRUE) {
            claw_memory_async_extract_job_t *node = s_async_extract.jobs;
            claw_memory_async_extract_job_t *prev = NULL;

            while (node) {
                if (node == job) {
                    if (prev) {
                        prev->next = node->next;
                    } else {
                        s_async_extract.jobs = node->next;
                    }
                    break;
                }
                prev = node;
                node = node->next;
            }
            xSemaphoreGive(s_async_extract.lock);
        }
        claw_memory_async_extract_free_job(job);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "async extract job created request=%" PRIu32 " session=%s",
             request->request_id,
             request->session_id);
    return ESP_OK;
}

static size_t session_history_effective_max_slots(void)
{
    static bool plain_clamp_logged = false;
    static bool tool_clamp_logged = false;
    size_t plain_slots = s_memory.max_session_messages ? s_memory.max_session_messages :
        CLAW_MEMORY_DEFAULT_MAX_SESSION_MESSAGES;
    size_t tool_iterations = s_memory.max_tool_iterations ? s_memory.max_tool_iterations :
        CLAW_MEMORY_DEFAULT_MAX_TOOL_ITERATIONS;
    size_t tool_turn_slots;
    size_t max_slots;

    if (plain_slots > CLAW_MEMORY_DEFAULT_MAX_SESSION_MESSAGES) {
        if (!plain_clamp_logged) {
            ESP_LOGW(TAG,
                     "Session history plain retention clamped from %u to %u records",
                     (unsigned)plain_slots,
                     (unsigned)CLAW_MEMORY_DEFAULT_MAX_SESSION_MESSAGES);
            plain_clamp_logged = true;
        }
        plain_slots = CLAW_MEMORY_DEFAULT_MAX_SESSION_MESSAGES;
    }
    if (tool_iterations > CLAW_MEMORY_MAX_TOOL_ITERATIONS) {
        if (!tool_clamp_logged) {
            ESP_LOGW(TAG,
                     "Session history tool retention clamped from %u to %u iterations",
                     (unsigned)tool_iterations,
                     (unsigned)CLAW_MEMORY_MAX_TOOL_ITERATIONS);
            tool_clamp_logged = true;
        }
        tool_iterations = CLAW_MEMORY_MAX_TOOL_ITERATIONS;
    }

    tool_turn_slots = (CLAW_MEMORY_SESSION_RECENT_TOOL_TURNS +
                       CLAW_MEMORY_SESSION_UNFINISHED_TOOL_TURNS) *
                      tool_iterations *
                      CLAW_MEMORY_SESSION_TOOL_RECORDS_PER_ROUND;
    max_slots = plain_slots + tool_turn_slots;
    if (max_slots > CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS) {
        max_slots = CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS;
    }
    if (max_slots == 0) {
        max_slots = 1;
    }
    return max_slots;
}

static void session_history_header_init(claw_memory_session_header_t *header,
                                        size_t max_slots)
{
    memset(header, 0, sizeof(*header));
    header->magic = CLAW_MEMORY_SESSION_HEADER_MAGIC;
    header->version = CLAW_MEMORY_SESSION_HEADER_VERSION;
    header->max_slots = (uint32_t)max_slots;
}

static size_t session_history_retained_count(const claw_memory_session_header_t *header);
static size_t session_history_retained_slot(const claw_memory_session_header_t *header,
                                            size_t index);

static bool session_history_record_type_valid(uint8_t record_type)
{
    switch ((claw_memory_record_type_t)record_type) {
    case CLAW_MEMORY_RECORD_TYPE_USER:
    case CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL:
    case CLAW_MEMORY_RECORD_TYPE_ASSISTANT_TOOL:
    case CLAW_MEMORY_RECORD_TYPE_TOOL_RESULT:
        return true;
    default:
        return false;
    }
}

static bool session_history_backend_format_valid(uint8_t backend_format)
{
    switch ((claw_memory_backend_format_t)backend_format) {
    case CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN:
    case CLAW_MEMORY_BACKEND_FORMAT_OPENAI:
    case CLAW_MEMORY_BACKEND_FORMAT_ANTHROPIC:
        return true;
    default:
        return false;
    }
}

static bool session_history_header_valid(const claw_memory_session_header_t *header)
{
    size_t count;
    size_t i;

    if (!header) {
        return false;
    }
    if (header->magic != CLAW_MEMORY_SESSION_HEADER_MAGIC) {
        ESP_LOGW(TAG, "Invalid session history header magic");
        return false;
    }
    if (header->version != CLAW_MEMORY_SESSION_HEADER_VERSION) {
        ESP_LOGW(TAG,
                 "Unsupported session history header version %" PRIu32,
                 header->version);
        return false;
    }
    if (header->max_slots == 0 ||
            header->max_slots > CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS) {
        ESP_LOGW(TAG,
                 "Invalid session history max_slots %" PRIu32,
                 header->max_slots);
        return false;
    }
    count = session_history_retained_count(header);
    for (i = 0; i < count; i++) {
        size_t slot = session_history_retained_slot(header, i);

        if (!session_history_record_type_valid(header->record_types[slot])) {
            ESP_LOGW(TAG,
                     "Invalid session history record_type slot=%u type=%u",
                     (unsigned)slot,
                     (unsigned)header->record_types[slot]);
            return false;
        }
        if (!session_history_backend_format_valid(header->backend_formats[slot])) {
            ESP_LOGW(TAG,
                     "Invalid session history backend_format slot=%u format=%u",
                     (unsigned)slot,
                     (unsigned)header->backend_formats[slot]);
            return false;
        }
    }
    return true;
}

static esp_err_t session_history_read_header(FILE *file,
                                             claw_memory_session_header_t *header)
{
    unsigned char encoded[CLAW_MEMORY_SESSION_HEADER_SIZE];
    size_t decoded_len = 0;
    size_t read_len;
    int ret;

    if (!file || !header) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history header failed");
        return ESP_FAIL;
    }

    memset(header, 0, sizeof(*header));
    read_len = fread(encoded, 1, CLAW_MEMORY_SESSION_HEADER_SIZE, file);
    if (read_len != CLAW_MEMORY_SESSION_HEADER_SIZE) {
        if (ferror(file)) {
            ESP_LOGE(TAG, "read session history header failed");
            return ESP_FAIL;
        }
        ESP_LOGW(TAG,
                 "Session history header is missing or short (%u/%u bytes)",
                 (unsigned)read_len,
                 (unsigned)CLAW_MEMORY_SESSION_HEADER_SIZE);
        return ESP_ERR_INVALID_STATE;
    }
    if (encoded[CLAW_MEMORY_SESSION_HEADER_SIZE - 1] != '\n') {
        ESP_LOGW(TAG, "Session history base64 header separator missing");
        return ESP_ERR_INVALID_STATE;
    }

    ret = mbedtls_base64_decode((unsigned char *)header,
                                sizeof(*header),
                                &decoded_len,
                                encoded,
                                CLAW_MEMORY_SESSION_HEADER_SIZE - 1);
    if (ret != 0 || decoded_len != sizeof(*header)) {
        ESP_LOGW(TAG, "Invalid session history base64 header");
        return ESP_ERR_INVALID_STATE;
    }
    if (!session_history_header_valid(header)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t session_history_write_header(FILE *file,
                                              const claw_memory_session_header_t *header)
{
    unsigned char encoded[CLAW_MEMORY_SESSION_HEADER_SIZE];
    size_t encoded_len = 0;

    if (!file || !header) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!session_history_header_valid(header)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mbedtls_base64_encode(encoded,
                              sizeof(encoded),
                              &encoded_len,
                              (const unsigned char *)header,
                              sizeof(*header)) != 0 ||
            encoded_len != CLAW_MEMORY_SESSION_HEADER_SIZE - 1) {
        ESP_LOGE(TAG, "encode session history header failed");
        return ESP_FAIL;
    }
    encoded[encoded_len] = '\n';

    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history header for write failed");
        return ESP_FAIL;
    }
    if (fwrite(encoded, 1, sizeof(encoded), file) != sizeof(encoded)) {
        ESP_LOGE(TAG, "write session history header failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static size_t session_history_retained_count(const claw_memory_session_header_t *header)
{
    if (!header || header->max_slots == 0) {
        return 0;
    }
    return header->total_records < header->max_slots ?
        header->total_records : header->max_slots;
}

static size_t session_history_retained_slot(const claw_memory_session_header_t *header,
                                            size_t index)
{
    size_t oldest_slot;

    if (!header || header->max_slots == 0) {
        return 0;
    }

    oldest_slot = (header->total_records < header->max_slots) ?
        0 : (header->total_records % header->max_slots);
    return (oldest_slot + index) % header->max_slots;
}

static size_t session_history_record_object_len(const claw_memory_session_index_entry_t *entry)
{
    if (!entry || entry->length == 0) {
        return 0;
    }
    return entry->length - 1;
}

static esp_err_t session_history_measure_indexed(const claw_memory_session_header_t *header,
                                                 size_t *out_count,
                                                 size_t *out_json_size)
{
    size_t count;
    size_t json_size = 3; /* '[' + ']' + trailing NUL */
    size_t i;

    if (!header || !out_count || !out_json_size) {
        return ESP_ERR_INVALID_ARG;
    }

    count = session_history_retained_count(header);
    if (count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (count > 1) {
        json_size += count - 1;
    }

    for (i = 0; i < count; i++) {
        size_t slot = session_history_retained_slot(header, i);
        const claw_memory_session_index_entry_t *entry = &header->entries[slot];
        size_t object_len = session_history_record_object_len(entry);

        if (entry->offset < CLAW_MEMORY_SESSION_HEADER_SIZE || object_len == 0) {
            ESP_LOGW(TAG,
                     "Invalid session history entry slot=%u offset=%" PRIu32 " length=%" PRIu32,
                     (unsigned)slot,
                     entry->offset,
                     entry->length);
            return ESP_ERR_INVALID_STATE;
        }
        json_size += object_len;
    }

    *out_count = count;
    *out_json_size = json_size;
    return ESP_OK;
}

static esp_err_t session_history_render_indexed_json(FILE *file,
                                                     const claw_memory_session_header_t *header,
                                                     size_t count,
                                                     char *json,
                                                     size_t json_size)
{
    char *cursor = json;
    char *expected_end = json + json_size - 1;
    size_t i;

    if (!file || !header || !json || json_size < 3 || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *cursor++ = '[';
    for (i = 0; i < count; i++) {
        size_t slot = session_history_retained_slot(header, i);
        const claw_memory_session_index_entry_t *entry = &header->entries[slot];
        size_t object_len = session_history_record_object_len(entry);

        if (i > 0) {
            *cursor++ = ',';
        }
        if (fseek(file, (long)entry->offset, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "seek session history record failed");
            return ESP_FAIL;
        }
        if (fread(cursor, 1, object_len, file) != object_len) {
            ESP_LOGE(TAG, "read session history record failed");
            return ESP_FAIL;
        }
        cursor += object_len;
    }
    *cursor++ = ']';
    *cursor = '\0';

    if (cursor != expected_end) {
        ESP_LOGE(TAG, "session history json size mismatch");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t session_history_close_file(FILE *file)
{
    if (!file) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "close session history failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t session_history_recreate_file(const char *path,
                                               FILE **out_file,
                                               claw_memory_session_header_t *header)
{
    FILE *file = NULL;
    esp_err_t err;

    if (!path || !out_file || !header) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "w+b");
    if (!file) {
        ESP_LOGE(TAG, "create session history %s failed: errno=%d", path, errno);
        return ESP_FAIL;
    }

    session_history_header_init(header, session_history_effective_max_slots());
    err = session_history_write_header(file, header);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    *out_file = file;
    return ESP_OK;
}

static esp_err_t session_history_validate_json_array(const char *json)
{
    cJSON *root = NULL;

    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_ParseWithOpts(json, NULL, 1);
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t claw_memory_session_load_json_alloc(const char *session_id, char **out_json)
{
    char *path = NULL;
    FILE *file = NULL;
    claw_memory_session_header_t header;
    char *json = NULL;
    size_t count = 0;
    size_t json_size = 0;
    esp_err_t err;
    bool reset_file = false;
    const char *reset_reason = NULL;

    if (!session_id || !out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_json = NULL;
    path = claw_memory_session_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    file = fopen(path, "rb");
    if (!file) {
        err = (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "open session history %s failed: errno=%d", path, errno);
        }
        goto cleanup;
    }

    err = session_history_read_header(file, &header);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "invalid header";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = session_history_measure_indexed(&header, &count, &json_size);
    if (err == ESP_ERR_INVALID_STATE) {
        reset_file = true;
        reset_reason = "invalid index";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    json = calloc(1, json_size);
    if (!json) {
        ESP_LOGE(TAG, "allocate session history json failed");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = session_history_render_indexed_json(file, &header, count, json, json_size);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "read indexed records failed";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = session_history_validate_json_array(json);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "invalid rendered json";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (reset_file && path) {
        FILE *reset_file_handle = NULL;
        claw_memory_session_header_t reset_header;
        esp_err_t reset_err;

        ESP_LOGW(TAG, "Resetting session history %s: %s", path, reset_reason);
        reset_err = session_history_recreate_file(path, &reset_file_handle, &reset_header);
        if (reset_err == ESP_OK) {
            reset_err = session_history_close_file(reset_file_handle);
        }
        if (reset_err != ESP_OK) {
            ESP_LOGE(TAG, "reset session history %s failed: %s",
                     path,
                     esp_err_to_name(reset_err));
            err = reset_err;
        }
    }
    free(path);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    *out_json = json;
    return ESP_OK;
}

static esp_err_t session_history_open_for_append(const char *path,
                                                 FILE **out_file,
                                                 claw_memory_session_header_t *header)
{
    FILE *file = NULL;
    esp_err_t err;

    if (!path || !out_file || !header) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_file = NULL;

    file = fopen(path, "r+b");
    if (!file) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "open session history %s failed: errno=%d", path, errno);
            return ESP_FAIL;
        }
        return session_history_recreate_file(path, out_file, header);
    }

    err = session_history_read_header(file, header);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Reinitializing legacy or invalid session history file %s", path);
        if (session_history_close_file(file) != ESP_OK) {
            return ESP_FAIL;
        }
        return session_history_recreate_file(path, out_file, header);
    }
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    *out_file = file;
    return ESP_OK;
}

static esp_err_t session_history_append_indexed_record(FILE *file,
                                                       claw_memory_session_header_t *header,
                                                       claw_memory_record_type_t record_type,
                                                       const char *role,
                                                       const char *text)
{
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t slot;
    esp_err_t err;

    if (!file || !header || !role || !text || header->max_slots == 0 ||
            !session_history_record_type_valid((uint8_t)record_type)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (header->total_records == UINT32_MAX) {
        ESP_LOGE(TAG, "session history total_records overflow");
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "seek session history EOF failed");
        return ESP_FAIL;
    }

    err = claw_memory_write_session_json_record(file, role, text, &offset, &length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write session history %s record failed: %s", role, esp_err_to_name(err));
        return err;
    }

    slot = header->total_records % header->max_slots;
    header->entries[slot].offset = offset;
    header->entries[slot].length = length;
    header->record_types[slot] = (uint8_t)record_type;
    header->backend_formats[slot] = (uint8_t)s_memory.backend_format;
    header->total_records++;

    return ESP_OK;
}

esp_err_t claw_memory_session_append(const char *session_id,
                                     const char *user_text,
                                     const char *assistant_text)
{
    char *path = NULL;
    FILE *file = NULL;
    claw_memory_session_header_t header;
    esp_err_t err = ESP_OK;

    if (!session_id || !user_text || !assistant_text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    path = claw_memory_session_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        return ESP_ERR_NO_MEM;
    }
    if (ensure_parent_dir(path) != ESP_OK) {
        free(path);
        return ESP_FAIL;
    }

    err = session_history_open_for_append(path, &file, &header);
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    err = session_history_append_indexed_record(file,
                                                &header,
                                                CLAW_MEMORY_RECORD_TYPE_USER,
                                                "user",
                                                user_text);
    if (err == ESP_OK) {
        err = session_history_append_indexed_record(file,
                                                    &header,
                                                    CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL,
                                                    "assistant",
                                                    assistant_text);
    }
    if (err == ESP_OK) {
        err = session_history_write_header(file, &header);
    }
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(path);
    return err;
}

esp_err_t claw_memory_note_session_summary(const char *session_id,
                                           const char *summary_list)
{
    return claw_memory_pending_summary_append(session_id, summary_list);
}

esp_err_t claw_memory_append_session_turn_callback(const char *session_id,
                                                   const char *user_text,
                                                   const char *assistant_text,
                                                   void *user_ctx)
{
    (void)user_ctx;
    return claw_memory_session_append(session_id, user_text, assistant_text);
}

esp_err_t claw_memory_request_start_callback(const claw_core_request_t *request,
                                             void *user_ctx)
{
    (void)user_ctx;
    return claw_memory_async_extract_ensure_started(request);
}

esp_err_t claw_memory_stage_note_callback(const claw_core_request_t *request,
                                          char **out_note,
                                          void *user_ctx)
{
    char *summary_list = NULL;
    char *async_summary = NULL;
    bool manual_write = false;

    (void)user_ctx;

    if (!out_note) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_note = NULL;
    if (!request || !request->session_id || !request->session_id[0]) {
        return ESP_OK;
    }

    manual_write = claw_memory_request_take_manual_write(request->request_id);
    summary_list = claw_memory_pending_summary_take_summary_list(request->session_id);
    async_summary = claw_memory_async_extract_take_summary_list(request, !manual_write);
    if (line_list_merge_unique(&summary_list, async_summary) != ESP_OK) {
        ESP_LOGW(TAG, "merge async extract summary failed for request=%" PRIu32, request->request_id);
    }
    free(async_summary);
    *out_note = claw_memory_format_update_stage_note(summary_list);
    free(summary_list);
    return ESP_OK;
}

static esp_err_t claw_memory_session_history_collect(const claw_core_request_t *request,
                                                     claw_core_context_t *out_context,
                                                     void *user_ctx)
{
    char *content = NULL;
    esp_err_t err;

    (void)user_ctx;

    if (!request || !out_context || !request->session_id || !request->session_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_context, 0, sizeof(*out_context));

    err = claw_memory_session_load_json_alloc(request->session_id, &content);
    if (err != ESP_OK) {
        return err;
    }
    if (!content || !content[0] || strcmp(content, "[]") == 0) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_memory_session_history_provider = {
    .name = "Session History",
    .collect = claw_memory_session_history_collect,
    .user_ctx = NULL,
};
