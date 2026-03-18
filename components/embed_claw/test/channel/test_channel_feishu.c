#include <string.h>

#include "unity.h"

#include "support/ec_test_hooks.h"

TEST_CASE("feishu channel parses chat id prefixes", "[embed_claw][channel][feishu]")
{
    char type[16];
    char id[64];

    ec_channel_feishu_parse_chat_id_for_test("open_id:ou_123", type, sizeof(type), id, sizeof(id));
    TEST_ASSERT_EQUAL_STRING("open_id", type);
    TEST_ASSERT_EQUAL_STRING("ou_123", id);

    ec_channel_feishu_parse_chat_id_for_test("chat_id:oc_456", type, sizeof(type), id, sizeof(id));
    TEST_ASSERT_EQUAL_STRING("chat_id", type);
    TEST_ASSERT_EQUAL_STRING("oc_456", id);

    ec_channel_feishu_parse_chat_id_for_test("invalid", type, sizeof(type), id, sizeof(id));
    TEST_ASSERT_EQUAL_STRING("", type);
    TEST_ASSERT_EQUAL_STRING("", id);
}

TEST_CASE("feishu channel ping frames round-trip through parser", "[embed_claw][channel][feishu]")
{
    uint8_t buf[128];
    char type[32] = {0};
    char payload[64] = {0};
    int32_t method = -1;
    int32_t service = 0;
    uint64_t seq_id = 0;
    uint64_t log_id = 0;
    int n;

    n = ec_channel_feishu_encode_ping_for_test(buf, sizeof(buf), 321);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(ec_channel_feishu_parse_frame_for_test(buf, (size_t)n, &method, &service,
                                                            &seq_id, &log_id,
                                                            type, sizeof(type),
                                                            payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(0, method);
    TEST_ASSERT_EQUAL(321, service);
    TEST_ASSERT_EQUAL_STRING("ping", type);
}

TEST_CASE("feishu channel response frames preserve ids and payload", "[embed_claw][channel][feishu]")
{
    uint8_t buf[256];
    char payload[64] = {0};
    int32_t method = -1;
    int32_t service = 0;
    uint64_t seq_id = 0;
    uint64_t log_id = 0;
    int n;

    n = ec_channel_feishu_encode_response_for_test(buf, sizeof(buf), 11, 22, 33);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_TRUE(ec_channel_feishu_parse_frame_for_test(buf, (size_t)n, &method, &service,
                                                            &seq_id, &log_id,
                                                            NULL, 0,
                                                            payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(1, method);
    TEST_ASSERT_EQUAL(33, service);
    TEST_ASSERT_EQUAL(11, (unsigned int)seq_id);
    TEST_ASSERT_EQUAL(22, (unsigned int)log_id);
    TEST_ASSERT_EQUAL_STRING("{\"code\":200}", payload);
}
