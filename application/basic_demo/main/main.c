/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_clawgent.h"
#include "basic_demo_settings.h"
#include "basic_demo_wifi.h"
#include "config_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wear_levelling.h"

static const char *TAG = "basic_demo";
static basic_demo_settings_t s_settings = {0};

#define BASIC_DEMO_FATFS_BASE_PATH       "/fatfs/data"
#define BASIC_DEMO_FATFS_PARTITION_LABEL "storage"
#define BASIC_DEMO_ENABLE_MEM_LOG        (0)

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(BASIC_DEMO_FATFS_BASE_PATH,
                                           BASIC_DEMO_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_info(BASIC_DEMO_FATFS_BASE_PATH, &total, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to query FATFS info: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "FATFS mounted total=%u used=%u",
                 (unsigned int)total,
                 (unsigned int)(total - free_bytes));
    }

    return ESP_OK;
}

#if BASIC_DEMO_ENABLE_MEM_LOG

static TaskStatus_t s_task_status_snapshot[24];

static void print_task_stack_info(void)
{
    UBaseType_t count = uxTaskGetSystemState(s_task_status_snapshot,
                                             sizeof(s_task_status_snapshot) / sizeof(s_task_status_snapshot[0]),
                                             NULL);

    for (UBaseType_t i = 0; i < count; i++) {
        ESP_LOGI(TAG,
                 "Task %s  %u",
                 s_task_status_snapshot[i].pcTaskName,
                 s_task_status_snapshot[i].usStackHighWaterMark);
    }
}

/* Periodic task: print internal free, minimum free, and PSRAM free every 20s */
static void memory_monitor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Memory: internal_free=%u bytes, internal_min_free=%u bytes, psram_free=%u bytes",
                 (unsigned)internal_free, (unsigned)internal_min, (unsigned)psram_free);
        // print_task_stack_info();
    }
}

#endif

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting basic_demo");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(basic_demo_settings_init());
    ESP_ERROR_CHECK(basic_demo_settings_load(&s_settings));
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(basic_demo_wifi_init());
    ESP_ERROR_CHECK(config_http_server_init(BASIC_DEMO_FATFS_BASE_PATH));

    if (basic_demo_wifi_start(s_settings.wifi_ssid, s_settings.wifi_password) == ESP_OK) {
        ESP_ERROR_CHECK(config_http_server_start());
        if (basic_demo_wifi_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi ready: %s", basic_demo_wifi_get_ip());
        } else {
            ESP_LOGW(TAG, "Wi-Fi connection timed out");
        }
    } else {
        ESP_LOGW(TAG, "Continuing without Wi-Fi");
    }

    ESP_ERROR_CHECK(app_clawgent_start(&s_settings));

#if BASIC_DEMO_ENABLE_MEM_LOG
    /* Start memory monitor: print internal free, min free, PSRAM free every 20s */
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif
}
