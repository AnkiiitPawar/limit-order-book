cmake_minimum_required(VERSION 3.20)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic> $<$<CXX_COMPILER_ID:MSVC>:/W4>)

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)
target_compile_options(lob PRIVATE ${WARNINGS})

add_executable(lob_tests tests/test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob)
target_compile_options(lob_tests PRIVATE ${WARNINGS})

add_executable(lob_bench bench/bench_order_book.cpp)
target_link_libraries(lob_bench PRIVATE lob)

enable_testing()
add_test(NAME lob_tests COMMAND lob_tests)
