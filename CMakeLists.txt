# Whippet -- independent compiler backend.
# Author: dhjs0000

cmake_minimum_required(VERSION 3.20)
project(whippet VERSION 0.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(WHIPPET_SANITIZE "Enable ASan+UBSan (Clang/GCC only)" OFF)
option(WHIPPET_WERROR "Treat compiler warnings as errors" ON)

# The MSVC CRT marks fopen/strtof family as deprecated; we use the portable
# C11 names on purpose (same source builds on Linux).
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(WHIPPET_WARNING_FLAGS
    $<$<C_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

add_library(whippet_core STATIC
    src/ir/ir.c
    src/ir/parse.c
    src/ir/print.c
    src/mid/passes.c
    src/isel/isel.c
    src/regalloc/linear.c
    src/emit/as.c
    src/emit/coff.c
)
target_include_directories(whippet_core PUBLIC include PRIVATE src)
target_compile_options(whippet_core PRIVATE ${WHIPPET_WARNING_FLAGS})
if(WHIPPET_WERROR)
    target_compile_options(whippet_core PRIVATE
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Werror>)
endif()

if(WHIPPET_SANITIZE)
    if(MSVC)
        message(WARNING "MSVC 仅支持 /fsanitize=address 且无 UBSan；建议调试档直接用 clang")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# On Windows the Clang ASan runtime is a DLL; copy it next to every binary so
# the sanitizer build is runnable without editing PATH.
set(WHIPPET_ASAN_DLL "")
if(WHIPPET_SANITIZE AND WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" --print-resource-dir
        OUTPUT_VARIABLE WHIPPET_CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(WHIPPET_CLANG_RESOURCE_DIR)
        set(WHIPPET_SANITIZER_BIN_DIR
            "${WHIPPET_CLANG_RESOURCE_DIR}/lib/windows")
        if(EXISTS
           "${WHIPPET_SANITIZER_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
            set(WHIPPET_ASAN_DLL
                "${WHIPPET_SANITIZER_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        endif()
    endif()
endif()

function(whippet_copy_sanitizer_runtime target)
    if(WHIPPET_ASAN_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WHIPPET_ASAN_DLL}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

add_executable(whippet src/cli/main.c)
target_link_libraries(whippet PRIVATE whippet_core)
target_compile_options(whippet PRIVATE ${WHIPPET_WARNING_FLAGS})
if(WHIPPET_WERROR)
    target_compile_options(whippet PRIVATE
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Werror>)
endif()
whippet_copy_sanitizer_runtime(whippet)

enable_testing()
add_subdirectory(tests)
