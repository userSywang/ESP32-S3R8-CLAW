/**
 * @file ec_agent.c
 * @author sywang
 * @brief
 * @version 0.1
 * @date 2026-03-05
 *
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 *
 */

/* ==================== [Includes] ========================================== */

#include "ec_agent.h"
#include "ec_config_internal.h"
#include "llm/ec_llm.h"

#include "ec_memory.h"
#include "ec_session.h"
#include "ec_tools.h"
#include "ec_skill_loader.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ==================== [Defines] =========================================== */

#define TOOL_RESULT_MAX_FOR_LLM 4096

#define EC_TOOL_OUTPUT_SIZE  (4 * 1024)
#define EC_AGENT_PROMPT_SCRATCH_SIZE 4096
#define EC_CRON_ADD_CACHE_SIZE 1024

#define AGENT_SYSTEM_PROMPT_STR \
        "You are Claw, a helpful and concise AI assistant running on an ESP32 device.\n"\
        "You communicate via Feishu and WebSocket.\n"\
        "Reply briefly to short messages (e.g. 你好, 在吗, 谢谢).\n"\
        "# Tools\n"\
        "- web_search: search current information.\n"\
        "- get_current_time: get date/time.\n"\
        "- device_status: get current device/network/Feishu health.\n"\
        "- read_file: read /spiffs/ files (path must start with " EC_FS_BASE "/).\n"\
        "- write_file: Write file.\n"\
        "- edit_file: edit file.\n"\
        "- list_dir: list files.\n"\
        "- cron_add: schedule task.\n"\
        "- cron_list: list tasks.\n"\
        "- cron_remove: remove task.\n\n"\
        "When using cron_add for feishu delivery, always set channel='feishu' and a valid chat_id like open_id:... or chat_id:....\n\n"\
        "For cron reminders, set cron_add.message to the exact future instruction to execute when the job fires. Preserve the user's intent, avoid confirmation wording, and do not ask follow-up questions inside the scheduled message.\n"\
        "If cron_add returns OK for a reminder in the current turn, do not call cron_add again for the same reminder.\n\n"\
        "When the user asks about device health, Wi-Fi, memory, or whether Feishu is connected, use device_status.\n\n"\
        "Use tools when needed. Provide your final answer as text after using tools.\n\n"\
        "## Memory\n"\
        "You have persistent memory stored on local flash:\n"\
        "- Long-term memory: " EC_FS_MEMORY_DIR "/MEMORY.md\n"\
        "- Daily notes: " EC_FS_MEMORY_DIR "/daily/<YYYY-MM-DD>.md\n\n"\
        "IMPORTANT: Actively use memory to remember things across conversations.\n"\
        "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"\
        "- When something noteworthy happens in a conversation, append it to today's daily note.\n"\
        "- Always read_file MEMORY.md before writing, so you can edit_file to update without losing existing content.\n"\
        "- Use get_current_time to know today's date before writing daily notes.\n"\
        "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"\
        "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n\n"\
        "## Skills\n"\
        "Skills are specialized instruction files stored in " EC_SKILLS_PREFIX ".\n"\
        "When a task matches a skill, read the full skill file for detailed instructions.\n"\
        "You can create new skills using write_file to " EC_SKILLS_PREFIX "<name>.md.\n"

/* ==================== [Typedefs] ========================================== */

/* ==================== [Static Prototypes] ================================= */

static void agent_loop_task(void *arg);
static bool parse_rule_based_reminder(const ec_msg_t *msg, char *tool_input, size_t tool_input_size,
                                      char *direct_reply, size_t direct_reply_size);
static bool parse_time_prefix(const char *text, int *hour, int *minute, const char **message_start);
static const char *skip_spaces(const char *s);
static void trim_trailing_spaces(char *s);
static bool extract_cron_job_id(char *content, char *job_id, size_t job_id_size);
static bool is_retryable_llm_error(esp_err_t err);
static esp_err_t call_llm_with_retry(const char *system_prompt, cJSON *messages, const char *tools_json,
                                     ec_llm_response_t *resp, int *attempts_used);
