add_executable(test_ir_roundtrip test_ir_roundtrip.c)
target_link_libraries(test_ir_roundtrip PRIVATE whippet_core)
whippet_copy_sanitizer_runtime(test_ir_roundtrip)

set(WHIPPET_ROUNDTRIP_CASES
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/control.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/loops.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/ops.wir
    ${CMAKE_CURRENT_SOURCE_DIR}/cases/calls.wir
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

# Exercise the CLI emit path (canonical IR and LF line endings) end to end.
add_test(NAME cli_emit
         COMMAND whippet -o ${CMAKE_CURRENT_BINARY_DIR}/basic.cli.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir)
add_test(NAME cli_emit_matches
         COMMAND ${CMAKE_COMMAND} -E compare_files
                 ${CMAKE_CURRENT_BINARY_DIR}/basic.cli.wir
                 ${CMAKE_CURRENT_SOURCE_DIR}/cases/basic.wir)
set_tests_properties(cli_emit_matches PROPERTIES DEPENDS cli_emit)
