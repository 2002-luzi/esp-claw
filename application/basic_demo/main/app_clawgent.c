/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_clawgent.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "basic_demo_board_audio.h"
#include "basic_demo_lua_modules.h"
#include "cap_cli.h"
#include "cap_files.h"
#include "cap_im_qq.h"
#include "cap_im_tg.h"
#include "cap_im_wechat.h"
#include "cap_llm_inspect.h"
#include "cap_lua.h"
#include "cap_mcp_client.h"
#include "cap_mcp_server.h"
#include "cap_skill.h"
#include "cap_time.h"
#include "cap_tts.h"
#include "cap_web_search.h"
#include "claw_event_router.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "app_clawgent";
static const char *const BASIC_DEMO_LLM_VISIBLE_GROUPS[] = {
    "cap_cli",
    "cap_files",
    "cap_tts",
    "cap_lua",
};

#define BASIC_DEMO_MEMORY_SESSION_ROOT   "/fatfs/data/sessions"
#define BASIC_DEMO_MEMORY_LONG_TERM_PATH "/fatfs/data/memory/MEMORY.md"
#define BASIC_DEMO_SKILLS_ROOT_DIR       "/fatfs/data/skills"
#define BASIC_DEMO_LUA_ROOT_DIR       "/fatfs/data/lua"
#define BASIC_DEMO_FATFS_BASE_PATH       "/fatfs/data"
#define BASIC_DEMO_AUTOMATION_RULES_PATH "/fatfs/data/automation/automations.json"
#define BASIC_DEMO_IM_ATTACHMENT_ROOT    "/fatfs/data/inbox"
#define BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES (2 * 1024 * 1024)

#define BASIC_DEMO_SYSTEM_PROMPT \
    "You are the clawgent running on ESP32. " \
    "Answer briefly and plainly. " \
    "Treat Skills List as a catalog of optional skills, not as callable cap. " \
    "Use 'activate_skill' to load a skill's documentation into the current session when needed.\n" \
    "Skills are user-facing functions, while Capabilities are internal functions used by the model. " \
    "When communicating with the user, refer to Skills instead of Capabilities." \
    "\n" \
    "/fatfs/data file tree:\n" \
    "/fatfs/data/\n" \
    "|-- automation/\n" \
    "|   `-- automations.json\n" \
    "|-- lua/\n" \
    "|   `-- xxx.lua\n" \
    "|-- memory/\n" \
    "|   `-- MEMORY.md\n" \
    "`-- skills/\n" \
    "    |-- xxx.md\n" \
    "    |-- skills_list.json\n" \
    "    `-- weather.md\n"

esp_err_t basic_demo_cli_start(void);

static uint32_t parse_timeout_ms(const char *value, uint32_t fallback)
{
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }

    parsed = strtoul(value, NULL, 10);
    if (parsed == 0 || parsed > UINT32_MAX) {
        return fallback;
    }

    return (uint32_t)parsed;
}

static esp_err_t init_tts_runtime(const basic_demo_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    return cap_tts_init_runtime(&(cap_tts_runtime_config_t) {
        .xfyun_app_id = settings->tts_xfyun_app_id,
        .xfyun_api_key = settings->tts_xfyun_api_key,
        .xfyun_api_secret = settings->tts_xfyun_api_secret,
        .xfyun_voice_name = settings->tts_xfyun_voice_name,
        .xfyun_audio_encoding = settings->tts_xfyun_audio_encoding,
        .xfyun_audio_format = settings->tts_xfyun_audio_format,
        .xfyun_text_encoding = settings->tts_xfyun_text_encoding,
        .xfyun_websocket_uri = settings->tts_xfyun_websocket_uri,
        .xfyun_auth_host = settings->tts_xfyun_auth_host,
        .timeout_ms = parse_timeout_ms(settings->tts_timeout_ms, 30000),
        .enable_speaker_output = BASIC_DEMO_TTS_ENABLE_SPEAKER_OUTPUT,
        .speaker_init = basic_demo_board_audio_init,
    });
}

static esp_err_t init_memory(void)
{
    claw_memory_config_t memory_config = {
        .session_root_dir = BASIC_DEMO_MEMORY_SESSION_ROOT,
        .long_term_memory_path = BASIC_DEMO_MEMORY_LONG_TERM_PATH,
        .max_session_messages = 20,
        .max_message_chars = 256,
    };
    esp_err_t err;

    err = claw_memory_init(&memory_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init claw_memory: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t init_skills(void)
{
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t) {
        .skills_root_dir = BASIC_DEMO_SKILLS_ROOT_DIR,
        .session_state_root_dir = BASIC_DEMO_MEMORY_SESSION_ROOT,
    }),
    TAG,
    "Failed to init claw_skill");
    return ESP_OK;
}

