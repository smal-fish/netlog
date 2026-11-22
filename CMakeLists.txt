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

add_executable(netlog src/main.cpp src/stats.cpp)
target_link_libraries(netlog PRIVATE netlog_parser)
netlog_warnings(netlog)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_parser tests/test_parser.cpp)
    target_link_libraries(test_parser PRIVATE netlog_parser)
    netlog_warnings(test_parser)
    add_test(NAME parser COMMAND test_parser)
    add_test(NAME cli
        COMMAND "${CMAKE_COMMAND}"
            "-DNETLOG=$<TARGET_FILE:netlog>"
            "-DSAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/data/sample.log"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-data"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake")
endif()
