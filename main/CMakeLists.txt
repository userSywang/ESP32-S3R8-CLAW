idf_component_register(
    SRCS
       "main.c"
       "wifi_connect.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        spiffs
        claw
)

set(EC_APP_CONFIG_HEADER "${CMAKE_CURRENT_LIST_DIR}/ec_config.h")
if(EXISTS "${EC_APP_CONFIG_HEADER}")
    idf_component_get_property(claw_lib claw COMPONENT_LIB)
    target_compile_options(${claw_lib} PRIVATE
        "-include"
        "${EC_APP_CONFIG_HEADER}"
    )
endif()
