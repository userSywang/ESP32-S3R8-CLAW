/**
 * @file embed_claw.c
 * @author cangyu (sky.kirto@qq.com)
 * @brief
 * @version 0.1
 * @date 2026-03-06
 *
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 *
 */

/* ==================== [Includes] ========================================== */

#include "embed_claw.h"

#include "core/ec_agent.h"
#include "core/ec_channel.h"
#include "core/ec_skill_loader.h"
#include "core/ec_tools.h"
#include "channel/ec_channel_feishu.h"
#include "llm/ec_llm.h"
#include "ec_config_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
/* ==================== [Defines] =========================================== */

#define EC_OUTBOUND_TASK_STACK 8192
#define EC_OUTBOUND_TASK_PRIO  5
#define EC_HEALTH_TASK_STACK   4096
#define EC_HEALTH_TASK_PRIO    3

/* ==================== [Typedefs] ========================================== */

/* ==================== [Static Prototypes] ================================= */

static void outbound_dispatch_task(void *arg);
static void health_monitor_task(void *arg);

/* ==================== [Static Variables] ================================== */

static const ec_llm_provider_ctx_t s_llm_provider_ctx = {
    .api_key = EC_LLM_API_KEY,
    .model = EC_LLM_MODEL,
    .url = EC_LLM_API_URL,
};

static const char *TAG = "embed_claw";
static bool s_started = false;
static TaskHandle_t s_health_task = NULL;

/* ==================== [Macros] ============================================ */

/* ==================== [Global Functions] ================================== */

esp_err_t ec_embed_claw_start(void)
{
    esp_err_t err;

    if (s_started) {
        ESP_LOGI(TAG, "EmbedClaw already started, skip duplicate start");
        return ESP_OK;
    }

    err = ec_channel_register_all();
    if (err != ESP_OK) {
        return err;
    }
    err = ec_skill_loader_init();
    if (err != ESP_OK) {
        return err;
    }
    err = ec_tools_register_all();
    if (err != ESP_OK) {
        return err;
    }
    err = ec_llm_init(LLM_TYPE_OPENAI, &s_llm_provider_ctx);
    if (err != ESP_OK) {
        return err;
    }
    err = ec_agent_start();
    if (err != ESP_OK) {
        return err;
    }
    err = ec_channel_start("feishu");
    if (err != ESP_OK) {
        return err;
    }
    err = ec_channel_start("websocket");
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t task_ok = xTaskCreate(
                             outbound_dispatch_task,
                             "ec_outbound",
                             EC_OUTBOUND_TASK_STACK,
                             NULL,
                             EC_OUTBOUND_TASK_PRIO,
                             NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "outbound task create failed");
        return ESP_FAIL;
    }

    if (!s_health_task) {
        task_ok = xTaskCreate(
                      health_monitor_task,
                      "ec_health",
                      EC_HEALTH_TASK_STACK,
                      NULL,
                      EC_HEALTH_TASK_PRIO,
                      &s_health_task);
        if (task_ok != pdPASS) {
            ESP_LOGE(TAG, "health task create failed");
            return ESP_FAIL;
        }
    }

    s_started = true;
    ESP_LOGI(TAG, "EmbedClaw started");

    return ESP_OK;
}

/* ==================== [Static Functions] ================================== */

static void outbound_dispatch_task(void *arg)
{
    (void)arg;

    while (1) {
        ec_msg_t msg = {0};
        esp_err_t err = ec_agent_outbound(&msg, UINT32_MAX);
        if (err != ESP_OK) {
            continue;
        }

        ESP_LOGI(TAG, "Dispatch outbound to %s:%s (%d bytes)",
                 msg.channel, msg.chat_id, msg.content ? (int)strlen(msg.content) : 0);
        ESP_LOGI(TAG, "ec_outbound stack free before send: %u bytes",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        err = ec_channel_send(&msg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Send outbound to:%s failed: %s",
                     msg.chat_id, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Outbound delivered to %s:%s", msg.channel, msg.chat_id);
        }

        ESP_LOGI(TAG, "ec_outbound stack free after send: %u bytes",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        free(msg.content);
    }
}

static void health_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        bool feishu_connected = false;
        uint32_t feishu_connect_count = 0;
        uint32_t feishu_disconnect_count = 0;
        int64_t last_event_age_ms = -1;
        int64_t last_send_age_ms = -1;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {0};
        wifi_ap_record_t ap_info = {0};
        esp_err_t ip_err = sta ? esp_netif_get_ip_info(sta, &ip_info) : ESP_ERR_INVALID_STATE;
        esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap_info);

        ec_channel_feishu_get_health(&feishu_connected,
                                     &feishu_connect_count,
                                     &feishu_disconnect_count,
                                     &last_event_age_ms,
                                     &last_send_age_ms);

        ESP_LOGI(TAG,
                 "health wifi=%s ip=" IPSTR " rssi=%d heap=%u psram=%u feishu_ws=%s connect=%" PRIu32 " disconnect=%" PRIu32 " last_event_ms=%" PRId64 " last_send_ms=%" PRId64 " outbound_stack=%u",
                 (ip_err == ESP_OK && ip_info.ip.addr != 0) ? "up" : "down",
                 IP2STR(&ip_info.ip),
                 (ap_err == ESP_OK) ? ap_info.rssi : 0,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 feishu_connected ? "up" : "down",
                 feishu_connect_count,
                 feishu_disconnect_count,
                 last_event_age_ms,
                 last_send_age_ms,
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

        vTaskDelay(pdMS_TO_TICKS(EC_HEALTH_LOG_INTERVAL_MS));
    }
}
