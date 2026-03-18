idf_component_register(
    SRC_DIRS
        "."
        "channel"
        "core"
        "llm"
        "support"
        "tools"
    PRIV_INCLUDE_DIRS
        "."
        "support"
    PRIV_REQUIRES
        unity
        test_utils
        embed_claw
)

set(EC_TEST_CONFIG_HEADER "${CMAKE_CURRENT_LIST_DIR}/ec_config.h")
if(EXISTS "${EC_TEST_CONFIG_HEADER}")
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "-include"
        "${EC_TEST_CONFIG_HEADER}"
    )

    idf_component_get_property(embed_claw_lib embed_claw COMPONENT_LIB)
    target_compile_options(${embed_claw_lib} PRIVATE
        "-include"
        "${EC_TEST_CONFIG_HEADER}"
    )
endif()
