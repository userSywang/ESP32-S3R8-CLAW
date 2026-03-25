/**
 * @file tools_get_time.c
 * @author sywang
 * @brief 
 * @version 0.1
 * @date 2026-03-05
 * 
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 * 
 */

/* ==================== [Includes] ========================================== */

#include "ec_config_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/ec_tools.h"

/* ==================== [Defines] =========================================== */

/* ==================== [Typedefs] ========================================== */

/* ==================== [Static Prototypes] ================================= */

static esp_err_t ec_tool_get_time_execute(const char *input_json, char *output, size_t output_size);
bool ec_tools_get_time_format_epoch_for_test(time_t epoch, char *out, size_t out_size);

/* ==================== [Static Variables] ================================== */

static const char *TAG = "tools_time";
static int64_t s_last_ntp_sync_us = 0;

static const ec_tools_t s_get_current_time = {
    .name = "get_current_time",
    .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
    .input_schema_json =
        "{\"type\":\"object\","
        "\"properties\":{},"
        "\"required\":[]}",
    .execute = ec_tool_get_time_execute,
};

/* ==================== [Macros] ============================================ */

/* ==================== [Global Functions] ================================== */

esp_err_t ec_tools_get_time(void)
{
    ec_tools_register(&s_get_current_time);
    return ESP_OK;
}

/* ==================== [Static Functions] ================================== */

static esp_err_t fetch_time_via_ntp(char *out, size_t out_size)
{
    esp_netif_sntp_deinit();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(EC_GET_TIME_NTP_SERVER);
    config.sync_cb = NULL;
    config.start = true;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    TickType_t ticks = pdMS_TO_TICKS(EC_GET_TIME_NTP_TIMEOUT_MS);
    err = esp_netif_sntp_sync_wait(ticks);
    esp_netif_sntp_deinit();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(err));
        return err;
    }

    s_last_ntp_sync_us = esp_timer_get_time();

    return ec_tools_get_time_format_epoch_for_test(time(NULL), out, out_size) ? ESP_OK : ESP_FAIL;
}

/* Format current system time (e.g. after NTP sync at boot). Return true if time looks valid. */
static bool format_system_time(char *out, size_t out_size)
{
    return ec_tools_get_time_format_epoch_for_test(time(NULL), out, out_size);
}

static esp_err_t ec_tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    bool system_time_valid = format_system_time(output, output_size);
    int64_t now_us = esp_timer_get_time();
    bool should_resync = !system_time_valid;

    if (system_time_valid && s_last_ntp_sync_us > 0) {
        int64_t age_s = (now_us - s_last_ntp_sync_us) / 1000000LL;
        should_resync = age_s >= EC_GET_TIME_RESYNC_MAX_AGE_S;
    }

    if (!should_resync) {
        ESP_LOGI(TAG, "Time (cached system): %s", output);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Fetching current time from %s...", EC_GET_TIME_NTP_SERVER);

    esp_err_t err = fetch_time_via_ntp(output, output_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Time (ntp): %s", output);
        return ESP_OK;
    }

    if (system_time_valid) {
        ESP_LOGW(TAG, "Use cached system time after NTP failure");
        return ESP_OK;
    }

    snprintf(output, output_size, "Error: failed to fetch time (%s)", esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", output);
    return err;
}

bool ec_tools_get_time_format_epoch_for_test(time_t epoch, char *out, size_t out_size)
{
    struct tm local;

    if (!out || out_size == 0) {
        return false;
    }

    setenv("TZ", EC_TIMEZONE, 1);
    tzset();

    if (localtime_r(&epoch, &local) == NULL) {
        return false;
    }

    if (local.tm_year + 1900 < 2020) {
        return false;
    }

    if (strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return false;
    }

    size_t used = strlen(out);
    const char *suffix = "北京时间 (UTC+8)";
    if (used + strlen(suffix) + 2 > out_size) {
        return false;
    }

    strcat(out, " ");
    strcat(out, suffix);
    return true;
}
