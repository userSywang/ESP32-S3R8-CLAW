#include <string.h>
#include <time.h>

#include "unity.h"

#include "support/ec_test_hooks.h"

TEST_CASE("get time formatter rejects unset epoch", "[embed_claw][tools][get_time]")
{
    char output[64] = {0};

    TEST_ASSERT_FALSE(ec_tools_get_time_format_epoch_for_test(0, output, sizeof(output)));
}

TEST_CASE("get time formatter accepts valid epoch", "[embed_claw][tools][get_time]")
{
    char output[64] = {0};

    TEST_ASSERT_TRUE(ec_tools_get_time_format_epoch_for_test((time_t)1760000000, output, sizeof(output)));
    TEST_ASSERT_NOT_EQUAL('\0', output[0]);
    TEST_ASSERT_NOT_NULL(strstr(output, "2025"));
}
