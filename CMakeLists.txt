cmake_minimum_required(VERSION 3.20)
project(hydrogen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(compiler)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME golden
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_tests.py
                 $<TARGET_FILE:h2c> ${CMAKE_SOURCE_DIR})
