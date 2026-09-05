add_library(floppy_core STATIC
    crc.c
    disk.c
    cache.c
    mfm.c
    fat12.c
    fat12_storage.c
    fat12_file.c
    fat12_check.c
    f12.c)

target_include_directories(floppy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(floppy_core PRIVATE ${FLOPPY_C_OPTIONS})

set_target_properties(floppy_core PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
