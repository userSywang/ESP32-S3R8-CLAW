/**
 * @file tools_cron.c
 * @author cangyu (sky.kirto@qq.com)
 * @brief
 * @version 0.1
 * @date 2026-03-05
 *
 * @copyright Copyright (c) 2026, Wireless-Tag. All rights reserved.
 *
 */

/* ==================== [Includes] ========================================== */

#include "core/ec_tools.h"
#include "core/ec_agent.h"
#include "ec_config_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"


/* ==================== [Defines] =========================================== */

/* ==================== [Typedefs] ========================================== */

/* Schedule types */
typedef enum {
    CRON_KIND_EVERY = 0,   /* Recurring interval in seconds */
    CRON_KIND_AT    = 1,   /* One-shot at unix timestamp */
} ec_cron_kind_t;

/* A single cron job */
typedef struct {
    char id[9];            /* 8-char hex ID + null */
    char name[32];
    bool enabled;
    ec_cron_kind_t kind;
    uint32_t interval_s;   /* For EVERY: interval in seconds */
    int64_t at_epoch;      /* For AT: unix timestamp */
    char message[256];     /* Message to inject into inbound queue */
    char channel[16];      /* Reply channel (default "system") */
    char chat_id[64];      /* Reply chat_id (default "cron") */
    int64_t last_run;      /* Last run epoch */
    int64_t next_run;      /* Next run epoch */
    bool delete_after_run; /* Remove job after firing (for AT jobs) */
} ec_cron_job_t;

/* ==================== [Static Prototypes] ================================= */

static esp_err_t ec_cron_service_start(void);
static esp_err_t cron_save_jobs(void);
static esp_err_t cron_load_jobs(void);

static esp_err_t ec_tool_cron_add_execute(const char *input_json, char *output, size_t output_size);
static esp_err_t ec_tool_cron_list_execute(const char *input_json, char *output, size_t output_size);
static esp_err_t ec_tool_cron_remove_execute(const char *input_json, char *output, size_t output_size);

/* ==================== [Static Variables] ================================== */

static const char *TAG = "tools_cron";

static ec_cron_job_t s_jobs[EC_MAX_CRON_JOBS];
static int s_job_count = 0;
static bool s_cron_service_started = false;
static bool s_skip_task_start_for_test = false;
static bool s_skip_persist_for_test = false;
static bool s_jobs_loaded = false;

static const ec_tools_t s_cron_add = {
    .name = "cron_add",
    .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
    .input_schema_json =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
    "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
    "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
    "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
    "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
    "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'feishu'). If omitted, current turn channel is used when available\"},"
    "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='feishu'. If omitted during a feishu turn, current chat_id is used\"}"
    "},"
    "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
    .execute = ec_tool_cron_add_execute,
};

static const ec_tools_t s_cron_list = {
    .name = "cron_list",
    .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
    .input_schema_json =
    "{\"type\":\"object\","
    "\"properties\":{},"
    "\"required\":[]}",
    .execute = ec_tool_cron_list_execute,
};

static const ec_tools_t s_cron_remove = {
    .name = "cron_remove",
    .description = "Remove a scheduled cron job by its ID.",
    .input_schema_json =
    "{\"type\":\"object\","
    "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
    "\"required\":[\"job_id\"]}",
    .execute = ec_tool_cron_remove_execute,
};

/* ==================== [Macros] ============================================ */

/* ==================== [Global Functions] ================================== */

esp_err_t ec_tools_cron_add(void)
{
    ec_cron_service_start();
    ec_tools_register(&s_cron_add);
    return ESP_OK;
}

esp_err_t ec_tools_cron_list(void)
{
    ec_tools_register(&s_cron_list);
    return ESP_OK;
}

esp_err_t ec_tools_cron_remove(void)
{
    ec_tools_register(&s_cron_remove);
    return ESP_OK; 
}
/* ==================== [Static Functions] ================================== */

