# Author: dhjs0000
#
# End-to-end M1 check: generate assembly with whippet, assemble/link it with
# clang, run the result, and require exit code 0. Invoked by ctest on Windows
# with the MSVC ABI.

foreach(required WHIPPET CLANG SOURCE DRIVER WORKDIR NAME)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_asm_test.cmake: missing -D${required}")
    endif()
endforeach()

set(generated_asm "${WORKDIR}/${NAME}.s")
set(generated_exe "${WORKDIR}/${NAME}_test.exe")
file(REMOVE "${generated_asm}" "${generated_exe}")

execute_process(
    COMMAND "${WHIPPET}" -S "${SOURCE}" -o "${generated_asm}"
    RESULT_VARIABLE codegen_result
    OUTPUT_VARIABLE codegen_stdout
    ERROR_VARIABLE codegen_stderr)
if(NOT codegen_result EQUAL 0)
    message(FATAL_ERROR
            "whippet -S failed (${codegen_result})\n${codegen_stdout}${codegen_stderr}")
endif()

execute_process(
    COMMAND "${CLANG}" "${DRIVER}" "${generated_asm}" -o "${generated_exe}"
    RESULT_VARIABLE link_result
    OUTPUT_VARIABLE link_stdout
    ERROR_VARIABLE link_stderr)
if(NOT link_result EQUAL 0)
    message(FATAL_ERROR
            "clang assemble/link failed (${link_result})\n${link_stdout}${link_stderr}")
endif()

execute_process(
    COMMAND "${generated_exe}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
            "generated code failed (${run_result})\n${run_stdout}${run_stderr}")
endif()

message(STATUS "ok ${NAME}: ${run_stdout}")
