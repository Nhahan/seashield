cmake_minimum_required(VERSION 3.24)
project(seashield LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SEASHIELD_BUILD_TESTS "Build unit tests" ON)
# Sanitizer preset: "address" enables ASan+UBSan, "thread" enables TSan.
set(SEASHIELD_SANITIZE "" CACHE STRING "Sanitizer preset: address, thread, or empty")

add_compile_options(-Wall -Wextra -Wpedantic)

if(SEASHIELD_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(SEASHIELD_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT SEASHIELD_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown SEASHIELD_SANITIZE value: ${SEASHIELD_SANITIZE}")
endif()

if(APPLE)
  set(SEASHIELD_EVENT_LOOP_BACKEND net/kqueue_event_loop.cpp)
elseif(UNIX)
  set(SEASHIELD_EVENT_LOOP_BACKEND net/epoll_event_loop.cpp)
else()
  message(FATAL_ERROR "Unsupported platform: SeaShield requires kqueue or epoll")
endif()

add_library(seashield_core STATIC
  core/logger.cpp
  net/acceptor.cpp
  net/event_loop.cpp
  net/frame_parser.cpp
  net/send_queue.cpp
  net/socket_util.cpp
  net/tcp_session.cpp
  net/udp_endpoint.cpp
  ${SEASHIELD_EVENT_LOOP_BACKEND}
)
target_include_directories(seashield_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(seashield_server server/main.cpp)
target_link_libraries(seashield_server PRIVATE seashield_core)

add_executable(loadclient tools/loadclient.cpp)
target_link_libraries(loadclient PRIVATE seashield_core)

if(SEASHIELD_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(seashield_tests
    tests/event_loop_test.cpp
    tests/frame_parser_test.cpp
    tests/integration_test.cpp
    tests/send_queue_test.cpp
    tests/unique_fd_test.cpp
  )
  target_link_libraries(seashield_tests PRIVATE seashield_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(seashield_tests)
endif()
