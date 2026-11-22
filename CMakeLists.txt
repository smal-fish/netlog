cmake_minimum_required(VERSION 3.16)
project(netlog VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(netlog_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(netlog_parser src/parser.cpp)
target_include_directories(netlog_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
netlog_warnings(netlog_parser)

add_library(netlog_stats src/stats.cpp)
target_include_directories(netlog_stats PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
netlog_warnings(netlog_stats)

find_package(Threads REQUIRED)
add_executable(netlog src/main.cpp)
target_link_libraries(netlog PRIVATE netlog_parser netlog_stats Threads::Threads)
netlog_warnings(netlog)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_parser tests/test_parser.cpp)
    target_link_libraries(test_parser PRIVATE netlog_parser)
    netlog_warnings(test_parser)
    foreach(parser_case IN ITEMS
        valid_fields numeric_maxima whitespace_and_zeros bom embedded_nul owned_strings
        empty_line blank_line one_field missing_field extra_field comment
        negative_timestamp signed_timestamp fractional_timestamp timestamp_suffix
        hex_timestamp scientific_timestamp timestamp_overflow huge_timestamp dotted_id
        slash_id lowercase_level unknown_level dotted_event negative_duration signed_duration
        fractional_duration duration_suffix hex_duration duration_overflow huge_duration first_error
    )
        add_test(NAME parser.${parser_case} COMMAND test_parser "${parser_case}")
    endforeach()

    add_executable(test_stats tests/test_stats.cpp)
    target_link_libraries(test_stats PRIVATE netlog_stats)
    netlog_warnings(test_stats)
    foreach(stats_case IN ITEMS device_totals event_ranking overflow_preserves_totals merge_totals merge_overflow)
        add_test(NAME stats.${stats_case} COMMAND test_stats "${stats_case}")
    endforeach()

    foreach(cli_case IN ITEMS
        sample empty crlf no_final_newline continue_after_bad_lines all_invalid
        top_cutoff_and_device_order total_duration_overflow missing_file directory directory_split directory_overflow
        no_arguments missing_input_value unknown_option extra_option help
        parallel_matches_serial parallel_single_file parallel_overflow parallel_worker_error
        fixture_aggregation fixture_top-k fixture_mixed
    )
        add_test(NAME cli.${cli_case}
            COMMAND "${CMAKE_COMMAND}"
                "-DNETLOG=$<TARGET_FILE:netlog>"
                "-DSAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/data/sample.log"
                "-DSAMPLES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/samples"
                "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-data"
                "-DCASE=${cli_case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake")
    endforeach()
endif()
