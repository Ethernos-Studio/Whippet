# Whippet -- independent compiler backend.
# Author: dhjs0000

add_executable(test_ir_roundtrip test_ir_roundtrip.c)
target_link_libraries(test_ir_roundtrip PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_ir_roundtrip)

set(WHIPPET_ROUNDTRIP_CASES
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/control.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/loops.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/ops.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/calls.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/int_ops.wir
    ${CMAKE_SOURCE_DIR}/examples/add_mul.wir
    ${CMAKE_SOURCE_DIR}/examples/fib.wir
    ${CMAKE_SOURCE_DIR}/examples/classify.wir
    ${CMAKE_SOURCE_DIR}/examples/bitops.wir
    ${CMAKE_SOURCE_DIR}/examples/exit42.wir
)
add_test(NAME ir_roundtrip
         COMMAND test_ir_roundtrip ${WHIPPET_ROUNDTRIP_CASES})

add_executable(test_ir_errors test_ir_errors.c)
target_link_libraries(test_ir_errors PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_ir_errors)
add_test(NAME ir_diagnostics COMMAND test_ir_errors)

add_executable(test_ir_metadata test_ir_metadata.c)
target_link_libraries(test_ir_metadata PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_ir_metadata)
add_test(NAME ir_metadata COMMAND test_ir_metadata)

add_executable(test_ir_normalize test_ir_normalize.c)
target_link_libraries(test_ir_normalize PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_ir_normalize)
add_test(NAME ir_normalisation COMMAND test_ir_normalize)

add_executable(test_asm test_asm.c)
target_link_libraries(test_asm PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_asm)
add_test(NAME asm_codegen
         COMMAND test_asm
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/loops.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/int_ops.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/control.wir)

# Exercise the CLI emit path (canonical IR and LF line endings) end to end.
add_test(NAME cli_emit
         COMMAND whippet --emit-ir
                 -o ${CMAKE_CURRENT_BINARY_DIR}/basic.cli.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir)
add_test(NAME cli_emit_matches
         COMMAND ${CMAKE_COMMAND} -E compare_files
                 ${CMAKE_CURRENT_BINARY_DIR}/basic.cli.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir)
set_tests_properties(cli_emit_matches PROPERTIES DEPENDS cli_emit)

# End-to-end acceptance: generate Windows x64 assembly, link it with clang and
# a small C driver, then run the result. Only meaningful for the MSVC ABI.
if(WIN32
   AND CMAKE_C_COMPILER_ID MATCHES "Clang"
   AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    add_test(NAME asm_add_mul
             COMMAND ${CMAKE_COMMAND}
                     -DWHIPPET=$<TARGET_FILE:whippet>
                     -DCLANG=${CMAKE_C_COMPILER}
                     -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir
                     -DDRIVER=${CMAKE_CURRENT_SOURCE_DIR}/asm/driver_add_mul.c
                     -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DNAME=add_mul
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_asm_test.cmake)
    add_test(NAME asm_sum_to
             COMMAND ${CMAKE_COMMAND}
                     -DWHIPPET=$<TARGET_FILE:whippet>
                     -DCLANG=${CMAKE_C_COMPILER}
                     -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cases/loops.wir
                     -DDRIVER=${CMAKE_CURRENT_SOURCE_DIR}/asm/driver_sum_to.c
                     -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DNAME=sum_to
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_asm_test.cmake)
    add_test(NAME asm_int_ops
             COMMAND ${CMAKE_COMMAND}
                     -DWHIPPET=$<TARGET_FILE:whippet>
                     -DCLANG=${CMAKE_C_COMPILER}
                     -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cases/int_ops.wir
                     -DDRIVER=${CMAKE_CURRENT_SOURCE_DIR}/asm/driver_int_ops.c
                     -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DNAME=int_ops
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_asm_test.cmake)
    add_test(NAME asm_switch
             COMMAND ${CMAKE_COMMAND}
                     -DWHIPPET=$<TARGET_FILE:whippet>
                     -DCLANG=${CMAKE_C_COMPILER}
                     -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/cases/control.wir
                     -DDRIVER=${CMAKE_CURRENT_SOURCE_DIR}/asm/driver_classify.c
                     -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -DNAME=switch
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/run_asm_test.cmake)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME asm_differential
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/diff_test.py
                         --whippet $<TARGET_FILE:whippet>
                         --clang ${CMAKE_C_COMPILER}
                         --workdir ${CMAKE_CURRENT_BINARY_DIR}/diff
                         --functions 40
                         --ops 12
                         --seed 20261108)
    endif()

    find_program(POWERSHELL_EXECUTABLE powershell)
    if(POWERSHELL_EXECUTABLE)
        add_test(NAME examples
                 COMMAND ${POWERSHELL_EXECUTABLE} -NoProfile
                         -ExecutionPolicy Bypass
                         -File ${CMAKE_SOURCE_DIR}/examples/run_examples.ps1
                         -WhippetPath $<TARGET_FILE:whippet>)
    endif()
endif()
