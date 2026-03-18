#include <stdio.h>
#include <string.h>
#include <time.h>

#include "unity.h"

#include "ec_config_internal.h"
#include "core/ec_memory.h"
#include "support/ec_test_hooks.h"

static void remove_if_exists(const char *path)
{
    (void)remove(path);
}

static void daily_note_path(char *buf, size_t size, int days_ago)
{
    time_t now = time(NULL) - (time_t)(days_ago * 86400);
    struct tm local = {0};
    char date_str[16];

    localtime_r(&now, &local);
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &local);
    snprintf(buf, size, "%s/%s.md", EC_FS_MEMORY_DIR, date_str);
}

static void write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");

    TEST_ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

TEST_CASE("memory long term read write lifecycle", "[embed_claw][core][memory]")
{
    char output[128];

    TEST_ASSERT_EQUAL(ESP_OK, ec_test_spiffs_mount());
    remove_if_exists(EC_MEMORY_FILE);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ec_memory_read_long_term(output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("", output);

    TEST_ASSERT_EQUAL(ESP_OK, ec_memory_write_long_term("name: codex"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_memory_read_long_term(output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("name: codex", output);

    remove_if_exists(EC_MEMORY_FILE);
    ec_test_spiffs_unmount();
}

TEST_CASE("memory appends today and reads recent notes", "[embed_claw][core][memory]")
{
    char today_path[64];
    char yesterday_path[64];
    char recent[256];

    TEST_ASSERT_EQUAL(ESP_OK, ec_test_spiffs_mount());

    daily_note_path(today_path, sizeof(today_path), 0);
    daily_note_path(yesterday_path, sizeof(yesterday_path), 1);
    remove_if_exists(today_path);
    remove_if_exists(yesterday_path);

    write_text_file(yesterday_path, "# yesterday\n\nyesterday note\n");
    TEST_ASSERT_EQUAL(ESP_OK, ec_memory_append_today("today note"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_memory_read_recent(recent, sizeof(recent), 2));
    TEST_ASSERT_NOT_NULL(strstr(recent, "today note"));
    TEST_ASSERT_NOT_NULL(strstr(recent, "yesterday note"));
    TEST_ASSERT_NOT_NULL(strstr(recent, "---"));

    remove_if_exists(today_path);
    remove_if_exists(yesterday_path);
    ec_test_spiffs_unmount();
}
