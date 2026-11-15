# Shared C++ code used by both the harness and the server module:
#   wlb_base: Chromium-style threads (default and epoll IO message pumps),
#             task runners, callbacks, WeakPtr, TimeDelta/TimeTicks.
#   wlb_net:  line framing, Unix socket server/client (on IO threads).
# No third-party runtime dependencies (GoogleTest for tests only).
# Plain C++20 (AzerothCore modules compile as C++20 and include these headers).
cmake_minimum_required(VERSION 3.20)
project(wlb_common C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(WLB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(wlb_base STATIC
  base/message_pump_default.cc
  base/message_pump_epoll.cc
  base/platform_thread.cc
  base/sequenced_task_runner.cc
  base/task_queue.cc
  base/thread.cc
  base/thread_pool.cc
  base/time.cc)
target_include_directories(wlb_base PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(wlb_base PRIVATE -Wall -Wextra)

add_library(wlb_net STATIC
  net/line_connection.cc
  net/line_framer.cc
  net/unix_socket_client.cc
  net/unix_socket_server.cc)
target_link_libraries(wlb_net PUBLIC wlb_base)
target_compile_options(wlb_net PRIVATE -Wall -Wextra)

# Unit tests.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${WLB_ROOT}/third_party/googletest" googletest EXCLUDE_FROM_ALL)
add_executable(wlb_common_unittests
  base/callback_unittest.cc
  base/task_queue_unittest.cc
  base/thread_unittest.cc
  base/time_unittest.cc
  net/line_framer_unittest.cc
  net/unix_socket_unittest.cc)
target_link_libraries(wlb_common_unittests PRIVATE wlb_net GTest::gtest_main)
