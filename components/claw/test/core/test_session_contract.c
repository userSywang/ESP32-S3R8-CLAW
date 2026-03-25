#include <string.h>

#include "unity.h"
#include "cJSON.h"

#include "support/ec_test_hooks.h"
#include "core/ec_session.h"

TEST_CASE("session history validates output buffer", "[embed_claw][core][session]")
{
    char history[32];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ec_session_get_history_json("unit_session", NULL, sizeof(history), 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ec_session_get_history_json("unit_session", history, 0, 1));
}

TEST_CASE("missing session history returns empty array", "[embed_claw][core][session]")
{
    char history[32];

    (void) ec_session_clear("unit_missing_session");
    TEST_ASSERT_EQUAL(ESP_OK,
                      ec_session_get_history_json("unit_missing_session", history, sizeof(history), 0));
    TEST_ASSERT_EQUAL_STRING("[]", history);
}

TEST_CASE("session append history and clear lifecycle", "[embed_claw][core][session]")
{
    const char *chat_id = "unit_session_lifecycle";
    char history[256];
    cJSON *arr;

    TEST_ASSERT_EQUAL(ESP_OK, ec_test_spiffs_mount());
    (void)ec_session_clear(chat_id);

    TEST_ASSERT_EQUAL(ESP_OK, ec_session_append(chat_id, "user", "hello"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_append(chat_id, "assistant", "hi there"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_get_history_json(chat_id, history, sizeof(history), 10));

    arr = cJSON_Parse(history);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(arr));
    TEST_ASSERT_EQUAL_STRING("user",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "role")));
    TEST_ASSERT_EQUAL_STRING("hello",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content")));
    TEST_ASSERT_EQUAL_STRING("assistant",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "role")));
    TEST_ASSERT_EQUAL_STRING("hi there",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "content")));
    cJSON_Delete(arr);

    TEST_ASSERT_EQUAL(ESP_OK, ec_session_clear(chat_id));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_get_history_json(chat_id, history, sizeof(history), 10));
    TEST_ASSERT_EQUAL_STRING("[]", history);

    ec_test_spiffs_unmount();
}

TEST_CASE("session history keeps only latest messages within limit", "[embed_claw][core][session]")
{
    const char *chat_id = "unit_session_limit";
    char history[256];
    cJSON *arr;

    TEST_ASSERT_EQUAL(ESP_OK, ec_test_spiffs_mount());
    (void)ec_session_clear(chat_id);

    TEST_ASSERT_EQUAL(ESP_OK, ec_session_append(chat_id, "user", "one"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_append(chat_id, "assistant", "two"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_append(chat_id, "user", "three"));
    TEST_ASSERT_EQUAL(ESP_OK, ec_session_get_history_json(chat_id, history, sizeof(history), 2));

    arr = cJSON_Parse(history);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(arr));
    TEST_ASSERT_EQUAL_STRING("two",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content")));
    TEST_ASSERT_EQUAL_STRING("three",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "content")));
    cJSON_Delete(arr);

    TEST_ASSERT_EQUAL(ESP_OK, ec_session_clear(chat_id));
    ec_test_spiffs_unmount();
}
