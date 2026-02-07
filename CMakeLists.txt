cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(floppy C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Floppy driver library
add_library(floppy_lib INTERFACE)

target_sources(floppy_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/crc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/floppy.c
    ${CMAKE_CURRENT_LIST_DIR}/src/f12.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fat12.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lru.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mfm_decode.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mfm_encode.c
)

target_include_directories(floppy_lib INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(floppy_lib INTERFACE
    pico_stdlib
    hardware_gpio
    hardware_pio
    hardware_clocks
)

function(add_floppy_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} floppy_lib)
    pico_generate_pio_header(${name}
        ${CMAKE_CURRENT_LIST_DIR}/src/flux_read.pio
        ${CMAKE_CURRENT_LIST_DIR}/src/flux_write.pio
    )
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

add_floppy_example(floppy_cli examples/cli.c)
target_link_libraries(floppy_cli hardware_watchdog)
add_floppy_example(floppy_selftest examples/selftest.c)
add_floppy_example(floppy_diskdump examples/diskdump.c)
add_floppy_example(floppy_mfmstats examples/mfmstats.c)
