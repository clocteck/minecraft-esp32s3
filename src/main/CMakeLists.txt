set(CLASSICUBE_SRC "${CLASSICUBE_ROOT}/src")
set(MODULE_ABI_DIR "${CUBIC_HOST_ROOT}/src/dynmod")

file(GLOB CLASSICUBE_COMMON_SOURCES "${CLASSICUBE_SRC}/*.c")

idf_component_register(
    SRCS
        "classicube_module.c"
        "holo_port.c"
        "holo_perf.c"
        "Audio_Holo.c"
        "Platform_Holo.c"
        "Window_Holo.c"
        ${CLASSICUBE_COMMON_SOURCES}
    INCLUDE_DIRS
        "."
        "${MODULE_ABI_DIR}"
        "${CLASSICUBE_SRC}"
    REQUIRES
        newlib)

option(CLASSICUBE_ENABLE_SCREENSHOTS "Enable PNG screenshots in ESP32-S3 debug builds" OFF)
if(CLASSICUBE_ENABLE_SCREENSHOTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CC_BUILD_ESP32S3_SCREENSHOTS=1)
endif()

option(CLASSICUBE_ENABLE_HOT_PROBES "Enable per-quad and raster hot-path probes" OFF)
if(CLASSICUBE_ENABLE_HOT_PROBES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CC_BUILD_ESP32S3_HOT_PROBES=1)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -fPIC
    -mlongcalls
    -fvisibility=hidden
    -fdata-sections
    -ffunction-sections
    -fno-strict-aliasing
    -fno-jump-tables
    -fno-tree-switch-conversion
    -fno-builtin-memcpy
    -fno-builtin-memmove
    -fno-builtin-memset
    -fno-builtin-malloc
    -fno-builtin-calloc
    -fno-builtin-realloc
    -fno-builtin-free
    -Wno-error
    -Wno-error=unused-parameter
    -Wno-error=implicit-fallthrough)
