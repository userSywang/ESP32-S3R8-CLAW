/**
 * @file ec_config_internal.h
 * @author cangyu (sky.kirto@qq.com)
 * @brief 
 * @version 0.1
 * @date 2026-03-12
 * 
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 * 
 */

#ifndef __EC_CONFIG_INTERNAL_H__
#define __EC_CONFIG_INTERNAL_H__

/* ==================== [Includes] ========================================== */

/* Project/test overrides are injected with compiler `-include` options. */

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== [Defines] =========================================== */

#ifndef EC_FS_BASE
#define EC_FS_BASE                  "/spiffs"
#endif

#ifndef EC_FS_CONFIG_DIR
#define EC_FS_CONFIG_DIR            EC_FS_BASE "/config"
#endif

#ifndef EC_FS_SESSION_DIR
#define EC_FS_SESSION_DIR           EC_FS_BASE "/session"
#endif

#ifndef EC_FS_MEMORY_DIR
#define EC_FS_MEMORY_DIR            EC_FS_BASE "/memory"
#endif

#ifndef EC_MEMORY_FILE
#define EC_MEMORY_FILE              EC_FS_MEMORY_DIR "/MEMORY.md"
#endif

#ifndef EC_SOUL_FILE
#define EC_SOUL_FILE                EC_FS_CONFIG_DIR "/SOUL.md"
#endif

#ifndef EC_USER_FILE
#define EC_USER_FILE                EC_FS_CONFIG_DIR "/USER.md"
#endif

#ifndef EC_SESSION_MAX_MSGS
#define EC_SESSION_MAX_MSGS         20
#endif

#ifndef EC_WS_PORT
#define EC_WS_PORT                  18789
#endif

#ifndef EC_WS_MAX_CLIENTS
#define EC_WS_MAX_CLIENTS           4
#endif

#ifndef EC_MAX_CRON_JOBS
#define EC_MAX_CRON_JOBS            16
#endif

#ifndef EC_CRON_TASK_PRIORITY
#define EC_CRON_TASK_PRIORITY       4
#endif

#ifndef EC_CRON_TASK_STACK_SIZE
#define EC_CRON_TASK_STACK_SIZE     4096
#endif

#ifndef EC_CRON_CHECK_INTERVAL_MS
#define EC_CRON_CHECK_INTERVAL_MS   (60 * 1000)
#endif

#ifndef EC_GET_TIME_NTP_SERVER
#define EC_GET_TIME_NTP_SERVER      "ntp.aliyun.com"
#endif

#ifndef EC_TIMEZONE
#define EC_TIMEZONE                 "UTC-8"
#endif

#ifndef EC_SEARCH_BUF_SIZE
#define EC_SEARCH_BUF_SIZE          (16 * 1024)
#endif

#ifndef EC_SEARCH_RESULT_COUNT
#define EC_SEARCH_RESULT_COUNT      5
#endif

#ifndef EC_QUERY_UTF8_MAX
#define EC_QUERY_UTF8_MAX           256
#endif

#ifndef EC_BUS_QUEUE_LEN
#define EC_BUS_QUEUE_LEN            16
#endif

#ifndef EC_SECRET_SEARCH_KEY
#define EC_SECRET_SEARCH_KEY        ""
#endif

#ifndef EC_CRON_FILE
#define EC_CRON_FILE                EC_FS_BASE "/cron.json"
#endif

#ifndef EC_SKILLS_PREFIX
#define EC_SKILLS_PREFIX            EC_FS_BASE "/skills/"
#endif

#ifndef EC_LLM_API_URL
#define EC_LLM_API_URL              "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions"
#endif

#ifndef EC_LLM_API_KEY
#define EC_LLM_API_KEY              ""
#endif

#ifndef EC_LLM_MODEL
#define EC_LLM_MODEL                "qwen-plus"
#endif

#ifndef EC_LLM_MAX_TOKENS
#define EC_LLM_MAX_TOKENS           4096
#endif

#ifndef EC_LLM_STREAM_BUF_SIZE
#define EC_LLM_STREAM_BUF_SIZE      (32 * 1024)
#endif

#ifndef EC_MAX_TOOL_CALLS
#define EC_MAX_TOOL_CALLS           4
#endif

#ifndef EC_AGENT_STACK
#define EC_AGENT_STACK              (24 * 1024)
#endif

#ifndef EC_AGENT_PRIO
#define EC_AGENT_PRIO               6
#endif

#ifndef EC_AGENT_CORE
#define EC_AGENT_CORE               0
#endif

#ifndef EC_CONTEXT_BUF_SIZE
#define EC_CONTEXT_BUF_SIZE         (16 * 1024)
#endif

#ifndef EC_AGENT_MAX_TOOL_ITER
#define EC_AGENT_MAX_TOOL_ITER      10
#endif

#ifndef EC_AGENT_MAX_HISTORY
#define EC_AGENT_MAX_HISTORY        20
#endif

#ifndef EC_AGENT_SEND_WORKING_STATUS
#define EC_AGENT_SEND_WORKING_STATUS 1
#endif

#ifndef EC_SECRET_FEISHU_APP_ID
#define EC_SECRET_FEISHU_APP_ID     ""
#endif

#ifndef EC_SECRET_FEISHU_APP_SECRET
#define EC_SECRET_FEISHU_APP_SECRET ""
#endif

#ifndef EC_FEISHU_WS_URL_MAX
#define EC_FEISHU_WS_URL_MAX        256
#endif

#ifndef EC_FEISHU_PING_INTERVAL_S
#define EC_FEISHU_PING_INTERVAL_S   120
#endif

/* ==================== [Typedefs] ========================================== */

/* ==================== [Global Prototypes] ================================= */

/* ==================== [Macros] ============================================ */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // __EC_CONFIG_INTERNAL_H__
