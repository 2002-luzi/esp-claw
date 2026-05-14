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

typedef struct {
    size_t turn_starts[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
    uint8_t turn_completed[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
    uint8_t turn_load_full[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
} claw_memory_session_select_scratch_t;

typedef struct {
    claw_memory_session_header_t header;
    uint8_t selected[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS];
} claw_memory_session_load_scratch_t;

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

static esp_err_t session_history_read_record_text(FILE *file,
                                                  const claw_memory_session_index_entry_t *entry,
                                                  char **out_text)
{
    char *text = NULL;
    size_t object_len;

    if (!file || !entry || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    object_len = session_history_record_object_len(entry);
    if (entry->offset < CLAW_MEMORY_SESSION_HEADER_SIZE || object_len == 0) {
        ESP_LOGW(TAG,
                 "Invalid session history entry offset=%" PRIu32 " length=%" PRIu32,
                 entry->offset,
                 entry->length);
        return ESP_ERR_INVALID_STATE;
    }

    text = calloc(1, object_len + 1);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    if (fseek(file, (long)entry->offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "seek session history record failed");
        free(text);
        return ESP_FAIL;
    }
    if (fread(text, 1, object_len, file) != object_len) {
        ESP_LOGE(TAG, "read session history record failed");
        free(text);
        return ESP_FAIL;
    }

    *out_text = text;
    return ESP_OK;
}

static bool session_history_turn_has_final(const claw_memory_session_header_t *header,
                                           const size_t *turn_starts,
                                           size_t turn_index,
                                           size_t turn_count,
                                           size_t retained_count)
{
    size_t start = turn_starts[turn_index];
    size_t end = (turn_index + 1 < turn_count) ?
        turn_starts[turn_index + 1] : retained_count;
    size_t i;

    for (i = start; i < end; i++) {
        size_t slot = session_history_retained_slot(header, i);

        if (header->record_types[slot] == CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL) {
            return true;
        }
    }
    return false;
}

static esp_err_t session_history_select_records(const claw_memory_session_header_t *header,
                                                uint8_t selected[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS],
                                                size_t *out_retained_count,
                                                size_t *out_selected_count)
{
    claw_memory_session_select_scratch_t *scratch = NULL;
    size_t retained_count;
    size_t selected_count = 0;
    size_t turn_count = 0;
    size_t recent_completed = 0;
    size_t i;
    size_t t;
    esp_err_t err = ESP_OK;

    if (!header || !selected || !out_retained_count || !out_selected_count) {
        return ESP_ERR_INVALID_ARG;
    }

    scratch = calloc(1, sizeof(*scratch));
    if (!scratch) {
        return ESP_ERR_NO_MEM;
    }

    retained_count = session_history_retained_count(header);
    *out_retained_count = retained_count;
    *out_selected_count = 0;
    memset(selected, 0, CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS);

    if (retained_count == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    for (i = 0; i < retained_count; i++) {
        size_t slot = session_history_retained_slot(header, i);

        if (header->record_types[slot] == CLAW_MEMORY_RECORD_TYPE_USER) {
            scratch->turn_starts[turn_count] = i;
            turn_count++;
        }
    }
    if (turn_count == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    for (t = 0; t < turn_count; t++) {
        scratch->turn_completed[t] = session_history_turn_has_final(header,
                                                                    scratch->turn_starts,
                                                                    t,
                                                                    turn_count,
                                                                    retained_count);
    }

    for (t = turn_count; t > 0; t--) {
        size_t turn_index = t - 1;

        if (scratch->turn_completed[turn_index] &&
                recent_completed < CLAW_MEMORY_SESSION_RECENT_TOOL_TURNS) {
            scratch->turn_load_full[turn_index] = 1;
            recent_completed++;
        }
    }
    if (!scratch->turn_completed[turn_count - 1]) {
        scratch->turn_load_full[turn_count - 1] = 1;
    }

    for (t = 0; t < turn_count; t++) {
        size_t start = scratch->turn_starts[t];
        size_t end = (t + 1 < turn_count) ? scratch->turn_starts[t + 1] : retained_count;

        for (i = start; i < end; i++) {
            size_t slot = session_history_retained_slot(header, i);
            claw_memory_record_type_t type = (claw_memory_record_type_t)header->record_types[slot];
            bool keep = false;

            if (scratch->turn_load_full[t]) {
                keep = true;
            } else if (scratch->turn_completed[t] &&
                       (type == CLAW_MEMORY_RECORD_TYPE_USER ||
                        type == CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL)) {
                keep = true;
            }

            if (keep) {
                selected[i] = 1;
                selected_count++;
            }
        }
    }

    if (selected_count == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    *out_selected_count = selected_count;

cleanup:
    free(scratch);
    return err;
}

static esp_err_t session_history_append_loaded_record(cJSON *records,
                                                      cJSON *record,
                                                      bool expand_array)
{
    cJSON *item;

    if (!records || !record) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!expand_array || !cJSON_IsArray(record)) {
        if (!cJSON_AddItemToArray(records, record)) {
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    cJSON_ArrayForEach(item, record) {
        cJSON *duplicate = cJSON_Duplicate(item, true);

        if (!duplicate) {
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
        if (!cJSON_AddItemToArray(records, duplicate)) {
            cJSON_Delete(duplicate);
            cJSON_Delete(record);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(record);
    return ESP_OK;
}

static bool session_history_backend_mismatch(uint8_t record_format);
static esp_err_t session_history_degrade_assistant_final(cJSON *record,
                                                         cJSON **out_record);

static esp_err_t session_history_load_selected_json(FILE *file,
                                                    const claw_memory_session_header_t *header,
                                                    const uint8_t selected[CLAW_MEMORY_SESSION_MAX_INDEX_SLOTS],
                                                    size_t retained_count,
                                                    char **out_json)
{
    cJSON *records = NULL;
    char *json = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!file || !header || !selected || !out_json || retained_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    records = cJSON_CreateArray();
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < retained_count; i++) {
        size_t slot;
        const claw_memory_session_index_entry_t *entry;
        claw_memory_record_type_t type;
        char *record_text = NULL;
        cJSON *record = NULL;

        if (!selected[i]) {
            continue;
        }

        slot = session_history_retained_slot(header, i);
        entry = &header->entries[slot];
        type = (claw_memory_record_type_t)header->record_types[slot];

        err = session_history_read_record_text(file, entry, &record_text);
        if (err != ESP_OK) {
            goto cleanup;
        }

        record = cJSON_ParseWithOpts(record_text, NULL, 1);
        free(record_text);
        if (!record) {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }

        if (session_history_backend_mismatch(header->backend_formats[slot])) {
            if (type == CLAW_MEMORY_RECORD_TYPE_ASSISTANT_TOOL ||
                    type == CLAW_MEMORY_RECORD_TYPE_TOOL_RESULT) {
                cJSON_Delete(record);
                continue;
            }
            if (type == CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL) {
                cJSON *degraded = NULL;

                err = session_history_degrade_assistant_final(record, &degraded);
                if (err == ESP_ERR_NOT_FOUND) {
                    err = ESP_OK;
                    continue;
                }
                if (err != ESP_OK) {
                    goto cleanup;
                }
                record = degraded;
            }
        }

        err = session_history_append_loaded_record(records,
                                                   record,
                                                   type == CLAW_MEMORY_RECORD_TYPE_TOOL_RESULT);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    json = cJSON_PrintUnformatted(records);
    if (!json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_json = json;
    json = NULL;

cleanup:
    if (json) {
        cJSON_free(json);
    }
    if (records) {
        cJSON_Delete(records);
    }
    return err;
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

static bool session_history_backend_mismatch(uint8_t record_format)
{
    if (s_memory.backend_format == CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN ||
            record_format == CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN) {
        return false;
    }
    return record_format != (uint8_t)s_memory.backend_format;
}

static esp_err_t session_history_degrade_assistant_final(cJSON *record,
                                                        cJSON **out_record)
{
    cJSON *content = NULL;
    cJSON *block = NULL;
    cJSON *fallback = NULL;
    char *text = NULL;
    size_t total_len = 0;
    size_t offset = 0;

    if (!record || !out_record) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_record = NULL;

    content = cJSON_GetObjectItem(record, "content");
    if (cJSON_IsString(content)) {
        *out_record = record;
        return ESP_OK;
    }
    if (!cJSON_IsArray(content)) {
        cJSON_Delete(record);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        cJSON *block_text = cJSON_GetObjectItem(block, "text");

        if (cJSON_IsString(type) && type->valuestring &&
                strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(block_text) && block_text->valuestring) {
            total_len += strlen(block_text->valuestring);
        }
    }
    if (total_len == 0) {
        cJSON_Delete(record);
        return ESP_ERR_NOT_FOUND;
    }

    text = calloc(1, total_len + 1);
    if (!text) {
        cJSON_Delete(record);
        return ESP_ERR_NO_MEM;
    }
    cJSON_ArrayForEach(block, content) {
        cJSON *type = cJSON_GetObjectItem(block, "type");
        cJSON *block_text = cJSON_GetObjectItem(block, "text");

        if (cJSON_IsString(type) && type->valuestring &&
                strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(block_text) && block_text->valuestring) {
            size_t len = strlen(block_text->valuestring);

            memcpy(text + offset, block_text->valuestring, len);
            offset += len;
        }
    }

    fallback = cJSON_CreateObject();
    if (!fallback ||
            !cJSON_AddStringToObject(fallback, "role", "assistant") ||
            !cJSON_AddStringToObject(fallback, "content", text)) {
        cJSON_Delete(fallback);
        free(text);
        cJSON_Delete(record);
        return ESP_ERR_NO_MEM;
    }

    free(text);
    cJSON_Delete(record);
    *out_record = fallback;
    return ESP_OK;
}

static esp_err_t claw_memory_session_load_json_alloc(const char *session_id, char **out_json)
{
    char *path = NULL;
    FILE *file = NULL;
    claw_memory_session_load_scratch_t *scratch = NULL;
    char *json = NULL;
    size_t retained_count = 0;
    size_t selected_count = 0;
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
    scratch = calloc(1, sizeof(*scratch));
    if (!scratch) {
        return ESP_ERR_NO_MEM;
    }

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

    err = session_history_read_header(file, &scratch->header);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "invalid header";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = session_history_select_records(&scratch->header,
                                         scratch->selected,
                                         &retained_count,
                                         &selected_count);
    if (err == ESP_ERR_INVALID_STATE) {
        reset_file = true;
        reset_reason = "invalid index";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (selected_count == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    err = session_history_load_selected_json(file,
                                             &scratch->header,
                                             scratch->selected,
                                             retained_count,
                                             &json);
    if (err != ESP_OK) {
        reset_file = true;
        reset_reason = "read indexed records failed";
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (reset_file && path) {
        FILE *reset_file_handle = NULL;
        esp_err_t reset_err;

        ESP_LOGW(TAG, "Resetting session history %s: %s", path, reset_reason);
        reset_err = session_history_recreate_file(path, &reset_file_handle, &scratch->header);
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
    free(scratch);
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

static esp_err_t session_history_append_raw_indexed_record(FILE *file,
                                                           claw_memory_session_header_t *header,
                                                           claw_memory_record_type_t record_type,
                                                           const char *json_text)
{
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t slot;
    esp_err_t err;

    if (!file || !header || !json_text || header->max_slots == 0 ||
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

    err = claw_memory_write_session_raw_record(file, json_text, &offset, &length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write raw session history record failed: %s", esp_err_to_name(err));
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

static esp_err_t claw_memory_session_append_records(const char *session_id,
                                                    const char *user_message_json,
                                                    const char *assistant_final_json,
                                                    const char *assistant_tool_json,
                                                    const char *tool_results_json)
{
    char *path = NULL;
    FILE *file = NULL;
    claw_memory_session_header_t *header = NULL;
    esp_err_t err = ESP_OK;

    if (!session_id || (!user_message_json && !assistant_final_json &&
                        !assistant_tool_json && !tool_results_json)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_memory.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((assistant_tool_json && !tool_results_json) ||
            (!assistant_tool_json && tool_results_json)) {
        return ESP_ERR_INVALID_ARG;
    }
    header = calloc(1, sizeof(*header));
    if (!header) {
        return ESP_ERR_NO_MEM;
    }

    path = claw_memory_session_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "allocate session history path failed");
        free(header);
        return ESP_ERR_NO_MEM;
    }
    if (ensure_parent_dir(path) != ESP_OK) {
        free(path);
        free(header);
        return ESP_FAIL;
    }

    err = session_history_open_for_append(path, &file, header);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (user_message_json) {
        err = session_history_append_raw_indexed_record(file,
                                                        header,
                                                        CLAW_MEMORY_RECORD_TYPE_USER,
                                                        user_message_json);
    }
    if (err == ESP_OK && assistant_tool_json) {
        err = session_history_append_raw_indexed_record(file,
                                                        header,
                                                        CLAW_MEMORY_RECORD_TYPE_ASSISTANT_TOOL,
                                                        assistant_tool_json);
    }
    if (err == ESP_OK && tool_results_json) {
        err = session_history_append_raw_indexed_record(file,
                                                        header,
                                                        CLAW_MEMORY_RECORD_TYPE_TOOL_RESULT,
                                                        tool_results_json);
    }
    if (err == ESP_OK && assistant_final_json) {
        err = session_history_append_raw_indexed_record(file,
                                                        header,
                                                        CLAW_MEMORY_RECORD_TYPE_ASSISTANT_FINAL,
                                                        assistant_final_json);
    }
    if (err == ESP_OK) {
        err = session_history_write_header(file, header);
    }

cleanup:
    if (file && session_history_close_file(file) != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
    }
    free(path);
    free(header);
    return err;
}

esp_err_t claw_memory_note_session_summary(const char *session_id,
                                           const char *summary_list)
{
    return claw_memory_pending_summary_append(session_id, summary_list);
}

esp_err_t claw_memory_append_session_turn_callback(const char *session_id,
                                                   const char *user_message_json,
                                                   const char *assistant_message_json,
                                                   void *user_ctx)
{
    (void)user_ctx;

    if (!session_id || !assistant_message_json) {
        return ESP_ERR_INVALID_ARG;
    }
    return claw_memory_session_append_records(session_id,
                                              user_message_json,
                                              assistant_message_json,
                                              NULL,
                                              NULL);
}

esp_err_t claw_memory_flush_tool_round_callback(const char *session_id,
                                                const char *user_message_json,
                                                const char *assistant_tool_json,
                                                const char *tool_results_json,
                                                void *user_ctx)
{
    (void)user_ctx;

    if (!session_id || !assistant_tool_json || !tool_results_json) {
        return ESP_ERR_INVALID_ARG;
    }
    return claw_memory_session_append_records(session_id,
                                              user_message_json,
                                              NULL,
                                              assistant_tool_json,
                                              tool_results_json);
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
    .flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY,
};