static esp_err_t ec_cron_remove_job(const char *job_id)
{
    for (int i = 0; i < s_job_count; i++) {
        if (strcmp(s_jobs[i].id, job_id) == 0) {
            ESP_LOGI(TAG, "Removing cron job: %s (%s)", s_jobs[i].name, job_id);

            /* Shift remaining jobs down */
            for (int j = i; j < s_job_count - 1; j++) {
                s_jobs[j] = s_jobs[j + 1];
            }
            s_job_count--;

            cron_save_jobs();
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Cron job not found: %s", job_id);
    return ESP_ERR_NOT_FOUND;
}

static void ec_cron_list_jobs(const ec_cron_job_t **jobs, int *count)
{
    *jobs = s_jobs;
    *count = s_job_count;
}

static void compute_initial_next_run(ec_cron_job_t *job)
{
    time_t now = time(NULL);

    if (job->kind == CRON_KIND_EVERY) {
        job->next_run = now + job->interval_s;
    } else if (job->kind == CRON_KIND_AT) {
        if (job->at_epoch > now) {
            job->next_run = job->at_epoch;
        } else {
            /* Already in the past */
            job->next_run = 0;
            job->enabled = false;
        }
    }
}

static ec_cron_job_t *cron_find_matching_job(const ec_cron_job_t *candidate)
{
    if (!candidate) {
        return NULL;
    }

    for (int i = 0; i < s_job_count; i++) {
        ec_cron_job_t *job = &s_jobs[i];
        if (job->kind != candidate->kind) {
            continue;
        }
        if (strcmp(job->name, candidate->name) != 0 ||
            strcmp(job->message, candidate->message) != 0 ||
            strcmp(job->channel, candidate->channel) != 0 ||
            strcmp(job->chat_id, candidate->chat_id) != 0 ||
            job->delete_after_run != candidate->delete_after_run) {
            continue;
        }

        if (job->kind == CRON_KIND_EVERY && job->interval_s == candidate->interval_s) {
            return job;
        }

        if (job->kind == CRON_KIND_AT && job->at_epoch == candidate->at_epoch) {
            return job;
        }
    }

    return NULL;
}

static char *cron_build_dispatch_message(const ec_cron_job_t *job)
{
    static const char *prefix =
        "Scheduled reminder: execute the following instruction now and reply directly. "
        "Do not mention scheduling, setup, or confirmation.\n\nInstruction: ";
    size_t len = 0;
    char *msg = NULL;

    if (!job || !job->message[0]) {
        return NULL;
    }

    len = strlen(prefix) + strlen(job->message) + 1;
    msg = (char *)malloc(len);
    if (!msg) {
        return NULL;
    }

    snprintf(msg, len, "%s%s", prefix, job->message);
    return msg;
}

static bool cron_sanitize_destination(ec_cron_job_t *job)
{
    bool changed = false;
    if (!job) {
        return false;
    }

    if (job->channel[0] == '\0') {
        strncpy(job->channel, EC_CHAN_SYSTEM, sizeof(job->channel) - 1);
        changed = true;
    }

    if (strcmp(job->channel, EC_CHAN_FEISHU) == 0) {
        if (job->chat_id[0] == '\0' || strcmp(job->chat_id, "cron") == 0) {
            ESP_LOGW(TAG, "Cron job %s has invalid feishu chat_id, fallback to system:cron",
                     job->id[0] ? job->id : "<new>");
            strncpy(job->channel, EC_CHAN_SYSTEM, sizeof(job->channel) - 1);
            strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
            changed = true;
        }
    } else if (job->chat_id[0] == '\0') {
        strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
        changed = true;
    }

    return changed;
}

static void cron_generate_id(char *id_buf)
{
    uint32_t r = esp_random();
    snprintf(id_buf, 9, "%08x", (unsigned int)r);
}

static esp_err_t cron_save_jobs(void)
{
    if (s_skip_persist_for_test) {
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *jobs_arr = cJSON_CreateArray();

    for (int i = 0; i < s_job_count; i++) {
        ec_cron_job_t *job = &s_jobs[i];
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "id", job->id);
        cJSON_AddStringToObject(item, "name", job->name);
        cJSON_AddBoolToObject(item, "enabled", job->enabled);
        cJSON_AddStringToObject(item, "kind",
                                job->kind == CRON_KIND_EVERY ? "every" : "at");

        if (job->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", job->interval_s);
        } else {
            cJSON_AddNumberToObject(item, "at_epoch", (double)job->at_epoch);
        }

        cJSON_AddStringToObject(item, "message", job->message);
        cJSON_AddStringToObject(item, "channel", job->channel);
        cJSON_AddStringToObject(item, "chat_id", job->chat_id);
        cJSON_AddNumberToObject(item, "last_run", (double)job->last_run);
        cJSON_AddNumberToObject(item, "next_run", (double)job->next_run);
        cJSON_AddBoolToObject(item, "delete_after_run", job->delete_after_run);

        cJSON_AddItemToArray(jobs_arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", jobs_arr);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to serialize cron jobs");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(EC_CRON_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", EC_CRON_FILE);
        free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Cron save incomplete: %d/%d bytes", (int)written, (int)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %d cron jobs to %s", s_job_count, EC_CRON_FILE);
    return ESP_OK;
}

static esp_err_t cron_load_jobs(void)
{
    FILE *f = NULL;
    long len = 0;
    char *json = NULL;
    cJSON *root = NULL;
    cJSON *jobs_arr = NULL;
    int loaded = 0;
    time_t now = time(NULL);

    if (s_jobs_loaded) {
        return ESP_OK;
    }

    f = fopen(EC_CRON_FILE, "r");
    if (!f) {
        s_jobs_loaded = true;
        ESP_LOGI(TAG, "No cron file found at %s", EC_CRON_FILE);
        return ESP_OK;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    len = ftell(f);
    if (len <= 0) {
        fclose(f);
        s_jobs_loaded = true;
        return ESP_OK;
    }
    rewind(f);

    json = (char *)calloc(1, (size_t)len + 1);
    if (!json) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (fread(json, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(json);
        return ESP_FAIL;
    }
    fclose(f);

    root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse %s", EC_CRON_FILE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    jobs_arr = cJSON_GetObjectItem(root, "jobs");
    if (!cJSON_IsArray(jobs_arr)) {
        cJSON_Delete(root);
        s_jobs_loaded = true;
        return ESP_OK;
    }

    memset(s_jobs, 0, sizeof(s_jobs));
    s_job_count = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, jobs_arr) {
        ec_cron_job_t job = {0};
        cJSON *kind = cJSON_GetObjectItem(item, "kind");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *message = cJSON_GetObjectItem(item, "message");
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        cJSON *delete_after_run = cJSON_GetObjectItem(item, "delete_after_run");
        cJSON *interval_s = cJSON_GetObjectItem(item, "interval_s");
        cJSON *at_epoch = cJSON_GetObjectItem(item, "at_epoch");
        cJSON *last_run = cJSON_GetObjectItem(item, "last_run");
        cJSON *next_run = cJSON_GetObjectItem(item, "next_run");
        cJSON *channel = cJSON_GetObjectItem(item, "channel");
        cJSON *chat_id = cJSON_GetObjectItem(item, "chat_id");
        cJSON *id = cJSON_GetObjectItem(item, "id");

        if (s_job_count >= EC_MAX_CRON_JOBS) {
            break;
        }
        if (!cJSON_IsString(kind) || !cJSON_IsString(name) || !cJSON_IsString(message)) {
            continue;
        }

        if (strcmp(kind->valuestring, "every") == 0) {
            if (!cJSON_IsNumber(interval_s) || interval_s->valuedouble <= 0) {
                continue;
            }
            job.kind = CRON_KIND_EVERY;
            job.interval_s = (uint32_t)interval_s->valuedouble;
        } else if (strcmp(kind->valuestring, "at") == 0) {
            if (!cJSON_IsNumber(at_epoch)) {
                continue;
            }
            job.kind = CRON_KIND_AT;
            job.at_epoch = (int64_t)at_epoch->valuedouble;
        } else {
            continue;
        }

        if (cJSON_IsString(id)) {
            strncpy(job.id, id->valuestring, sizeof(job.id) - 1);
        }
        strncpy(job.name, name->valuestring, sizeof(job.name) - 1);
        strncpy(job.message, message->valuestring, sizeof(job.message) - 1);
        if (cJSON_IsString(channel)) {
            strncpy(job.channel, channel->valuestring, sizeof(job.channel) - 1);
        }
        if (cJSON_IsString(chat_id)) {
            strncpy(job.chat_id, chat_id->valuestring, sizeof(job.chat_id) - 1);
        }
        job.enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
        job.delete_after_run = cJSON_IsBool(delete_after_run) ? cJSON_IsTrue(delete_after_run) : (job.kind == CRON_KIND_AT);
        job.last_run = cJSON_IsNumber(last_run) ? (int64_t)last_run->valuedouble : 0;
        job.next_run = cJSON_IsNumber(next_run) ? (int64_t)next_run->valuedouble : 0;

        cron_sanitize_destination(&job);

        if (job.kind == CRON_KIND_AT && job.at_epoch <= now) {
            if (job.delete_after_run) {
                ESP_LOGI(TAG, "Skip expired one-shot cron job during restore: %s (%s)", job.name, job.id);
                continue;
            }
            job.enabled = false;
            job.next_run = 0;
        } else if (job.enabled && job.next_run <= 0) {
            compute_initial_next_run(&job);
        }

        s_jobs[s_job_count++] = job;
        loaded++;
    }

    cJSON_Delete(root);
    s_jobs_loaded = true;
    ESP_LOGI(TAG, "Loaded %d cron jobs from %s", loaded, EC_CRON_FILE);
    return ESP_OK;
}

static void cron_process_due_jobs(void)
{
    time_t now = time(NULL);

    bool changed = false;

    for (int i = 0; i < s_job_count; i++) {
        ec_cron_job_t *job = &s_jobs[i];
        if (!job->enabled) {
            continue;
        }
        if (job->next_run <= 0) {
            continue;
        }
        if (job->next_run > now) {
            continue;
        }

        /* Job is due — fire it */
        ESP_LOGI(TAG, "Cron job firing: %s (%s)", job->name, job->id);

        /* Push message to inbound queue */
        ec_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, job->channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, job->chat_id, sizeof(msg.chat_id) - 1);
        ESP_LOGI(TAG, "Cron dispatch -> %s:%s msg=%s", job->channel, job->chat_id, job->message);
        msg.content = cron_build_dispatch_message(job);

        if (msg.content) {
            esp_err_t err = ec_agent_inbound(&msg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to push cron message: %s", esp_err_to_name(err));
                free(msg.content);
            }
        } else {
            ESP_LOGW(TAG, "Failed to build cron dispatch message");
        }

        /* Update state */
        job->last_run = now;

        if (job->kind == CRON_KIND_AT) {
            /* One-shot: disable or delete */
            if (job->delete_after_run) {
                /* Remove by shifting array */
                ESP_LOGI(TAG, "Deleting one-shot job: %s", job->name);
                for (int j = i; j < s_job_count - 1; j++) {
                    s_jobs[j] = s_jobs[j + 1];
                }
                s_job_count--;
                i--; /* Re-check this index */
            } else {
                job->enabled = false;
                job->next_run = 0;
            }
        } else {
            /* Recurring: compute next run */
            job->next_run = now + job->interval_s;
        }

        changed = true;
    }

    if (changed) {
        cron_save_jobs();
    }
}

static void cron_task_main(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EC_CRON_CHECK_INTERVAL_MS));
        cron_process_due_jobs();
    }
}

