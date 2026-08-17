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
