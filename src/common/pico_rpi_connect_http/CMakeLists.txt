# pico_rpi_connect_http holds downstream patches to the lwIP HTTP client
# (http_client.c / http_client.h). It is kept as a standalone library so the
# changes can eventually be upstreamed to lwIP and this library removed.
if (NOT TARGET pico_rpi_connect_http)
    add_library(pico_rpi_connect_http INTERFACE)
    target_include_directories(pico_rpi_connect_http INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_sources(pico_rpi_connect_http INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/http_client.c
    )
    target_link_libraries(pico_rpi_connect_http INTERFACE
        pico_mbedtls
        pico_stdlib
    )
endif()
