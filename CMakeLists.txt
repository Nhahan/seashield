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
# Determinism: forbid FMA contraction so the simulation produces identical
# bit patterns across optimization levels of the same toolchain (charter §5.1).
add_compile_options(-ffp-contract=off)
# Determinism: at -O1+ the compiler fuses same-argument sin/cos pairs into a
# combined sincos call whose results can differ from the separate libm calls
# by 1 ulp — enough to fork the state-hash regression between build types.
add_compile_options(-fno-builtin-sin -fno-builtin-cos)

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

# Shared protocol core (charter §4.1 "프로토콜 코어의 공유"): wire format,
# message catalogue, reliable-UDP channel logic. Depends on the standard
# library only, so the same archive links into the server, the dummy client,
# and later the UE5 client module without dragging net/sim along.
add_library(seashield_protocol STATIC
  protocol/messages.cpp
  protocol/reliable.cpp
)
target_include_directories(seashield_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(seashield_core STATIC
  core/logger.cpp
  sim/ballistics.cpp
  sim/environment.cpp
  sim/fire_control.cpp
  sim/journal.cpp
  sim/radar.cpp
  sim/scenario.cpp
  sim/tracking.cpp
  sim/world.cpp
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

# Server assembly and headless tool components (sim server, chaos proxy,
# dummy client) shared between executables and the integration tests.
add_library(seashield_app STATIC
  server/sim_server.cpp
  tools/dummy_client.cpp
  tools/net_chaos_proxy.cpp
)
target_link_libraries(seashield_app PUBLIC seashield_core seashield_protocol)

add_executable(seashield_server server/main.cpp)
target_link_libraries(seashield_server PRIVATE seashield_app)

add_executable(loadclient tools/loadclient.cpp)
target_link_libraries(loadclient PRIVATE seashield_core)

add_executable(ballistic_sandbox tools/ballistic_sandbox/main.cpp)
target_link_libraries(ballistic_sandbox PRIVATE seashield_core)

add_executable(netproxy tools/netproxy_main.cpp)
target_link_libraries(netproxy PRIVATE seashield_app)

add_executable(dummyclient tools/dummyclient_main.cpp)
target_link_libraries(dummyclient PRIVATE seashield_app)

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
    tests/ballistics_test.cpp
    tests/determinism_test.cpp
    tests/engagement_test.cpp
    tests/environment_test.cpp
    tests/fire_control_test.cpp
    tests/event_loop_test.cpp
    tests/frame_parser_test.cpp
    tests/integration_test.cpp
    tests/math_test.cpp
    tests/matrix_test.cpp
    tests/messages_test.cpp
    tests/pcg32_test.cpp
    tests/radar_test.cpp
    tests/protocol_integration_test.cpp
    tests/reliable_test.cpp
    tests/scenario_test.cpp
    tests/send_queue_test.cpp
    tests/spsc_queue_test.cpp
    tests/target_test.cpp
    tests/tracking_test.cpp
    tests/unique_fd_test.cpp
    tests/wire_test.cpp
    tests/world_test.cpp
  )
  target_link_libraries(seashield_tests PRIVATE seashield_app GTest::gtest_main)
  target_compile_definitions(seashield_tests
    PRIVATE SEASHIELD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  include(GoogleTest)
  gtest_discover_tests(seashield_tests)
endif()