static esp_err_t init_capabilities(const basic_demo_settings_t *settings)
{
    claw_cap_config_t cap_config = {
        .max_capabilities = 37,
        .max_groups = 16,
    };

    ESP_RETURN_ON_ERROR(claw_cap_init(&cap_config), TAG, "Failed to init claw_cap");

    ESP_RETURN_ON_ERROR(cap_cli_init(&(cap_cli_config_t) {
        .max_commands = 16,
        .max_output_bytes = 2048,
    }),
    TAG,
    "Failed to init CLI cap");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "help",
        .description = "List available console commands",
        .usage_hint = "help [command]",
    }),
    TAG,
    "Failed to whitelist help");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "cap",
        .description = "Manage console cap commands",
        .usage_hint = "cap list",
    }),
    TAG,
    "Failed to whitelist cap");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "auto",
        .description = "Manage automation console commands",
        .usage_hint = "auto rules",
    }),
    TAG,
    "Failed to whitelist auto");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "llm_inspect",
        .description = "Inspect one local image with the LLM",
        .usage_hint = "llm_inspect --path /fatfs/data/inbox/pic.jpg --prompt \"Describe this image\"",
    }),
    TAG,
    "Failed to whitelist llm_inspect");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "mcp_client",
        .description = "Run MCP discovery and remote tool calls",
        .usage_hint = "mcp_client --discover",
    }),
    TAG,
    "Failed to whitelist mcp_client");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "mcp_server",
        .description = "Manage local MCP server lifecycle and config",
        .usage_hint = "mcp_server --status",
    }),
    TAG,
    "Failed to whitelist mcp_server");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "skill",
        .description = "Manage active skills for one session",
        .usage_hint = "skill --list --session default",
    }),
    TAG,
    "Failed to whitelist skill");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "time",
        .description = "Sync time or update timezone",
        .usage_hint = "time --now",
    }),
    TAG,
    "Failed to whitelist time");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "web_search",
        .description = "Run web search with the configured provider",
        .usage_hint = "web_search --query \"ESP-IDF mDNS example\"",
    }),
    TAG,
    "Failed to whitelist web_search");
    ESP_RETURN_ON_ERROR(cap_cli_register_command(&(cap_cli_command_t) {
        .command_name = "event_router",
        .description = "Manage and inspect event router operations",
        .usage_hint = "event_router --rules",
    }),
    TAG,
    "Failed to whitelist event_router");
    ESP_RETURN_ON_ERROR(cap_time_set_timezone(settings->time_timezone),
                        TAG,
                        "Failed to set time cap timezone");
    ESP_RETURN_ON_ERROR(cap_files_set_base_dir(BASIC_DEMO_FATFS_BASE_PATH),
                        TAG,
                        "Failed to set files cap base dir");
    ESP_RETURN_ON_ERROR(cap_lua_set_base_dir(BASIC_DEMO_LUA_ROOT_DIR),
                        TAG,
                        "Failed to set Lua base dir");
    ESP_RETURN_ON_ERROR(cap_im_qq_set_attachment_config(
    &(cap_im_qq_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set QQ attachment config");
    ESP_RETURN_ON_ERROR(cap_im_tg_set_attachment_config(
    &(cap_im_tg_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set Telegram attachment config");
    ESP_RETURN_ON_ERROR(cap_im_wechat_set_attachment_config(
    &(cap_im_wechat_attachment_config_t) {
        .storage_root_dir = BASIC_DEMO_IM_ATTACHMENT_ROOT,
        .max_inbound_file_bytes = BASIC_DEMO_IM_ATTACHMENT_MAX_BYTES,
        .enable_inbound_attachments = true,
    }),
    TAG,
    "Failed to set WeChat attachment config");

    if (settings->qq_app_id[0] && settings->qq_app_secret[0]) {
        ESP_RETURN_ON_ERROR(cap_im_qq_set_credentials(settings->qq_app_id,
                                                      settings->qq_app_secret),
                            TAG,
                            "Failed to set QQ credentials");
    }

    if (settings->tg_bot_token[0]) {
        ESP_RETURN_ON_ERROR(cap_im_tg_set_token(settings->tg_bot_token),
                            TAG,
                            "Failed to set Telegram bot token");
    }

    if (settings->wechat_token[0] && settings->wechat_base_url[0]) {
        ESP_RETURN_ON_ERROR(cap_im_wechat_set_client_config(
        &(cap_im_wechat_client_config_t) {
            .token = settings->wechat_token,
            .base_url = settings->wechat_base_url,
            .cdn_base_url = settings->wechat_cdn_base_url,
            .account_id = settings->wechat_account_id,
        }),
        TAG,
        "Failed to set WeChat client config");
    }

    if (settings->search_brave_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_brave_key(settings->search_brave_key),
                            TAG,
                            "Failed to set Brave search key");
    }

    if (settings->search_tavily_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_tavily_key(settings->search_tavily_key),
                            TAG,
                            "Failed to set Tavily search key");
    }

    ESP_RETURN_ON_ERROR(cap_im_qq_register_group(), TAG, "Failed to register QQ cap");
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(),
                        TAG,
                        "Failed to register Telegram cap");
    ESP_RETURN_ON_ERROR(cap_im_wechat_register_group(),
                        TAG,
                        "Failed to register WeChat cap");
    ESP_RETURN_ON_ERROR(cap_files_register_group(), TAG, "Failed to register files cap");
    ESP_RETURN_ON_ERROR(basic_demo_lua_modules_register(),
                        TAG,
                        "Failed to register app Lua modules");
    ESP_RETURN_ON_ERROR(cap_lua_register_group(), TAG, "Failed to register Lua cap");
    ESP_RETURN_ON_ERROR(cap_mcp_client_register_group(),
                        TAG,
                        "Failed to register MCP client cap");
    ESP_RETURN_ON_ERROR(cap_mcp_server_register_group(),
                        TAG,
                        "Failed to register MCP server cap");
    ESP_RETURN_ON_ERROR(cap_cli_register_group(), TAG, "Failed to register CLI cap");
    ESP_RETURN_ON_ERROR(cap_skill_register_group(),
                        TAG,
                        "Failed to register skill cap");
    ESP_RETURN_ON_ERROR(cap_time_register_group(), TAG, "Failed to register time cap");
    ESP_RETURN_ON_ERROR(cap_tts_register_group(), TAG, "Failed to register TTS cap");
    ESP_RETURN_ON_ERROR(cap_llm_inspect_register_group(),
                        TAG,
                        "Failed to register LLM inspect cap");
    ESP_RETURN_ON_ERROR(cap_web_search_register_group(),
                        TAG,
                        "Failed to register web search cap");
    ESP_RETURN_ON_ERROR(claw_cap_set_llm_visible_groups(
                            BASIC_DEMO_LLM_VISIBLE_GROUPS,
                            sizeof(BASIC_DEMO_LLM_VISIBLE_GROUPS) /
                            sizeof(BASIC_DEMO_LLM_VISIBLE_GROUPS[0])),
                        TAG,
                        "Failed to set LLM-visible capability groups");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "Failed to start capabilities");

    return ESP_OK;
}

esp_err_t app_clawgent_start(const basic_demo_settings_t *settings)
{
    claw_core_config_t core_config = {0};
    claw_event_router_config_t router_config = {
        .rules_path = BASIC_DEMO_AUTOMATION_RULES_PATH,
        .task_stack_size = 6144,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 130000,
        .default_route_messages_to_agent = true,
    };

    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config),
                        TAG,
                        "Failed to init event router");
    ESP_RETURN_ON_ERROR(init_memory(), TAG, "Failed to init memory");
    ESP_RETURN_ON_ERROR(init_skills(), TAG, "Failed to init skills");
    ESP_RETURN_ON_ERROR(init_tts_runtime(settings), TAG, "Failed to init TTS runtime");
    ESP_RETURN_ON_ERROR(init_capabilities(settings), TAG, "Failed to init capabilities");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("qq", "qq_send_message"),
                        TAG,
                        "Failed to bind QQ outbound");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"),
                        TAG,
                        "Failed to bind Telegram outbound");
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("wechat", "wechat_send_message"),
                        TAG,
                        "Failed to bind WeChat outbound");

    core_config.api_key = settings->llm_api_key;
    core_config.backend_type = settings->llm_backend_type;
    core_config.profile = settings->llm_profile;
    core_config.model = settings->llm_model;
    core_config.base_url = settings->llm_base_url;
    core_config.auth_type = settings->llm_auth_type;
    core_config.timeout_ms = (uint32_t)strtoul(settings->llm_timeout_ms, NULL, 10);
    core_config.system_prompt = BASIC_DEMO_SYSTEM_PROMPT;
    core_config.append_session_turn = claw_memory_append_session_turn_callback;
    core_config.call_cap = claw_cap_call_from_core;
    core_config.task_stack_size = 8 * 1024;
    core_config.task_priority = 5;
    core_config.task_core = tskNO_AFFINITY;
    core_config.max_tool_iterations = 10;
    core_config.request_queue_len = 4;
    core_config.response_queue_len = 4;
    core_config.max_context_providers = 5;

    ESP_RETURN_ON_ERROR(claw_core_init(&core_config), TAG, "Failed to init claw_core");
    ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_provider),
                        TAG,
                        "Failed to add long-term memory provider");
    ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_session_history_provider),
                        TAG,
                        "Failed to add session history provider");
    ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_skills_list_provider),
                        TAG,
                        "Failed to add skills list provider");
    ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_active_skill_docs_provider),
                        TAG,
                        "Failed to add active skill docs provider");
    ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_cap_tools_provider),
                        TAG,
                        "Failed to add cap tools provider");
    ESP_RETURN_ON_ERROR(claw_core_start(), TAG, "Failed to start claw_core");
    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
    ESP_RETURN_ON_ERROR(basic_demo_cli_start(), TAG, "Failed to start CLI");

    return ESP_OK;
}
