cmake_minimum_required(VERSION 3.13)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2)
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(floppy C CXX ASM)

include(cmake/project_options.cmake)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

add_subdirectory(src)
add_subdirectory(examples)

add_library(floppy_lib STATIC src/floppy.c)
set_property(SOURCE src/floppy.c APPEND PROPERTY
    COMPILE_OPTIONS ${FLOPPY_C_OPTIONS})

target_link_libraries(floppy_lib PUBLIC
    floppy_core
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
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/examples)
    set_property(SOURCE ${source} APPEND PROPERTY
        COMPILE_OPTIONS ${FLOPPY_C_OPTIONS})
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
    pico_set_binary_type(${name} copy_to_ram)
endfunction()

add_floppy_example(floppy_cli examples/cli.c)
target_sources(floppy_cli PRIVATE examples/mfm_probe.c)
set_property(SOURCE examples/mfm_probe.c APPEND PROPERTY
    COMPILE_OPTIONS ${FLOPPY_C_OPTIONS})
target_link_libraries(floppy_cli PRIVATE hardware_watchdog)

option(FLOPPY_ALT_PINS "Use the alternate GPIO pin mapping in the CLI example" OFF)
if(FLOPPY_ALT_PINS)
    target_compile_definitions(floppy_cli PRIVATE FLOPPY_ALT_PINS)
endif()

add_floppy_example(floppy_rpc_slave examples/rpc_slave.c)
target_link_libraries(floppy_rpc_slave PRIVATE hardware_spi floppy_rpc)

add_floppy_example(floppy_rpc_master examples/rpc_master.c)
target_link_libraries(floppy_rpc_master PRIVATE hardware_spi floppy_rpc)
