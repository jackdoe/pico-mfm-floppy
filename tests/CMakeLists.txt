cmake_minimum_required(VERSION 3.13)
project(floppy_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -g -O2)

option(ENABLE_SANITIZERS "Enable host sanitizers" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(SRCDIR ${CMAKE_CURRENT_LIST_DIR}/../src)
set(STUBS ${CMAKE_CURRENT_LIST_DIR}/stubs)

set(SRCS
    ${SRCDIR}/crc.c
    ${SRCDIR}/fat12.c
    ${SRCDIR}/lru.c
    ${SRCDIR}/mfm_decode.c
    ${SRCDIR}/mfm_encode.c
    ${SRCDIR}/f12.c
)

function(add_test_exe name)
    add_executable(${name} ${name}.c ${SRCS})
    target_include_directories(${name} PRIVATE ${STUBS} ${SRCDIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

function(add_test_exe_minimal name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE ${STUBS} ${SRCDIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()

add_test_exe(test_mfm)
add_test_exe(test_fat12)
add_test_exe_minimal(test_lru ${SRCDIR}/lru.c)
add_test_exe(test_robustness)
add_test_exe(test_fuzz)
add_test_exe(test_f12)

add_executable(test_flux_sim test_flux_sim.c flux_sim.c ${SRCS})
target_include_directories(test_flux_sim PRIVATE ${STUBS} ${SRCDIR})
add_test(NAME test_flux_sim COMMAND test_flux_sim)

add_executable(test_scp_fat12 test_scp_fat12.c flux_sim.c ${SRCS})
target_include_directories(test_scp_fat12 PRIVATE ${STUBS} ${SRCDIR})
add_test(NAME test_scp_fat12 COMMAND test_scp_fat12)

add_executable(test_scp_roundtrip test_scp_roundtrip.c flux_sim.c ${SRCS})
target_include_directories(test_scp_roundtrip PRIVATE ${STUBS} ${SRCDIR})
add_test(NAME test_scp_roundtrip COMMAND test_scp_roundtrip)

add_executable(test_pio_sim test_pio_sim.c pio_sim.c flux_sim.c ${SRCS} ${SRCDIR}/floppy.c)
target_include_directories(test_pio_sim PRIVATE ${STUBS} ${SRCDIR})
target_compile_definitions(test_pio_sim PRIVATE FLOPPY_DEBUG=0)
add_test(NAME test_pio_sim COMMAND test_pio_sim)

add_executable(test_pio_emu test_pio_emu.c pio_emu.c flux_sim.c ${SRCS})
target_include_directories(test_pio_emu PRIVATE ${STUBS} ${SRCDIR})
add_test(NAME test_pio_emu COMMAND test_pio_emu)

add_executable(test_write_verify test_write_verify.c pio_sim.c flux_sim.c ${SRCS} ${SRCDIR}/floppy.c)
target_include_directories(test_write_verify PRIVATE ${STUBS} ${SRCDIR})
target_compile_definitions(test_write_verify PRIVATE FLOPPY_DEBUG=0)
add_test(NAME test_write_verify COMMAND test_write_verify)

add_executable(test_floppy_overflow test_floppy_overflow.c pio_sim.c ${SRCS} ${SRCDIR}/floppy.c)
target_include_directories(test_floppy_overflow PRIVATE ${STUBS} ${SRCDIR})
target_compile_definitions(test_floppy_overflow PRIVATE
    FLOPPY_DEBUG=0
    FLOPPY_FLUX_BUF_SIZE=64)
add_test(NAME test_floppy_overflow COMMAND test_floppy_overflow)

add_executable(test_floppy_timeouts test_floppy_timeouts.c pio_sim.c ${SRCS} ${SRCDIR}/floppy.c)
target_include_directories(test_floppy_timeouts PRIVATE ${STUBS} ${SRCDIR})
target_compile_definitions(test_floppy_timeouts PRIVATE
    FLOPPY_DEBUG=0
    FLOPPY_FLUX_WAIT_TIMEOUT_MS=5
    FLOPPY_INDEX_TIMEOUT_MS=5
    FLOPPY_TX_DRAIN_TIMEOUT_MS=5)
add_test(NAME test_floppy_timeouts COMMAND test_floppy_timeouts)