static esp_err_t ec_cron_service_start(void)
{
    if (s_skip_task_start_for_test) {
        s_cron_service_started = true;
        return ESP_OK;
    }

    if (s_cron_service_started) {
        return ESP_OK;
    }

    cron_load_jobs();

    /* Recompute next_run for all enabled jobs that don't have one */
    time_t now = time(NULL);
    for (int i = 0; i < s_job_count; i++) {
        ec_cron_job_t *job = &s_jobs[i];
        if (job->enabled && job->next_run <= 0) {
            if (job->kind == CRON_KIND_EVERY) {
                job->next_run = now + job->interval_s;
            } else if (job->kind == CRON_KIND_AT && job->at_epoch > now) {
                job->next_run = job->at_epoch;
            }
        }
    }

    BaseType_t ok = xTaskCreate(
                        cron_task_main,
                        "cron",
                        EC_CRON_TASK_STACK_SIZE,
                        NULL,
                        EC_CRON_TASK_PRIORITY,
                        NULL
                    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cron task");
        return ESP_FAIL;
    }

    s_cron_service_started = true;
    ESP_LOGI(TAG, "Cron service started (%d jobs, check every %ds)",
             s_job_count, EC_CRON_CHECK_INTERVAL_MS / 1000);
    return ESP_OK;
}

