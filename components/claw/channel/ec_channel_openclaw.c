/**
 * @file ec_channel_openclaw.c
 * @author sywang
 * @brief OpenClaw peer channel over dedicated WebSocket client
 * @version 0.1
 * @date 2026-03-23
 *
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 *
 */

#include "ec_config_internal.h"
#include "channel/ec_channel_openclaw.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/ec_agent.h"
#include "core/ec_channel.h"

#define OPENCLAW_TASK_STACK      (8 * 1024)
#define OPENCLAW_TASK_PRIO       5
#define OPENCLAW_TASK_CORE       0
#define OPENCLAW_RECONNECT_MS    5000
#define OPENCLAW_MAX_FRAME       2048

typedef enum {
    OPENCLAW_STATE_DISCONNECTED = 0,
    OPENCLAW_STATE_CONNECTING,
    OPENCLAW_STATE_CONNECTED,
} openclaw_state_t;

static void handle_ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
static void openclaw_task(void *arg);
static bool openclaw_network_ready(void);
static esp_err_t openclaw_send_json(cJSON *root);
static esp_err_t ec_channel_openclaw_start(void);
static esp_err_t ec_channel_openclaw_send(const ec_msg_t *msg);

static const char *TAG = "openclaw";
static esp_websocket_client_handle_t s_ws_client = NULL;
static TaskHandle_t s_openclaw_task = NULL;
static volatile openclaw_state_t s_state = OPENCLAW_STATE_DISCONNECTED;
static bool s_connected = false;
static uint32_t s_connect_count = 0;
static uint32_t s_disconnect_count = 0;
static int64_t s_last_recv_at_us = 0;
static int64_t s_last_send_at_us = 0;

static const ec_channel_t s_driver = {
    .name = EC_CHAN_OPENCLAW,
    .vtable = {
        .start = ec_channel_openclaw_start,
        .send = ec_channel_openclaw_send,
    }
};

esp_err_t ec_channel_openclaw(void)
{
    return ec_channel_register(&s_driver);
}

void ec_channel_openclaw_get_health(bool *connected,
                                    uint32_t *connect_count,
                                    uint32_t *disconnect_count,
                                    int64_t *last_recv_age_ms,
                                    int64_t *last_send_age_ms)
{
    int64_t now_us = esp_timer_get_time();

    if (connected) {
        *connected = s_connected;
    }
    if (connect_count) {
        *connect_count = s_connect_count;
    }
    if (disconnect_count) {
        *disconnect_count = s_disconnect_count;
    }
    if (last_recv_age_ms) {
        *last_recv_age_ms = s_last_recv_at_us > 0 ? (now_us - s_last_recv_at_us) / 1000 : -1;
    }
    if (last_send_age_ms) {
        *last_send_age_ms = s_last_send_at_us > 0 ? (now_us - s_last_send_at_us) / 1000 : -1;
    }
}

static bool openclaw_network_ready(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};

    if (!sta) {
        return false;
    }

    return esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

static esp_err_t openclaw_send_json(cJSON *root)
{
    char *payload;
    int ret;

    if (!root || !s_ws_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_websocket_client_send_text(s_ws_client, payload, strlen(payload), pdMS_TO_TICKS(5000));
    free(payload);

    if (ret < 0) {
        return ESP_FAIL;
    }

    s_last_send_at_us = esp_timer_get_time();
    return ESP_OK;
}

static esp_err_t ec_channel_openclaw_send(const ec_msg_t *msg)
{
    cJSON *root;
    esp_err_t err;

    if (!msg || !msg->content) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "channel", EC_CHAN_OPENCLAW);
    cJSON_AddStringToObject(root, "device_id", EC_OPENCLAW_DEVICE_ID);
    cJSON_AddStringToObject(root, "chat_id", msg->chat_id);
    cJSON_AddStringToObject(root, "content", msg->content);

    err = openclaw_send_json(root);
    cJSON_Delete(root);
    return err;
}

