/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"
#include "claw_task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_core";

static claw_core_state_t *s_core = NULL;

static void free_context_provider_storage(claw_core_state_t *core)
{
    size_t i;

    if (!core) {
        return;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        free((char *)core->context_providers[i].name);
        core->context_providers[i].name = NULL;
    }
    free(core->context_providers);
    core->context_providers = NULL;
    core->context_provider_count = 0;
    core->context_provider_capacity = 0;
}

static void claw_core_free_state_storage(void)
{
    free(s_core);
    s_core = NULL;
}

static void claw_core_reset_runtime(void)
{
    if (!s_core) {
        return;
    }

    free_context_provider_storage(s_core);
    free(s_core->system_prompt);
    if (s_core->request_queue) {
        vQueueDelete(s_core->request_queue);
    }
    if (s_core->response_queue) {
        vQueueDelete(s_core->response_queue);
    }
    if (s_core->response_lock) {
        vSemaphoreDelete(s_core->response_lock);
    }
    if (s_core->inflight_lock) {
        if (xSemaphoreTake(s_core->inflight_lock, portMAX_DELAY) == pdTRUE) {
            claw_core_ingress_clear_insert_queue_locked(s_core);
            xSemaphoreGive(s_core->inflight_lock);
        }
        vSemaphoreDelete(s_core->inflight_lock);
    }
    memset(s_core, 0, sizeof(*s_core));
    claw_core_free_state_storage();
}

