#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "core/ec_skill_loader.h"
#include "support/ec_test_hooks.h"

static void remove_if_exists(const char *path)
{
    (void)remove(path);
}

static void write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");

    TEST_ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

TEST_CASE("skill loader installs builtins and summarizes skills", "[embed_claw][core][skills]")
{
    char summary[2048];
    FILE *f;

    TEST_ASSERT_EQUAL(ESP_OK, ec_test_spiffs_mount());

    remove_if_exists("/spiffs/skills/weather.md");
    remove_if_exists("/spiffs/skills/daily-briefing.md");
    remove_if_exists("/spiffs/skills/skill-creator.md");
    remove_if_exists("/spiffs/skills/unit-test.md");

    TEST_ASSERT_EQUAL(ESP_OK, ec_skill_loader_init());

    f = fopen("/spiffs/skills/weather.md", "r");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    f = fopen("/spiffs/skills/daily-briefing.md", "r");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    f = fopen("/spiffs/skills/skill-creator.md", "r");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    write_text_file("/spiffs/skills/unit-test.md",
                    "# Unit Test Skill\n\nUsed for verifying summary output.\n");

    TEST_ASSERT_GREATER_THAN(0, (int)ec_skill_loader_build_summary(summary, sizeof(summary)));
    TEST_ASSERT_NOT_NULL(strstr(summary, "**Weather**"));
    TEST_ASSERT_NOT_NULL(strstr(summary, "**Daily Briefing**"));
    TEST_ASSERT_NOT_NULL(strstr(summary, "**Skill Creator**"));
    TEST_ASSERT_NOT_NULL(strstr(summary, "**Unit Test Skill**"));
    TEST_ASSERT_NOT_NULL(strstr(summary, "Used for verifying summary output."));

    remove_if_exists("/spiffs/skills/unit-test.md");
    ec_test_spiffs_unmount();
}