static void handle_ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    (void)arg;
    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        cJSON *hello = cJSON_CreateObject();
        ESP_LOGI(TAG, "OpenClaw channel connected");
        s_connected = true;
        s_state = OPENCLAW_STATE_CONNECTED;
        s_connect_count++;
        if (!hello) {
            break;
        }
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "channel", EC_CHAN_OPENCLAW);
        cJSON_AddStringToObject(hello, "device_id", EC_OPENCLAW_DEVICE_ID);
        cJSON_AddStringToObject(hello, "auth_token", EC_OPENCLAW_AUTH_TOKEN);
        cJSON_AddStringToObject(hello, "capabilities", "message,response,heartbeat");
        openclaw_send_json(hello);
        cJSON_Delete(hello);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "OpenClaw channel disconnected");
        s_connected = false;
        s_state = OPENCLAW_STATE_DISCONNECTED;
        s_disconnect_count++;
        break;
    case WEBSOCKET_EVENT_DATA: {
        cJSON *root;
        cJSON *type;
        cJSON *chat_id;
        cJSON *content;
        ec_msg_t msg = {0};

        if (!data || !data->data_ptr || data->data_len <= 0 || data->op_code != 0x01) {
            break;
        }

        root = cJSON_ParseWithLength((const char *)data->data_ptr, data->data_len);
        if (!root) {
            ESP_LOGW(TAG, "OpenClaw payload parse failed");
            break;
        }

        type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "message") != 0) {
            cJSON_Delete(root);
            break;
        }

        chat_id = cJSON_GetObjectItem(root, "chat_id");
        content = cJSON_GetObjectItem(root, "content");
        if (!cJSON_IsString(chat_id) || !chat_id->valuestring ||
            !cJSON_IsString(content) || !content->valuestring) {
            cJSON_Delete(root);
            break;
        }

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, EC_CHAN_OPENCLAW, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id->valuestring, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content->valuestring);
        cJSON_Delete(root);

        if (!msg.content) {
            break;
        }

        s_last_recv_at_us = esp_timer_get_time();
        ESP_LOGI(TAG, "OpenClaw inbound chat_id=%s content_len=%d", msg.chat_id, (int)strlen(msg.content));
        if (ec_agent_inbound(&msg) != ESP_OK) {
            ESP_LOGW(TAG, "OpenClaw inbound queue full, drop message");
            free(msg.content);
        }
        break;
    }
    default:
        break;
    }
}

static void openclaw_task(void *arg)
{
    (void)arg;

    while (1) {
        esp_websocket_client_config_t cfg = {0};
        int wait_connect_ms = 0;

        if (!EC_OPENCLAW_WS_URL[0]) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        if (!openclaw_network_ready()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        cfg.uri = EC_OPENCLAW_WS_URL;
        cfg.reconnect_timeout_ms = OPENCLAW_RECONNECT_MS;
        cfg.network_timeout_ms = 15000;
        cfg.disable_auto_reconnect = true;
        cfg.buffer_size = OPENCLAW_MAX_FRAME;
        cfg.task_stack = 6144;
        if (strncmp(EC_OPENCLAW_WS_URL, "wss://", 6) == 0) {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
        }

        s_ws_client = esp_websocket_client_init(&cfg);
        if (!s_ws_client) {
            vTaskDelay(pdMS_TO_TICKS(OPENCLAW_RECONNECT_MS));
            continue;
        }

        s_connected = false;
        s_state = OPENCLAW_STATE_CONNECTING;
        esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, handle_ws_event, NULL);
        esp_websocket_client_start(s_ws_client);

        while (s_state == OPENCLAW_STATE_CONNECTING && wait_connect_ms < 15000) {
            vTaskDelay(pdMS_TO_TICKS(200));
            wait_connect_ms += 200;
        }

        if (s_state != OPENCLAW_STATE_CONNECTED) {
            ESP_LOGW(TAG, "OpenClaw connect timeout, retry later");
            esp_websocket_client_stop(s_ws_client);
            esp_websocket_client_destroy(s_ws_client);
            s_ws_client = NULL;
            s_connected = false;
            s_state = OPENCLAW_STATE_DISCONNECTED;
            vTaskDelay(pdMS_TO_TICKS(OPENCLAW_RECONNECT_MS));
            continue;
        }

        while (s_state == OPENCLAW_STATE_CONNECTED && s_ws_client &&
               esp_websocket_client_is_connected(s_ws_client)) {
            cJSON *ping = cJSON_CreateObject();
            if (ping) {
                cJSON_AddStringToObject(ping, "type", "heartbeat");
                cJSON_AddStringToObject(ping, "device_id", EC_OPENCLAW_DEVICE_ID);
                openclaw_send_json(ping);
                cJSON_Delete(ping);
            }
            vTaskDelay(pdMS_TO_TICKS(EC_OPENCLAW_PING_INTERVAL_S * 1000));
        }

        ESP_LOGW(TAG, "OpenClaw channel loop ended, reconnecting");
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        s_connected = false;
        s_state = OPENCLAW_STATE_DISCONNECTED;
        vTaskDelay(pdMS_TO_TICKS(OPENCLAW_RECONNECT_MS));
    }
}

static esp_err_t ec_channel_openclaw_start(void)
{
    BaseType_t ok;

    if (!EC_OPENCLAW_WS_URL[0]) {
        ESP_LOGI(TAG, "OpenClaw channel disabled (EC_OPENCLAW_WS_URL empty)");
        return ESP_OK;
    }

    if (s_openclaw_task) {
        ESP_LOGI(TAG, "OpenClaw channel already running");
        return ESP_OK;
    }

    ok = xTaskCreatePinnedToCore(openclaw_task, "openclaw_ws",
                                 OPENCLAW_TASK_STACK, NULL,
                                 OPENCLAW_TASK_PRIO, &s_openclaw_task,
                                 OPENCLAW_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OpenClaw task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OpenClaw channel started");
    return ESP_OK;
}
