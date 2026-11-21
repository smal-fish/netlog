if(NOT DEFINED NETLOG OR NOT DEFINED SAMPLE OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "NETLOG, SAMPLE and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(check_cli label expected_exit expected_output error_pattern)
    execute_process(COMMAND "${NETLOG}" ${ARGN}
        RESULT_VARIABLE actual_exit OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    string(REPLACE "\r\n" "\n" output "${output}")
    string(REPLACE "\r\n" "\n" errors "${errors}")
    if(NOT "${actual_exit}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR "${label}: exit ${actual_exit}, expected ${expected_exit}; ${errors}")
    endif()
    if(NOT "${output}" STREQUAL "${expected_output}")
        message(FATAL_ERROR "${label}: unexpected stdout: ${output}")
    endif()
    if("${error_pattern}" STREQUAL "")
        if(NOT "${errors}" STREQUAL "")
            message(FATAL_ERROR "${label}: unexpected stderr: ${errors}")
        endif()
    elseif(NOT errors MATCHES "${error_pattern}")
        message(FATAL_ERROR "${label}: unexpected stderr: ${errors}")
    endif()
endfunction()

check_cli(sample 0 "read_lines=6\nvalid_records=5\ninvalid_records=1\n"
    ":6: expected exactly 5 fields" --input "${SAMPLE}")

file(WRITE "${TEST_DIR}/empty.log" "")
check_cli(empty 0 "read_lines=0\nvalid_records=0\ninvalid_records=0\n" ""
    --input "${TEST_DIR}/empty.log")

file(WRITE "${TEST_DIR}/crlf.log" "0 dev INFO OK 0\r\n1 dev WARN SLOW 2\r\n")
check_cli(crlf 0 "read_lines=2\nvalid_records=2\ninvalid_records=0\n" ""
    --input "${TEST_DIR}/crlf.log")

file(WRITE "${TEST_DIR}/no final newline.log" "0 dev INFO OK 0")
check_cli(no_final_newline 0 "read_lines=1\nvalid_records=1\ninvalid_records=0\n" ""
    --input "${TEST_DIR}/no final newline.log")

file(WRITE "${TEST_DIR}/mixed.log" "\n0 dev INFO OK 0\nbroken_line\n1 dev ERROR TIMEOUT 10\n")
check_cli(continue_after_bad_lines 0 "read_lines=4\nvalid_records=2\ninvalid_records=2\n"
    ":1: expected exactly 5 fields\n[^\n]*:3: expected exactly 5 fields\n"
    --input "${TEST_DIR}/mixed.log")

file(WRITE "${TEST_DIR}/invalid.log" "0 dev INFO OK -1\n")
check_cli(all_invalid 0 "read_lines=1\nvalid_records=0\ninvalid_records=1\n"
    ":1: duration_us must contain decimal digits only" --input "${TEST_DIR}/invalid.log")

check_cli(missing_file 3 "" "Cannot inspect input file|Input is not a regular file"
    --input "${TEST_DIR}/does-not-exist.log")
check_cli(directory 3 "" "Input is not a regular file" --input "${TEST_DIR}")
check_cli(no_arguments 2 "" "Invalid command line")
check_cli(missing_input_value 2 "" "Invalid command line" --input)
check_cli(unknown_option 2 "" "Invalid command line" --threads 4)
check_cli(extra_option 2 "" "Invalid command line" --input "${SAMPLE}" --threads 4)
check_cli(help 0 "Usage: netlog --input <log-file>\n       netlog --help\n" "" --help)
message(STATUS "13 CLI scenarios passed")
