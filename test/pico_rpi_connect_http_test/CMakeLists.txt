if (TARGET pico_cyw43_arch_lwip_threadsafe_background AND TARGET pico_rpi_connect_http)
    # Defaults match the access point and HTTP server scripts in host/.
    if ("${WIFI_SSID}" STREQUAL "")
        set(WIFI_SSID "pico-http-test")
    endif()
    if ("${WIFI_PASSWORD}" STREQUAL "")
        set(WIFI_PASSWORD "pico-http-test-pw")
    endif()

    add_executable(pico_rpi_connect_http_test pico_rpi_connect_http_test.c)
    # Requires an access point and HTTP server (see host/).
    set_target_properties(pico_rpi_connect_http_test PROPERTIES
            PICO_TEST_TIMEOUT "300"
            PICO_TEST_SKIP_IN_CI TRUE
    )
    target_compile_definitions(pico_rpi_connect_http_test PRIVATE
            WIFI_SSID=\"${WIFI_SSID}\"
            WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    )
    # lwipopts.h and mbedtls_config.h (pulled in via pico_rpi_connect_http's
    # pico_mbedtls dependency; TLS itself is not used).
    target_include_directories(pico_rpi_connect_http_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(pico_rpi_connect_http_test PRIVATE
            pico_cyw43_arch_lwip_threadsafe_background
            pico_rpi_connect_http
            pico_stdlib
            pico_test
    )
    pico_add_extra_outputs(pico_rpi_connect_http_test)
endif()
