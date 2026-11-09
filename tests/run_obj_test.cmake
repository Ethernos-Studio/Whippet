# Whippet -- independent compiler backend.
# Author: dhjs0000
#
# Generate a COFF object with the internal assembler, link it with clang and a
# C driver, then run it.

foreach(variable WHIPPET CLANG SOURCE DRIVER WORKDIR NAME)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "missing -D${variable}")
    endif()
endforeach()

set(object_file "${WORKDIR}/${NAME}.obj")
set(executable "${WORKDIR}/${NAME}.exe")

execute_process(
    COMMAND "${WHIPPET}" -c -o "${object_file}" "${SOURCE}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)
if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
            "whippet -c failed (${compile_result})\n${compile_stdout}${compile_stderr}")
endif()

execute_process(
    COMMAND "${CLANG}" "${DRIVER}" "${object_file}" -o "${executable}"
    RESULT_VARIABLE link_result
    OUTPUT_VARIABLE link_stdout
    ERROR_VARIABLE link_stderr)
if(NOT link_result EQUAL 0)
    message(FATAL_ERROR
            "link failed (${link_result})\n${link_stdout}${link_stderr}")
endif()

execute_process(
    COMMAND "${executable}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
            "run failed (${run_result})\n${run_stdout}${run_stderr}")
endif()
