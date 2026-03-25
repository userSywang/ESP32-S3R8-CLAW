/**
 * @file tools_device_status.c
 * @brief Device/network health tool
 */

#include "ec_config_internal.h"
#include "core/ec_tools.h"
#include "channel/ec_channel_feishu.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"

static esp_err_t ec_tool_device_status_execute(const char *input_json, char *output, size_t output_size);

static const ec_tools_t s_device_status = {
    .name = "device_status",
    .description = "Get current device health including Wi-Fi, IP, RSSI, free heap/PSRAM, and Feishu connection state.",
    .input_schema_json =
        "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
    .execute = ec_tool_device_status_execute,
};

esp_err_t ec_tools_device_status(void)
{
    ec_tools_register(&s_device_status);
    return ESP_OK;
}

static esp_err_t ec_tool_device_status_execute(const char *input_json, char *output, size_t output_size)
{
    esp_netif_t *sta = NULL;
    esp_netif_ip_info_t ip_info = {0};
    wifi_ap_record_t ap_info = {0};
    esp_err_t ip_err = ESP_FAIL;
    esp_err_t ap_err = ESP_FAIL;
    bool feishu_connected = false;
    uint32_t feishu_connect_count = 0;
    uint32_t feishu_disconnect_count = 0;
    int64_t last_event_age_ms = -1;
    int64_t last_send_age_ms = -1;
    const char *wifi_state = "down";
    char ip_buf[16] = "0.0.0.0";
    int rssi = 0;

    (void)input_json;

    sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        ip_err = esp_netif_get_ip_info(sta, &ip_info);
    }
    if (ip_err == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
        wifi_state = "up";
    }

    ap_err = esp_wifi_sta_get_ap_info(&ap_info);
    if (ap_err == ESP_OK) {
        rssi = ap_info.rssi;
    }

    ec_channel_feishu_get_health(&feishu_connected,
                                 &feishu_connect_count,
                                 &feishu_disconnect_count,
                                 &last_event_age_ms,
                                 &last_send_age_ms);

    snprintf(output, output_size,
             "device_status:\n"
             "- wifi: %s\n"
             "- ip: %s\n"
             "- rssi: %d\n"
             "- free_heap: %u\n"
             "- free_psram: %u\n"
             "- feishu_ws: %s\n"
             "- feishu_connect_count: %" PRIu32 "\n"
             "- feishu_disconnect_count: %" PRIu32 "\n"
             "- last_feishu_event_ms: %" PRId64 "\n"
             "- last_feishu_send_ms: %" PRId64,
             wifi_state,
             ip_buf,
             rssi,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             feishu_connected ? "up" : "down",
             feishu_connect_count,
             feishu_disconnect_count,
             last_event_age_ms,
             last_send_age_ms);

    return ESP_OK;
}
