cmake_minimum_required(VERSION 3.13)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico)
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(floppy C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

set(FLOPPY_WARNINGS
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wformat=2
    -Wundef
    -Wstrict-prototypes)

set(FLOPPY_C_OPTIONS
    -std=c11
    ${FLOPPY_WARNINGS})

set(FLOPPY_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/crc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/floppy.c
    ${CMAKE_CURRENT_LIST_DIR}/src/f12.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fat12.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mfm_decode.c
    ${CMAKE_CURRENT_LIST_DIR}/src/mfm_encode.c)

set_property(SOURCE ${FLOPPY_SOURCES} APPEND PROPERTY
    COMPILE_OPTIONS ${FLOPPY_C_OPTIONS})

add_library(floppy_lib STATIC ${FLOPPY_SOURCES})

target_include_directories(floppy_lib PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(floppy_lib PUBLIC
    pico_stdlib
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_clocks
)

pico_generate_pio_header(floppy_lib
    ${CMAKE_CURRENT_LIST_DIR}/src/flux_read.pio
    ${CMAKE_CURRENT_LIST_DIR}/src/flux_write.pio
)

function(add_floppy_example name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE floppy_lib)
    set_property(SOURCE ${source} APPEND PROPERTY
        COMPILE_OPTIONS ${FLOPPY_C_OPTIONS})
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

add_floppy_example(floppy_cli examples/cli.c)
target_link_libraries(floppy_cli PRIVATE hardware_watchdog)
