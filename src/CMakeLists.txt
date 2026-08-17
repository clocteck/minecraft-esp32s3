cmake_minimum_required(VERSION 3.16)

set(CLASSICUBE_PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(CLASSICUBE_ROOT "${CLASSICUBE_PORT_ROOT}/upstream/ClassiCube" CACHE PATH "ClassiCube checkout")
set(CUBIC_HOST_ROOT "C:/Users/wzh/Documents/PlatformIO/Projects/260105-200034-arduino-wifiscan" CACHE PATH "Cubic S3 firmware checkout")

if(NOT EXISTS "${CLASSICUBE_ROOT}/src/Game.c")
    message(FATAL_ERROR "ClassiCube checkout not found at ${CLASSICUBE_ROOT}")
endif()
if(NOT EXISTS "${CUBIC_HOST_ROOT}/src/dynmod/module_abi.h")
    message(FATAL_ERROR "Current host module_abi.h not found under ${CUBIC_HOST_ROOT}")
endif()
if(NOT EXISTS "${CUBIC_HOST_ROOT}/components/elf_loader/elf_loader.cmake")
    message(FATAL_ERROR "Current host elf_loader component not found under ${CUBIC_HOST_ROOT}")
endif()

set(EXTRA_COMPONENT_DIRS "${CUBIC_HOST_ROOT}/components/elf_loader")
set(COMPONENTS main elf_loader)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(classicube)

idf_build_set_property(COMPILE_DEFINITIONS "PLAT_ESP32S3=1" APPEND)
foreach(include_dir
    "${CMAKE_CURRENT_LIST_DIR}/main"
    "${CUBIC_HOST_ROOT}/src/dynmod"
    "${CLASSICUBE_ROOT}/src")
    idf_build_set_property(COMPILE_INCLUDE_DIRECTORIES "${include_dir}" APPEND)
endforeach()

execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
    OUTPUT_VARIABLE CLASSICUBE_LIBGCC
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libm.a
    OUTPUT_VARIABLE CLASSICUBE_LIBM
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libc.a
    OUTPUT_VARIABLE CLASSICUBE_LIBC
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${CMAKE_AR} x "${CLASSICUBE_LIBC}" libc_a-setjmp.o
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE CLASSICUBE_SETJMP_RESULT)
if(NOT CLASSICUBE_SETJMP_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to extract the PIC-safe Xtensa setjmp object")
endif()

set(ELF_LIBS
    "${CMAKE_BINARY_DIR}/esp-idf/main/libmain.a"
    "${CLASSICUBE_LIBM}"
    "${CMAKE_BINARY_DIR}/libc_a-setjmp.o"
    "${CLASSICUBE_LIBGCC}")

include("${CMAKE_CURRENT_LIST_DIR}/cmake/elf_loader_port.cmake")
project_so(classicube)
