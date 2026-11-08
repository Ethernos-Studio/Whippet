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
