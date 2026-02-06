cmake_minimum_required(VERSION 3.13)
project(floppy_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -g -O2)

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