esp_err_t claw_core_init(const claw_core_config_t *config)
{
    claw_core_llm_config_t llm_config = {0};
    char *llm_error = NULL;
    esp_err_t err;
    uint32_t request_queue_len;
    uint32_t response_queue_len;

    if (!config || !config->system_prompt || !config->api_key || !config->model ||
            !(config->backend_type && config->backend_type[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core && s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_core = calloc(1, sizeof(*s_core));
    if (!s_core) {
        return ESP_ERR_NO_MEM;
    }
    claw_core_check_timezone();

    s_core->system_prompt = claw_core_dup_string(config->system_prompt);
    if (!s_core->system_prompt) {
        claw_core_free_state_storage();
        return ESP_ERR_NO_MEM;
    }
    s_core->persist_context = config->persist_context;
    s_core->persist_context_user_ctx = config->persist_context_user_ctx;
    s_core->request_gate = config->request_gate;
    s_core->request_gate_user_ctx = config->request_gate_user_ctx;
    s_core->on_request_start = config->on_request_start;
    s_core->on_request_start_user_ctx = config->on_request_start_user_ctx;
    s_core->collect_stage_note = config->collect_stage_note;
    s_core->collect_stage_note_user_ctx = config->collect_stage_note_user_ctx;
    s_core->call_cap = config->call_cap;
    s_core->cap_user_ctx = config->cap_user_ctx;

    request_queue_len = config->request_queue_len ? config->request_queue_len : CLAW_CORE_DEFAULT_REQUEST_Q;
    response_queue_len = config->response_queue_len ? config->response_queue_len : CLAW_CORE_DEFAULT_RESPONSE_Q;
    s_core->task_stack_size = config->task_stack_size ? config->task_stack_size : CLAW_CORE_DEFAULT_STACK_SIZE;
    s_core->task_priority = config->task_priority ? config->task_priority : CLAW_CORE_DEFAULT_PRIORITY;
    s_core->task_core = config->task_core;
    s_core->max_tool_iterations = config->max_tool_iterations ?
                                  config->max_tool_iterations : CLAW_CORE_DEFAULT_TOOL_ITERATIONS;
    s_core->context_provider_capacity = config->max_context_providers;

    if (s_core->context_provider_capacity > 0) {
        s_core->context_providers = calloc(s_core->context_provider_capacity,
                                           sizeof(claw_core_context_provider_t));
        if (!s_core->context_providers) {
            claw_core_reset_runtime();
            return ESP_ERR_NO_MEM;
        }
    }

    s_core->request_queue = xQueueCreate(request_queue_len, sizeof(claw_core_request_item_t));
    s_core->response_queue = xQueueCreate(response_queue_len, sizeof(claw_core_response_item_t));
    s_core->response_lock = xSemaphoreCreateMutex();
    s_core->inflight_lock = xSemaphoreCreateMutex();
    if (!s_core->request_queue || !s_core->response_queue ||
            !s_core->response_lock || !s_core->inflight_lock) {
        free_context_provider_storage(s_core);
        free(s_core->system_prompt);
        if (s_core->request_queue) {
            vQueueDelete(s_core->request_queue);
        }
        if (s_core->response_queue) {
            vQueueDelete(s_core->response_queue);
        }
        if (s_core->response_lock) {
            vSemaphoreDelete(s_core->response_lock);
        }
        if (s_core->inflight_lock) {
            vSemaphoreDelete(s_core->inflight_lock);
        }
        claw_core_free_state_storage();
        return ESP_ERR_NO_MEM;
    }

    llm_config.api_key = config->api_key;
    llm_config.backend_type = config->backend_type;
    llm_config.model = config->model;
    llm_config.base_url = config->base_url;
    llm_config.auth_type = config->auth_type;
    llm_config.max_tokens_field = config->max_tokens_field;
    llm_config.timeout_ms = config->timeout_ms;
    llm_config.max_tokens = config->max_tokens;
    llm_config.image_max_bytes = config->image_max_bytes;
    llm_config.supports_tools = config->supports_tools;
    llm_config.supports_vision = config->supports_vision;
    llm_config.image_remote_url_only = config->image_remote_url_only;
    err = claw_core_llm_init(&llm_config, &llm_error);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM init failed: %s", llm_error ? llm_error : esp_err_to_name(err));
        free(llm_error);
        claw_core_reset_runtime();
        return err;
    }

    s_core->initialized = true;
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t claw_core_start(void)
{
    BaseType_t task_result;

    if (!s_core || !s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_core->started) {
        return ESP_OK;
    }

    task_result = claw_task_create(&(claw_task_config_t){
                                        .name = "claw_core",
                                        .stack_size = s_core->task_stack_size,
                                        .priority = s_core->task_priority,
                                        .core_id = s_core->task_core,
                                        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                                    },
                                    claw_core_agent_loop_task,
                                    s_core,
                                    &s_core->task_handle);

    if (task_result != pdPASS) {
        return ESP_FAIL;
    }

    s_core->started = true;
    ESP_LOGI(TAG, "Started worker task");
    return ESP_OK;
}

esp_err_t claw_core_add_context_provider(const claw_core_context_provider_t *provider)
{
    claw_core_context_provider_t *slot = NULL;

    if (!s_core || !s_core->initialized || s_core->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!provider || !provider->name || !provider->collect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core->context_provider_count >= s_core->context_provider_capacity) {
        return ESP_ERR_NO_MEM;
    }

    slot = &s_core->context_providers[s_core->context_provider_count];
    slot->name = claw_core_dup_string(provider->name);
    if (!slot->name) {
        return ESP_ERR_NO_MEM;
    }
    slot->collect = provider->collect;
    slot->user_ctx = provider->user_ctx;
    slot->flags = provider->flags;
    s_core->context_provider_count++;
    return ESP_OK;
}

esp_err_t claw_core_add_completion_observer(claw_core_completion_observer_fn observer,
                                            void *user_ctx)
{
    if (!s_core || !s_core->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!observer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_core->completion_observer_count >= CLAW_CORE_MAX_COMPLETION_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_core->completion_observers[s_core->completion_observer_count].fn = observer;
    s_core->completion_observers[s_core->completion_observer_count].user_ctx = user_ctx;
    s_core->completion_observer_count++;
    return ESP_OK;
}

esp_err_t claw_core_call_cap(const char *cap_name,
                             const char *input_json,
                             const claw_core_request_t *request,
                             char **out_output)
{
    if (!s_core || !s_core->initialized || !s_core->call_cap) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_core->call_cap(cap_name,
                            input_json,
                            request,
                            out_output,
                            s_core->cap_user_ctx);
}

esp_err_t claw_core_cancel_request(uint32_t request_id)
{
    return claw_core_control_cancel_request(s_core, request_id);
}

claw_core_agent_loop_phase_t claw_core_get_agent_loop_phase(void)
{
    return claw_core_control_get_phase(s_core);
}

esp_err_t claw_core_submit(const claw_core_request_t *request, uint32_t timeout_ms)
{
    return claw_core_ingress_submit(s_core, request, timeout_ms);
}

esp_err_t claw_core_receive(claw_core_response_t *response, uint32_t timeout_ms)
{
    return claw_core_receive_for(0, response, timeout_ms);
}

esp_err_t claw_core_receive_for(uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms)
{
    return claw_core_response_receive_for(s_core, request_id, response, timeout_ms);
}