static const char *map_llm_error_to_user_message(esp_err_t err);

typedef struct {
    bool has_last_cron_add;
    char last_cron_add_input[EC_CRON_ADD_CACHE_SIZE];
    char last_cron_add_result[EC_CRON_ADD_CACHE_SIZE];
} agent_turn_state_t;

/* ==================== [Static Variables] ================================== */

static const char *TAG = "agent";
static QueueHandle_t s_inbound_queue;
static QueueHandle_t s_outbound_queue;

/* ==================== [Macros] ============================================ */

/* ==================== [Global Functions] ================================== */

esp_err_t ec_agent_start(void)
{
    s_inbound_queue = xQueueCreate(EC_BUS_QUEUE_LEN, sizeof(ec_msg_t));
    s_outbound_queue = xQueueCreate(EC_BUS_QUEUE_LEN, sizeof(ec_msg_t));

    if (!s_inbound_queue || !s_outbound_queue) {
        ESP_LOGE(TAG, "Failed to create message queues");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
                         agent_loop_task, "agent_loop",
                         EC_AGENT_STACK, NULL,
                         EC_AGENT_PRIO, NULL, EC_AGENT_CORE);

    if (ret == pdPASS) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t ec_agent_inbound(const ec_msg_t *msg)
{
    if (xQueueSend(s_inbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ec_agent_outbound(ec_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_outbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ==================== [Static Functions] ================================== */

static cJSON *build_assistant_content(const ec_llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const ec_llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);
        cJSON_AddNumberToObject(tool_block, "index", call->index);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static char *patch_tool_input_with_context(const ec_llm_tool_call_t *call, const ec_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, EC_CHAN_FEISHU) == 0 &&
            strcmp(msg->channel, EC_CHAN_FEISHU) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

static cJSON *build_tool_results(const ec_llm_response_t *resp, const ec_msg_t *msg,
                                 char *tool_output, size_t tool_output_size,
                                 agent_turn_state_t *turn_state)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const ec_llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        esp_err_t tool_err = ESP_OK;
        if (patched_input) {
            tool_input = patched_input;
        }

        tool_output[0] = '\0';

        if (turn_state && strcmp(call->name, "cron_add") == 0 &&
                turn_state->has_last_cron_add &&
                strcmp(turn_state->last_cron_add_input, tool_input) == 0) {
            snprintf(tool_output, tool_output_size, "%s",
                     turn_state->last_cron_add_result[0] ?
                     turn_state->last_cron_add_result :
                     "OK: cron_add already executed for this reminder in the current turn.");
            ESP_LOGI(TAG, "Skip duplicate cron_add in same turn");
        } else {
            tool_err = ec_tools_execute(call->name, tool_input, tool_output, tool_output_size);
            if (turn_state && strcmp(call->name, "cron_add") == 0 && tool_err == ESP_OK) {
                snprintf(turn_state->last_cron_add_input,
                         sizeof(turn_state->last_cron_add_input),
                         "%s", tool_input);
                snprintf(turn_state->last_cron_add_result,
                         sizeof(turn_state->last_cron_add_result),
                         "%s", tool_output);
                turn_state->has_last_cron_add = true;
            }
        }

        free(patched_input);

        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    if (!buf || size == 0 || offset >= size) {
        return size ? (size - 1) : 0;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return offset;
    }

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
        if (offset >= size) {
            offset = size - 1;
        }
    }

    if (offset >= size - 1) {
        fclose(f);
        return size - 1;
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

static esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    char *scratch = NULL;

    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;
    size_t cap = size - 1;

    off += snprintf(buf + off, size - off, AGENT_SYSTEM_PROMPT_STR);
    if (off > cap) {
        off = cap;
    }

    /* Bootstrap files */
    off = append_file(buf, size, off, EC_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, EC_USER_FILE, "User Info");

    scratch = calloc(1, EC_AGENT_PROMPT_SCRATCH_SIZE);
    if (!scratch) {
        ESP_LOGW(TAG, "Skipping optional prompt sections: out of memory");
    } else {
        if (off < cap && ec_memory_read_long_term(scratch, EC_AGENT_PROMPT_SCRATCH_SIZE) == ESP_OK && scratch[0]) {
            off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", scratch);
            if (off > cap) {
                off = cap;
            }
        }

        scratch[0] = '\0';
        if (off < cap && ec_memory_read_recent(scratch, EC_AGENT_PROMPT_SCRATCH_SIZE, 3) == ESP_OK && scratch[0]) {
            off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", scratch);
            if (off > cap) {
                off = cap;
            }
        }

        scratch[0] = '\0';
        size_t skills_len = ec_skill_loader_build_summary(scratch, EC_AGENT_PROMPT_SCRATCH_SIZE);
        if (off < cap && skills_len > 0) {
            off += snprintf(buf + off, size - off,
                            "\n## Available Skills\n\n"
                            "Available skills (use read_file to load full instructions):\n%s\n",
                            scratch);
            if (off > cap) {
                off = cap;
            }
        }

        free(scratch);
    }

    if (off >= size) {
        buf[size - 1] = '\0';
        off = size - 1;
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    ESP_LOGI(TAG, "prompt:%s", buf);

    return ESP_OK;
}

static void append_turn_context_prompt(char *prompt, size_t size, const ec_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
                prompt + off, size - off,
                "\n## Current Turn Context\n"
                "- source_channel: %s\n"
                "- source_chat_id: %s\n"
                "- If using cron_add for feishu in this turn, set channel='feishu' and chat_id to source_chat_id.\n"
                "- Never use chat_id 'cron' for feishu messages.\n",
                msg->channel[0] ? msg->channel : "(unknown)",
                msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static const char *skip_spaces(const char *s)
{
    while (s && *s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static void trim_trailing_spaces(char *s)
{
    size_t len = 0;
    if (!s) {
        return;
    }

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static bool extract_cron_job_id(char *content, char *job_id, size_t job_id_size)
{
    static const char *prefix = "[[CRON_JOB_ID:";
    char *end = NULL;
    char *payload = NULL;
    size_t id_len = 0;

    if (!content || !job_id || job_id_size == 0) {
        return false;
    }
    if (strncmp(content, prefix, strlen(prefix)) != 0) {
        return false;
    }

    end = strstr(content, "]]");
    if (!end) {
        return false;
    }
    id_len = (size_t)(end - (content + strlen(prefix)));
    if (id_len == 0 || id_len >= job_id_size) {
        return false;
    }

    memcpy(job_id, content + strlen(prefix), id_len);
    job_id[id_len] = '\0';

    payload = end + 2;
    if (payload[0] == '\n') {
        payload++;
    }
    memmove(content, payload, strlen(payload) + 1);
    return true;
}

static bool parse_time_prefix(const char *text, int *hour, int *minute, const char **message_start)
{
    int h = -1;
    int m = 0;
    int consumed = 0;
    const char *cursor = skip_spaces(text);

    if (!cursor) {
        return false;
    }

    if (*cursor == '在') {
        cursor = skip_spaces(cursor + 1);
    }

    if (sscanf(cursor, "%d:%d分提醒我%n", &h, &m, &consumed) == 2 ||
            sscanf(cursor, "%d:%d提醒我%n", &h, &m, &consumed) == 2 ||
            sscanf(cursor, "%d点%d分提醒我%n", &h, &m, &consumed) == 2 ||
            sscanf(cursor, "%d时%d分提醒我%n", &h, &m, &consumed) == 2 ||
            sscanf(cursor, "%d点%d提醒我%n", &h, &m, &consumed) == 2 ||
            sscanf(cursor, "%d点提醒我%n", &h, &consumed) == 1 ||
            sscanf(cursor, "%d时提醒我%n", &h, &consumed) == 1) {
        if (h < 0 || h > 23 || m < 0 || m > 59) {
            return false;
        }
        if (hour) {
            *hour = h;
        }
        if (minute) {
            *minute = m;
        }
        if (message_start) {
            *message_start = cursor + consumed;
        }
        return true;
    }

    return false;
}

static bool parse_rule_based_reminder(const ec_msg_t *msg, char *tool_input, size_t tool_input_size,
                                      char *direct_reply, size_t direct_reply_size)
{
    int hour = 0;
    int minute = 0;
    time_t now = 0;
    struct tm local_tm = {0};
    struct tm target_tm = {0};
    time_t target_epoch = 0;
    const char *message_start = NULL;
    const char *reply_day = "今天";
    char reminder_message[192] = {0};
    char job_name[32] = "提醒";
    char escaped_message[320] = {0};
    size_t j = 0;
    const char *cursor = NULL;
    char chat_part[160] = {0};

    if (!msg || !msg->content || !tool_input || !direct_reply) {
        return false;
    }

    if (!parse_time_prefix(msg->content, &hour, &minute, &message_start)) {
        return false;
    }

    cursor = skip_spaces(message_start);
    if (!cursor || !cursor[0]) {
        return false;
    }

    snprintf(reminder_message, sizeof(reminder_message), "%s", cursor);
    trim_trailing_spaces(reminder_message);
    if (!reminder_message[0]) {
        return false;
    }

    setenv("TZ", EC_TIMEZONE, 1);
    tzset();
    now = time(NULL);
    if (now <= 0 || !localtime_r(&now, &local_tm)) {
        return false;
    }

    target_tm = local_tm;
    target_tm.tm_hour = hour;
    target_tm.tm_min = minute;
    target_tm.tm_sec = 0;
    target_epoch = mktime(&target_tm);
    if (target_epoch <= now) {
        target_tm.tm_mday += 1;
        target_epoch = mktime(&target_tm);
        reply_day = "明天";
    }
    if (target_epoch <= now) {
        return false;
    }

    snprintf(job_name, sizeof(job_name), "%.24s", reminder_message);
    size_t escaped_len = 0;
    for (j = 0; reminder_message[j] && escaped_len < sizeof(escaped_message) - 2; j++) {
        char c = reminder_message[j];
        if (c == '"' || c == '\\') {
            escaped_message[escaped_len++] = '\\';
        }
        escaped_message[escaped_len++] = c;
    }
    escaped_message[escaped_len] = '\0';

    if (msg->channel[0]) {
        if (strcmp(msg->channel, EC_CHAN_FEISHU) == 0 && msg->chat_id[0]) {
            snprintf(chat_part, sizeof(chat_part),
                     ",\"channel\":\"%.*s\",\"chat_id\":\"%.*s\"",
                     (int)sizeof(msg->channel) - 1, msg->channel,
                     (int)sizeof(msg->chat_id) - 1, msg->chat_id);
        } else {
            snprintf(chat_part, sizeof(chat_part), ",\"channel\":\"%.*s\"",
                     (int)sizeof(msg->channel) - 1, msg->channel);
        }
    }

    snprintf(tool_input, tool_input_size,
             "{\"name\":\"%s\",\"schedule_type\":\"at\",\"at_epoch\":%lld,"
             "\"message\":\"%s\"%s}",
             job_name, (long long)target_epoch, escaped_message, chat_part);

    snprintf(direct_reply, direct_reply_size,
             "好的，已为您设置%s%02d:%02d的提醒：%s",
             reply_day, hour, minute, reminder_message);
    return true;
}

static bool is_retryable_llm_error(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT ||
           err == ESP_ERR_HTTP_CONNECT ||
           err == ESP_ERR_HTTP_FETCH_HEADER ||
           err == ESP_ERR_HTTP_WRITE_DATA ||
           err == ESP_FAIL;
}

static esp_err_t call_llm_with_retry(const char *system_prompt, cJSON *messages, const char *tools_json,
                                     ec_llm_response_t *resp, int *attempts_used)
{
    esp_err_t err = ec_llm_chat_tools(system_prompt, messages, tools_json, resp);
    int attempts = 1;

    if (err != ESP_OK && is_retryable_llm_error(err)) {
        ESP_LOGW(TAG, "Transient LLM error (%s), retrying once", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = ec_llm_chat_tools(system_prompt, messages, tools_json, resp);
        attempts++;
    }

    if (attempts_used) {
        *attempts_used = attempts;
    }
    return err;
}

static const char *map_llm_error_to_user_message(esp_err_t err)
{
    switch (err) {
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_HTTP_CONNECT:
        case ESP_ERR_HTTP_FETCH_HEADER:
        case ESP_ERR_HTTP_WRITE_DATA:
            return "模型请求超时或网络异常，请稍后重试。";
        case ESP_ERR_INVALID_ARG:
            return "模型配置无效，请检查 API Key、模型名和地址。";
        case ESP_ERR_NO_MEM:
            return "设备当前内存不足，请稍后重试。";
        default:
            return "处理请求时发生错误，请稍后重试。";
    }
}

static void agent_loop_task(void *arg)
{
    esp_err_t err = ESP_OK;
    bool sent_working_status = false;
    char *final_text = NULL; // 存储最终文本回复，用于发送给用户
    esp_err_t last_llm_err = ESP_OK;
    int llm_attempts = 0;
    static uint32_t s_consecutive_llm_failures = 0;
    char *system_prompt = (char *)malloc(EC_CONTEXT_BUF_SIZE + EC_LLM_STREAM_BUF_SIZE + EC_TOOL_OUTPUT_SIZE);
    if (!system_prompt) {
        ESP_LOGE(TAG, "Failed to allocate agent buffers");
        vTaskDelete(NULL);
        return;
    }

    char *history_json = system_prompt + EC_CONTEXT_BUF_SIZE;
    char *tool_output = history_json + EC_LLM_STREAM_BUF_SIZE;

    // 获取所有工具的 JSON 描述，供后续 LLM 调用时使用
    const char *tools_json = ec_tools_get_json();

    while (1) {
        // 获取入站消息，统一进行处理
        // 该消息来源于ws、飞书等适配器，或者系统适配器（定时任务触发等）
        ec_msg_t msg = {0};
        int64_t turn_started_us = 0;
        char cron_job_id[16] = {0};
        sent_working_status = false;
        final_text = NULL;
        last_llm_err = ESP_OK;
        llm_attempts = 0;
        
        if (xQueueReceive(s_inbound_queue, &msg, UINT32_MAX) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        turn_started_us = esp_timer_get_time();
        (void)extract_cron_job_id(msg.content, cron_job_id, sizeof(cron_job_id));

        char cron_tool_input[512] = {0};
        char cron_direct_reply[256] = {0};
        if (parse_rule_based_reminder(&msg, cron_tool_input, sizeof(cron_tool_input),
                                      cron_direct_reply, sizeof(cron_direct_reply))) {
            char cron_output[EC_TOOL_OUTPUT_SIZE] = {0};
            err = ec_tools_execute("cron_add", cron_tool_input, cron_output, sizeof(cron_output));
            if (err == ESP_OK) {
                final_text = strdup(cron_direct_reply);
            } else {
                final_text = strdup("设置提醒失败，请检查时间格式或稍后重试。");
            }
            goto finalize_message;
        }

        // 构建系统提示词，包含基本信息和当前消息的上下文（来源渠道、chat_id等）
        context_build_system_prompt(system_prompt, EC_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, EC_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        // 读取当前会话历史，构建消息数组供 LLM 使用
        ec_session_get_history_json(msg.chat_id, history_json,
                                    EC_LLM_STREAM_BUF_SIZE, EC_AGENT_MAX_HISTORY);
        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) {
            messages = cJSON_CreateArray();
        }
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);
        agent_turn_state_t turn_state = {0};

        // 进入 ReAct 循环，最多迭代 EC_AGENT_MAX_TOOL_ITER 次
        for (size_t i = 0; i < EC_AGENT_MAX_TOOL_ITER; i++) {
#if EC_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status &&
                    strcmp(msg.channel, EC_CHAN_SYSTEM) != 0 &&
                    (esp_timer_get_time() - turn_started_us) >= (int64_t)EC_AGENT_WORKING_STATUS_DELAY_MS * 1000) {
                ec_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("\xF0\x9F\x90\xB1" "agent is working...");
                if (status.content) {
                    if (xQueueSend(s_outbound_queue, &status, pdMS_TO_TICKS(1000)) != pdTRUE) {
                        ESP_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif
            ec_llm_response_t resp;
            err = call_llm_with_retry(system_prompt, messages, tools_json, &resp, &llm_attempts);

            if (err != ESP_OK) {
                s_consecutive_llm_failures++;
                last_llm_err = err;
                ESP_LOGE(TAG, "LLM call failed after %d attempt(s): %s (consecutive_failures=%lu)",
                         llm_attempts, esp_err_to_name(err), (unsigned long)s_consecutive_llm_failures);
                break;
            }
            s_consecutive_llm_failures = 0;

            if (!resp.tool_use) {
                // 正常对话结束，保存最终文本并退出循环
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                }
                ec_llm_response_free(&resp);
                break;
            }


            // 构建助手消息，包含文本回复和工具调用信息，供工具执行结果使用
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            // 执行工具并将结果追加到消息数组中，供下一轮 LLM 调用使用
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, EC_TOOL_OUTPUT_SIZE, &turn_state);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            ec_llm_response_free(&resp);
        }

finalize_message:
        if (final_text && final_text[0]) {
            // 保存用户消息和助手回复到会话历史中
            esp_err_t save_user = ec_session_append(msg.chat_id, "user", msg.content);
            esp_err_t save_asst = ec_session_append(msg.chat_id, "assistant", final_text);
            if (save_user != ESP_OK || save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for chat %s (user=%s, assistant=%s)",
                         msg.chat_id,
                         esp_err_to_name(save_user),
                         esp_err_to_name(save_asst));
            } else {
                ESP_LOGI(TAG, "Session saved for chat %s", msg.chat_id);
            }

            // 推送消息到出站队列，发送给用户
            ec_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(final_text));
            if (xQueueSend(s_outbound_queue, &out, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(TAG, "Outbound queue full, drop final response");
                free(final_text);
                if (cron_job_id[0]) {
                    (void)ec_tools_cron_mark_dispatch_result(cron_job_id, false);
                }
            } else {
                final_text = NULL;
                if (cron_job_id[0]) {
                    (void)ec_tools_cron_mark_dispatch_result(cron_job_id, true);
                }
            }
        } else {
            /* Error or empty response */
            free(final_text);
            ec_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup(map_llm_error_to_user_message(last_llm_err));
            if (out.content) {
                if (xQueueSend(s_outbound_queue, &out, pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGW(TAG, "Outbound queue full, drop error response");
                    free(out.content);
                }
            }
            if (cron_job_id[0]) {
                (void)ec_tools_cron_mark_dispatch_result(cron_job_id, false);
            }
        }
        free(msg.content);
#if CONFIG_SPIRAM
        ESP_LOGI(TAG, "Free PSRAM: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
        ESP_LOGI(TAG, "Free internal heap: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
    }

}

char *ec_agent_patch_tool_input_with_context_for_test(const char *tool_name, const char *input_json,
                                                      const ec_msg_t *msg)
{
    ec_llm_tool_call_t call = {0};

    if (tool_name) {
        strncpy(call.name, tool_name, sizeof(call.name) - 1);
    }
    call.input = (char *)(input_json ? input_json : "{}");
    call.input_len = input_json ? strlen(input_json) : 2;

    return patch_tool_input_with_context(&call, msg);
}

esp_err_t ec_agent_build_system_prompt_for_test(char *buf, size_t size)
{
    return context_build_system_prompt(buf, size);
}

void ec_agent_append_turn_context_for_test(char *prompt, size_t size, const ec_msg_t *msg)
{
    append_turn_context_prompt(prompt, size, msg);
}
