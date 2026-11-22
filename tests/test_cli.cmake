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

set(device_header "devices:\ndevice_id requests errors error_rate avg_us max_us\n")
set(event_header "top_events (up to 3):\n")

check_cli(sample 0 "read_lines=6\nvalid_records=5\ninvalid_records=1\n${device_header}dev01 3 1 33.33% 400.00 900\ndev02 2 1 50.00% 300.00 400\n${event_header}REQ_OK 3\nCRC_ERROR 1\nTIMEOUT 1\n"
    ":6: expected exactly 5 fields" --input "${SAMPLE}")

file(WRITE "${TEST_DIR}/empty.log" "")
check_cli(empty 0 "read_lines=0\nvalid_records=0\ninvalid_records=0\n${device_header}${event_header}" ""
    --input "${TEST_DIR}/empty.log")

file(WRITE "${TEST_DIR}/crlf.log" "0 dev INFO OK 0\r\n1 dev WARN SLOW 2\r\n")
check_cli(crlf 0 "read_lines=2\nvalid_records=2\ninvalid_records=0\n${device_header}dev 2 0 0.00% 1.00 2\n${event_header}OK 1\nSLOW 1\n" ""
    --input "${TEST_DIR}/crlf.log")

file(WRITE "${TEST_DIR}/no final newline.log" "0 dev INFO OK 0")
check_cli(no_final_newline 0 "read_lines=1\nvalid_records=1\ninvalid_records=0\n${device_header}dev 1 0 0.00% 0.00 0\n${event_header}OK 1\n" ""
    --input "${TEST_DIR}/no final newline.log")

file(WRITE "${TEST_DIR}/mixed.log" "\n0 dev INFO OK 0\nbroken_line\n1 dev ERROR TIMEOUT 10\n")
check_cli(continue_after_bad_lines 0 "read_lines=4\nvalid_records=2\ninvalid_records=2\n${device_header}dev 2 1 50.00% 5.00 10\n${event_header}OK 1\nTIMEOUT 1\n"
    ":1: expected exactly 5 fields\n[^\n]*:3: expected exactly 5 fields\n"
    --input "${TEST_DIR}/mixed.log")

file(WRITE "${TEST_DIR}/invalid.log" "0 dev INFO OK -1\n")
check_cli(all_invalid 0 "read_lines=1\nvalid_records=0\ninvalid_records=1\n${device_header}${event_header}"
    ":1: duration_us must contain decimal digits only" --input "${TEST_DIR}/invalid.log")

file(WRITE "${TEST_DIR}/ranking.log" "0 z INFO D 1\n0 a INFO C 1\n0 a INFO B 2\n0 a ERROR A 2\n")
check_cli(top_cutoff_and_device_order 0 "read_lines=4\nvalid_records=4\ninvalid_records=0\n${device_header}a 3 1 33.33% 1.67 2\nz 1 0 0.00% 1.00 1\n${event_header}A 1\nB 1\nC 1\n" ""
    --input "${TEST_DIR}/ranking.log")

file(WRITE "${TEST_DIR}/overflow.log" "0 dev INFO OK 18446744073709551615\n0 dev INFO OK 1\n")
check_cli(total_duration_overflow 4 "" "total duration for device dev exceeds uint64 range"
    --input "${TEST_DIR}/overflow.log")

check_cli(missing_file 3 "" "Cannot inspect input file|Input is not a regular file"
    --input "${TEST_DIR}/does-not-exist.log")
check_cli(directory 3 "" "Input is not a regular file" --input "${TEST_DIR}")
check_cli(no_arguments 2 "" "Invalid command line")
check_cli(missing_input_value 2 "" "Invalid command line" --input)
check_cli(unknown_option 2 "" "Invalid command line" --threads 4)
check_cli(extra_option 2 "" "Invalid command line" --input "${SAMPLE}" --threads 4)
check_cli(help 0 "Usage: netlog --input <log-file>\n       netlog --help\n" "" --help)
message(STATUS "15 CLI scenarios passed")
