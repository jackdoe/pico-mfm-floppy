cmake_minimum_required(VERSION 3.13)
project(floppy_tests C)

include(../cmake/project_options.cmake)
set(CMAKE_C_STANDARD 23)
add_compile_options(${FLOPPY_WARNINGS})

option(ENABLE_SANITIZERS "Enable host sanitizers" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(SRCDIR ${CMAKE_CURRENT_LIST_DIR}/../src)
set(STUBS ${CMAKE_CURRENT_LIST_DIR}/stubs)
set(GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)

set(SCP_FIXTURE "$ENV{SCP_FIXTURE}" CACHE FILEPATH "SCP flux capture of a canonical 1.44 MB FAT12 disk")
if(NOT SCP_FIXTURE)
    set(SCP_FIXTURE
        "${CMAKE_CURRENT_LIST_DIR}/../system-shock-multilingual-floppy-ibm-pc/disk1.scp"
        CACHE FILEPATH "SCP flux capture of a canonical 1.44 MB FAT12 disk" FORCE)
endif()
set(HAVE_SCP_FIXTURE OFF)
if(EXISTS "${SCP_FIXTURE}")
    file(SIZE "${SCP_FIXTURE}" SCP_FIXTURE_SIZE)
    if(SCP_FIXTURE_SIZE GREATER 0)
        set(HAVE_SCP_FIXTURE ON)
    endif()
endif()
if(NOT HAVE_SCP_FIXTURE)
    message(WARNING "SCP fixture not found; real-media tests are skipped: ${SCP_FIXTURE}")
endif()

set(SCP_SEED 3235823838 CACHE STRING "Deterministic SCP roundtrip seed")
set(SCP_ITERATIONS 4 CACHE STRING "SCP fuzz roundtrip iterations")
if(NOT SCP_ITERATIONS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "SCP_ITERATIONS must be a positive integer")
endif()

find_program(PIOASM
    NAMES pioasm
    HINTS
        $ENV{PICO_SDK_PATH}/build/pioasm
        $ENV{PICO_SDK_PATH}/build/pioasm-install/pioasm)
if(NOT PIOASM)
    message(FATAL_ERROR "pioasm not found")
endif()

set(FLUX_READ_HEADER ${GENERATED}/flux_read.pio.h)
set(FLUX_WRITE_HEADER ${GENERATED}/flux_write.pio.h)

add_custom_command(
    OUTPUT ${FLUX_READ_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED}
    COMMAND ${PIOASM} -o c-sdk -v 0 ${SRCDIR}/flux_read.pio ${FLUX_READ_HEADER}
    DEPENDS ${SRCDIR}/flux_read.pio
    VERBATIM)

add_custom_command(
    OUTPUT ${FLUX_WRITE_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED}
    COMMAND ${PIOASM} -o c-sdk -v 0 ${SRCDIR}/flux_write.pio ${FLUX_WRITE_HEADER}
    DEPENDS ${SRCDIR}/flux_write.pio
    VERBATIM)

add_subdirectory(${SRCDIR} ${CMAKE_CURRENT_BINARY_DIR}/core)
add_subdirectory(../examples ${CMAKE_CURRENT_BINARY_DIR}/rpc)

set(SIM_NOISE ${CMAKE_CURRENT_LIST_DIR}/flux_noise.c)

function(add_test_exe name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${STUBS})
    target_link_libraries(${name} PRIVATE floppy_core)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

enable_testing()

add_test_exe(test_disk)
add_test_exe(test_cache)
add_test_exe(test_mfm)
add_test_exe(test_fat12)
add_test_exe(test_robustness)
add_test_exe(test_fuzz)
add_test_exe(test_f12)
add_test_exe(test_rpc)
target_link_libraries(test_rpc PRIVATE floppy_rpc)

add_executable(test_flux_sim test_flux_sim.c flux_sim.c ${SIM_NOISE})
target_include_directories(test_flux_sim PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_flux_sim PRIVATE floppy_core)
if(HAVE_SCP_FIXTURE)
    add_test(NAME test_flux_sim COMMAND test_flux_sim --fixture ${SCP_FIXTURE})
else()
    add_test(NAME test_flux_sim COMMAND test_flux_sim)
endif()

if(HAVE_SCP_FIXTURE)

add_executable(test_scp_fat12 test_scp_fat12.c flux_sim.c ${SIM_NOISE})
target_include_directories(test_scp_fat12 PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_scp_fat12 PRIVATE floppy_core)
add_test(NAME test_scp_fat12
    COMMAND test_scp_fat12 --fixture ${SCP_FIXTURE})

add_executable(test_scp_roundtrip test_scp_roundtrip.c flux_sim.c ${SIM_NOISE})
target_include_directories(test_scp_roundtrip PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_scp_roundtrip PRIVATE floppy_core)
add_test(NAME test_scp_roundtrip
    COMMAND test_scp_roundtrip --fixture ${SCP_FIXTURE}
        --seed ${SCP_SEED} --iterations ${SCP_ITERATIONS})
endif()

add_executable(test_pio_sim test_pio_sim.c pio_sim.c flux_sim.c ${SIM_NOISE}
    ${SRCDIR}/floppy.c)
target_include_directories(test_pio_sim PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_pio_sim PRIVATE floppy_core)
add_test(NAME test_pio_sim COMMAND test_pio_sim)

add_executable(test_pio_emu test_pio_emu.c pio_emu.c flux_sim.c ${SIM_NOISE}
    ${FLUX_READ_HEADER} ${FLUX_WRITE_HEADER})
target_include_directories(test_pio_emu PRIVATE ${GENERATED} ${STUBS} ${SRCDIR})
target_link_libraries(test_pio_emu PRIVATE floppy_core)
target_compile_definitions(test_pio_emu PRIVATE PICO_NO_HARDWARE=1)
add_test(NAME test_pio_emu COMMAND test_pio_emu)

add_executable(test_write_verify test_write_verify.c pio_sim.c flux_sim.c
    ${SIM_NOISE} ${SRCDIR}/floppy.c)
target_include_directories(test_write_verify PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_write_verify PRIVATE floppy_core)
add_test(NAME test_write_verify COMMAND test_write_verify)

add_executable(test_mfm_probe test_mfm_probe.c pio_sim.c
    ${SIM_NOISE} ${SRCDIR}/floppy.c ../examples/mfm_probe.c)
target_include_directories(test_mfm_probe PRIVATE ${STUBS} ${SRCDIR} ../examples)
target_link_libraries(test_mfm_probe PRIVATE floppy_core)
add_test(NAME test_mfm_probe COMMAND test_mfm_probe)

add_executable(test_floppy_underrun test_floppy_underrun.c pio_sim.c
    ${SIM_NOISE} ${SRCDIR}/floppy.c)
target_include_directories(test_floppy_underrun PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_floppy_underrun PRIVATE floppy_core)
target_compile_definitions(test_floppy_underrun PRIVATE
    FLOPPY_WRITE_FLUX_TIMEOUT_MS=5)
add_test(NAME test_floppy_underrun COMMAND test_floppy_underrun)

add_executable(test_floppy_timeouts test_floppy_timeouts.c pio_sim.c
    ${SIM_NOISE} ${SRCDIR}/floppy.c)
target_include_directories(test_floppy_timeouts PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_floppy_timeouts PRIVATE floppy_core)
target_compile_definitions(test_floppy_timeouts PRIVATE
    FLOPPY_FLUX_WAIT_TIMEOUT_MS=5
    FLOPPY_INDEX_TIMEOUT_MS=5)
add_test(NAME test_floppy_timeouts COMMAND test_floppy_timeouts)

if(HAVE_SCP_FIXTURE)
add_executable(test_e2e_corruption test_e2e_corruption.c pio_sim.c flux_sim.c
    ${SIM_NOISE} ${SRCDIR}/floppy.c)
target_include_directories(test_e2e_corruption PRIVATE ${STUBS} ${SRCDIR})
target_link_libraries(test_e2e_corruption PRIVATE floppy_core)
add_test(NAME test_e2e_corruption
    COMMAND test_e2e_corruption --fixture ${SCP_FIXTURE})
endif()

get_property(TEST_NAMES DIRECTORY PROPERTY TESTS)
if(NOT TEST_NAMES)
    message(FATAL_ERROR "No tests registered")
endif()
if(APPLE)
    set(SANITIZER_ENV
        "ASAN_OPTIONS=halt_on_error=1:abort_on_error=1"
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
else()
    set(SANITIZER_ENV
        "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1"
        "LSAN_OPTIONS=exitcode=23"
        "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()
set_tests_properties(${TEST_NAMES} PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "${SANITIZER_ENV}")
set_tests_properties(test_fuzz PROPERTIES TIMEOUT 300)
if(HAVE_SCP_FIXTURE)
    set_tests_properties(test_flux_sim test_scp_fat12 test_scp_roundtrip
        test_e2e_corruption PROPERTIES TIMEOUT 900)
endif()

add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E echo "Running coverage suite..."
    COMMAND ${CMAKE_CURRENT_LIST_DIR}/run_coverage.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    USES_TERMINAL)
