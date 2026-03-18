#include <string.h>

#include "unity.h"
#include "cJSON.h"

#include "support/ec_test_hooks.h"

TEST_CASE("openai provider converts tool schemas to function calls", "[embed_claw][llm][openai]")
{
    cJSON *out = ec_llm_openai_convert_tools_for_test(
        "[{"
        "\"name\":\"web_search\","
        "\"description\":\"search the web\","
        "\"input_schema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}"
        "}]");
    cJSON *item;
    cJSON *function;

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(cJSON_IsArray(out));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(out));

    item = cJSON_GetArrayItem(out, 0);
    TEST_ASSERT_EQUAL_STRING("function", cJSON_GetStringValue(cJSON_GetObjectItem(item, "type")));
    function = cJSON_GetObjectItem(item, "function");
    TEST_ASSERT_EQUAL_STRING("web_search", cJSON_GetStringValue(cJSON_GetObjectItem(function, "name")));
    TEST_ASSERT_EQUAL_STRING("search the web", cJSON_GetStringValue(cJSON_GetObjectItem(function, "description")));
    TEST_ASSERT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(function, "parameters")));

    cJSON_Delete(out);
}

TEST_CASE("openai provider converts assistant and tool messages", "[embed_claw][llm][openai]")
{
    cJSON *messages = cJSON_Parse(
        "["
        "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]},"
        "{\"role\":\"assistant\",\"content\":["
        "  {\"type\":\"text\",\"text\":\"checking\"},"
        "  {\"type\":\"tool_use\",\"id\":\"call_1\",\"index\":2,\"name\":\"web_search\",\"input\":{\"query\":\"today news\"}}"
        "]},"
        "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call_1\",\"content\":\"1. done\"}]}"
        "]");
    cJSON *out = ec_llm_openai_convert_messages_for_test("system prompt", messages);
    cJSON *assistant;
    cJSON *tool_calls;
    cJSON *tool_msg;

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(cJSON_IsArray(out));
    TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(out));

    TEST_ASSERT_EQUAL_STRING("system",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(out, 0), "role")));
    TEST_ASSERT_EQUAL_STRING("hello",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(out, 1), "content")));

    assistant = cJSON_GetArrayItem(out, 2);
    TEST_ASSERT_EQUAL_STRING("assistant", cJSON_GetStringValue(cJSON_GetObjectItem(assistant, "role")));
    TEST_ASSERT_EQUAL_STRING("checking", cJSON_GetStringValue(cJSON_GetObjectItem(assistant, "content")));
    tool_calls = cJSON_GetObjectItem(assistant, "tool_calls");
    TEST_ASSERT_TRUE(cJSON_IsArray(tool_calls));
    TEST_ASSERT_EQUAL(2, cJSON_GetObjectItem(cJSON_GetArrayItem(tool_calls, 0), "index")->valueint);
    TEST_ASSERT_NOT_NULL(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(
                                  cJSON_GetObjectItem(cJSON_GetArrayItem(tool_calls, 0), "function"),
                                  "arguments")),
                              "today news"));

    tool_msg = cJSON_GetArrayItem(out, 3);
    TEST_ASSERT_EQUAL_STRING("tool", cJSON_GetStringValue(cJSON_GetObjectItem(tool_msg, "role")));
    TEST_ASSERT_EQUAL_STRING("call_1", cJSON_GetStringValue(cJSON_GetObjectItem(tool_msg, "tool_call_id")));
    TEST_ASSERT_EQUAL_STRING("1. done", cJSON_GetStringValue(cJSON_GetObjectItem(tool_msg, "content")));

    cJSON_Delete(out);
    cJSON_Delete(messages);
}

TEST_CASE("openai provider selects pinned CA only for aliyuncs endpoints", "[embed_claw][llm][openai]")
{
    TEST_ASSERT_NOT_NULL(ec_llm_openai_select_server_ca_pem_for_test(
        "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions"));
    TEST_ASSERT_NULL(ec_llm_openai_select_server_ca_pem_for_test("https://api.openai.com/v1/chat/completions"));
}