static esp_err_t ec_cron_add_job(ec_cron_job_t *job)
{
    ec_cron_job_t *existing = NULL;

    if (s_job_count >= EC_MAX_CRON_JOBS) {
        ESP_LOGW(TAG, "Max cron jobs reached (%d)", EC_MAX_CRON_JOBS);
        return ESP_ERR_NO_MEM;
    }

    /* Validate/sanitize channel and chat_id before storing. */
    cron_sanitize_destination(job);

    /* Compute initial next_run */
    job->enabled = true;
    job->last_run = 0;
    compute_initial_next_run(job);

    existing = cron_find_matching_job(job);
    if (existing) {
        strncpy(job->id, existing->id, sizeof(job->id) - 1);
        job->last_run = existing->last_run;
        job->next_run = existing->next_run;
        job->enabled = existing->enabled;
        ESP_LOGI(TAG, "Duplicate cron job ignored: %s (%s)", existing->name, existing->id);
        return ESP_OK;
    }

    /* Generate ID */
    cron_generate_id(job->id);

    /* Copy into static array */
    s_jobs[s_job_count] = *job;
    s_job_count++;

    cron_save_jobs();

    ESP_LOGI(TAG, "Added cron job: %s (%s) kind=%s next_run=%lld",
             job->name, job->id,
             job->kind == CRON_KIND_EVERY ? "every" : "at",
             (long long)job->next_run);
    return ESP_OK;
}

static esp_err_t ec_tool_cron_add_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    const char *schedule_type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "schedule_type"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));

    if (!name || !schedule_type || !message) {
        snprintf(output, output_size, "Error: missing required fields (name, schedule_type, message)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(message) == 0) {
        snprintf(output, output_size, "Error: message must not be empty");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    ec_cron_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.name, name, sizeof(job.name) - 1);
    strncpy(job.message, message, sizeof(job.message) - 1);

    /* Optional channel and chat_id */
    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(root, "channel"));
    const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id"));

    if (channel) strncpy(job.channel, channel, sizeof(job.channel) - 1);
    if (chat_id) strncpy(job.chat_id, chat_id, sizeof(job.chat_id) - 1);

    if (strcmp(job.channel, EC_CHAN_FEISHU) == 0 &&
        (job.chat_id[0] == '\0' || strcmp(job.chat_id, "cron") == 0)) {
        snprintf(output, output_size,
                 "Error: cron_add with channel='feishu' requires a valid chat_id like open_id:... or chat_id:...");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(schedule_type, "every") == 0) {
        job.kind = CRON_KIND_EVERY;
        cJSON *interval = cJSON_GetObjectItem(root, "interval_s");
        if (!interval || !cJSON_IsNumber(interval) || interval->valuedouble <= 0) {
            snprintf(output, output_size, "Error: 'every' schedule requires positive 'interval_s'");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        job.interval_s = (uint32_t)interval->valuedouble;
        job.delete_after_run = false;
    } else if (strcmp(schedule_type, "at") == 0) {
        job.kind = CRON_KIND_AT;
        cJSON *at_epoch = cJSON_GetObjectItem(root, "at_epoch");
        if (!at_epoch || !cJSON_IsNumber(at_epoch)) {
            snprintf(output, output_size, "Error: 'at' schedule requires 'at_epoch' (unix timestamp)");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        job.at_epoch = (int64_t)at_epoch->valuedouble;

        /* Check if already in the past */
        time_t now = time(NULL);
        if (job.at_epoch <= now) {
            snprintf(output, output_size, "Error: at_epoch %lld is in the past (now=%lld)",
                     (long long)job.at_epoch, (long long)now);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        /* Default: delete one-shot jobs after run */
        cJSON *delete_j = cJSON_GetObjectItem(root, "delete_after_run");
        job.delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : true;
    } else {
        snprintf(output, output_size, "Error: schedule_type must be 'every' or 'at'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);

    esp_err_t err = ec_cron_add_job(&job);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to add job (%s)", esp_err_to_name(err));
        return err;
    }

    /* Format success response */
    if (job.kind == CRON_KIND_EVERY) {
        snprintf(output, output_size,
                 "OK: Added recurring job '%s' (id=%s), runs every %lu seconds. Next run at epoch %lld.",
                 job.name, job.id, (unsigned long)job.interval_s, (long long)job.next_run);
    } else {
        snprintf(output, output_size,
                 "OK: Added one-shot job '%s' (id=%s), fires at epoch %lld.%s",
                 job.name, job.id, (long long)job.at_epoch,
                 job.delete_after_run ? " Will be deleted after firing." : "");
    }

    ESP_LOGI(TAG, "cron_add: %s", output);
    return ESP_OK;
}

static esp_err_t ec_tool_cron_list_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    const ec_cron_job_t *jobs;
    int count;
    ec_cron_list_jobs(&jobs, &count);

    if (count == 0) {
        snprintf(output, output_size, "No cron jobs scheduled.");
        return ESP_OK;
    }

    size_t off = 0;
    off += snprintf(output + off, output_size - off,
                    "Scheduled jobs (%d):\n", count);

    for (int i = 0; i < count && off < output_size - 1; i++) {
        const ec_cron_job_t *j = &jobs[i];

        if (j->kind == CRON_KIND_EVERY) {
            off += snprintf(output + off, output_size - off,
                "  %d. [%s] \"%s\" — every %lus, %s, next=%lld, last=%lld, ch=%s:%s\n",
                i + 1, j->id, j->name,
                (unsigned long)j->interval_s,
                j->enabled ? "enabled" : "disabled",
                (long long)j->next_run, (long long)j->last_run,
                j->channel, j->chat_id);
        } else {
            off += snprintf(output + off, output_size - off,
                "  %d. [%s] \"%s\" — at %lld, %s, last=%lld, ch=%s:%s%s\n",
                i + 1, j->id, j->name,
                (long long)j->at_epoch,
                j->enabled ? "enabled" : "disabled",
                (long long)j->last_run,
                j->channel, j->chat_id,
                j->delete_after_run ? " (auto-delete)" : "");
        }
    }

    ESP_LOGI(TAG, "cron_list: %d jobs", count);
    return ESP_OK;
}

static esp_err_t ec_tool_cron_remove_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *job_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "job_id"));
    if (!job_id || strlen(job_id) == 0) {
        snprintf(output, output_size, "Error: missing 'job_id' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char job_id_copy[16] = {0};
    strncpy(job_id_copy, job_id, sizeof(job_id_copy) - 1);

    esp_err_t err = ec_cron_remove_job(job_id_copy);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: Removed cron job %s", job_id_copy);
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: job '%s' not found", job_id_copy);
    } else {
        snprintf(output, output_size, "Error: failed to remove job (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "cron_remove: %s -> %s", job_id_copy, esp_err_to_name(err));
    return err;
}

void ec_tools_cron_configure_for_test(bool skip_task_start, bool skip_persist)
{
    s_skip_task_start_for_test = skip_task_start;
    s_skip_persist_for_test = skip_persist;
}

void ec_tools_cron_reset_for_test(void)
{
    memset(s_jobs, 0, sizeof(s_jobs));
    s_job_count = 0;
    s_cron_service_started = false;
    s_jobs_loaded = false;
    s_skip_task_start_for_test = false;
    s_skip_persist_for_test = false;
}
