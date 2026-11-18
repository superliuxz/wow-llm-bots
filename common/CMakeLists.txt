# Shared C++ code used by both the harness and the server module:
#   wlb_base: threads (default and epoll IO message pumps),
#             task runners, callbacks, WeakPtr, TimeDelta/TimeTicks.
#   wlb_net:  length-prefixed framing, Unix socket server/client (on IO threads).
#   wlb_proto: module <-> harness snapshot/command structs (proto/snapshot.h)
#             and Glaze BEVE/JSON codecs (proto/snapshot_wire.cc).
# Plain C++20 (AzerothCore modules compile as C++20 and include these headers).
cmake_minimum_required(VERSION 3.20)
project(wlb_common C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(WLB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(wlb_base STATIC
  base/message_loop/message_pump_default.cc
  base/strings/strcat.cc
  base/strings/string_number_conversions.cc
  base/strings/string_split.cc
  base/strings/string_util.cc
  base/strings/stringprintf.cc
  base/synchronization/condition_variable.cc
  base/synchronization/lock.cc
  base/synchronization/waitable_event.cc
  base/message_loop/message_pump_epoll.cc
  base/threading/platform_thread.cc
  base/task/sequenced_task_runner.cc
  base/task/task_queue.cc
  base/threading/thread.cc
  base/task/thread_pool.cc
  base/time/time.cc)
target_include_directories(wlb_base PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# Our code: Clang thread-safety analysis (as errors) and a hardened standard
# library (bounds-checked containers etc.) in every build.
set(WLB_SAFETY_FLAGS -Wall -Wextra -Wthread-safety -Werror=thread-safety)
# Hardening must cover a whole program; the copy linked into AzerothCore (not
# hardened) is built with WLB_HARDENED=OFF (tools/wlb build server).
option(WLB_HARDENED "Hardened standard library (whole-program builds)" ON)
if(WLB_HARDENED)
  add_compile_definitions(_GLIBCXX_ASSERTIONS
                          _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)
endif()
target_compile_options(wlb_base PRIVATE ${WLB_SAFETY_FLAGS})

add_library(wlb_net STATIC
  net/frame_codec.cc
  net/framed_connection.cc
  net/unix_socket_client.cc
  net/unix_socket_server.cc)
target_link_libraries(wlb_net PUBLIC wlb_base)
target_compile_options(wlb_net PRIVATE ${WLB_SAFETY_FLAGS})

# Protocol library: plain C++20 structs in proto/snapshot.h, serialized to BEVE
# (wire) and JSON (fixtures/tools) via Glaze (C++23, confined to snapshot_wire.cc
# so C++20 server_module never includes Glaze templates).
add_library(wlb_proto STATIC proto/snapshot_wire.cc)
target_include_directories(wlb_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(wlb_proto SYSTEM PRIVATE
  "${WLB_ROOT}/third_party/glaze/include")
target_link_libraries(wlb_proto PUBLIC wlb_base)
target_compile_features(wlb_proto PRIVATE cxx_std_23)
target_compile_options(wlb_proto PRIVATE ${WLB_SAFETY_FLAGS} -Wno-missing-braces)

add_executable(snapshot_check proto/snapshot_check.cc)
target_link_libraries(snapshot_check PRIVATE wlb_proto)
target_compile_options(snapshot_check PRIVATE ${WLB_SAFETY_FLAGS})

add_library(wlb_wire_py SHARED proto/snapshot_wire_c.cc)
target_link_libraries(wlb_wire_py PRIVATE wlb_proto)
target_compile_options(wlb_wire_py PRIVATE ${WLB_SAFETY_FLAGS})

# Unit tests.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${WLB_ROOT}/third_party/googletest" googletest EXCLUDE_FROM_ALL)
add_executable(wlb_common_unittests
  base/functional/callback_unittest.cc
  base/memory/raw_ptr_unittest.cc
  base/strings/strings_unittest.cc
  base/task/task_queue_unittest.cc
  base/threading/thread_unittest.cc
  base/synchronization/waitable_event_unittest.cc
  base/time/time_unittest.cc
  net/frame_codec_unittest.cc
  net/framed_connection_unittest.cc
  net/unix_socket_unittest.cc)
target_link_libraries(wlb_common_unittests PRIVATE wlb_net GTest::gtest_main)
target_compile_options(wlb_common_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# Unit tests for our AzerothCore module's AzerothCore-independent parts. They
# sit next to the code; mod-llm-brain.cmake keeps them out of the server.
set(WLB_MODULE_SRC "${WLB_ROOT}/server_module/src")
set(WLB_PROTO_ROOT "${WLB_ROOT}/server_module")
add_executable(llm_brain_unittests
  "${WLB_MODULE_SRC}/bridge/brain_bridge.cc"
  "${WLB_MODULE_SRC}/bridge/brain_bridge_unittest.cc"
  "${WLB_MODULE_SRC}/bridge/latest_wins.cc"
  "${WLB_MODULE_SRC}/bridge/latest_wins_unittest.cc"
  "${WLB_MODULE_SRC}/chat/chat_inbox.cc"
  "${WLB_MODULE_SRC}/chat/chat_inbox_unittest.cc"
  "${WLB_MODULE_SRC}/chat/chat_text.cc"
  "${WLB_MODULE_SRC}/chat/chat_text_unittest.cc"
  "${WLB_MODULE_SRC}/config/config_parsing.cc"
  "${WLB_MODULE_SRC}/config/config_parsing_unittest.cc"
  "${WLB_MODULE_SRC}/options/options.cc"
  "${WLB_MODULE_SRC}/options/options_unittest.cc"
  "${WLB_MODULE_SRC}/options/position_facts.cc"
  "${WLB_MODULE_SRC}/options/position_facts_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/perception.cc"
  "${WLB_MODULE_SRC}/snapshot/perception_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_schedule.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_schedule_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_stats.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_stats_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/spell_text.cc"
  "${WLB_MODULE_SRC}/snapshot/spell_text_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/ui_state.cc"
  "${WLB_MODULE_SRC}/snapshot/ui_state_unittest.cc"
  "${WLB_MODULE_SRC}/test/test_accounts.cc"
  "${WLB_MODULE_SRC}/test/test_accounts_unittest.cc"
  "${WLB_PROTO_ROOT}/proto/snapshot_unittest.cc")
target_include_directories(llm_brain_unittests PRIVATE "${WLB_MODULE_SRC}")
target_link_libraries(llm_brain_unittests PRIVATE wlb_net wlb_proto
                      GTest::gtest_main)
target_compile_options(llm_brain_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# Writes the module's options into saved snapshots (eval and screen fixtures).
add_executable(fixture_options "${WLB_ROOT}/server_module/tools/fixture_options.cc"
               "${WLB_MODULE_SRC}/options/options.cc")
target_include_directories(fixture_options PRIVATE "${WLB_MODULE_SRC}")
target_link_libraries(fixture_options PRIVATE wlb_base wlb_proto)
target_compile_options(fixture_options PRIVATE ${WLB_SAFETY_FLAGS})

# The harness (harness/): the brain's process. Its parts that don't need the
# game are built and tested here, with the same flags and sanitizers as the rest.
set(WLB_HARNESS_SRC "${WLB_ROOT}/harness/src")
add_library(wlb_harness STATIC "${WLB_HARNESS_SRC}/bot_loop.cc"
            "${WLB_HARNESS_SRC}/decide.cc"
            "${WLB_HARNESS_SRC}/decision_log.cc"
            "${WLB_HARNESS_SRC}/file_util.cc"
            "${WLB_HARNESS_SRC}/llm_client.cc"
            "${WLB_HARNESS_SRC}/module_connection.cc"
            "${WLB_HARNESS_SRC}/plan.cc"
            "${WLB_HARNESS_SRC}/plan_runner.cc"
            "${WLB_HARNESS_SRC}/prompt.cc"
            "${WLB_HARNESS_SRC}/run.cc"
            "${WLB_HARNESS_SRC}/scheduler.cc"
            "${WLB_HARNESS_SRC}/screen_renderer.cc"
            "${WLB_HARNESS_SRC}/triggers.cc")
target_include_directories(wlb_harness PUBLIC "${WLB_HARNESS_SRC}")
# cpp-httplib and nlohmann/json are header-only third-party code (their own style,
# so their warnings are not ours).
target_include_directories(wlb_harness SYSTEM PUBLIC
  "${WLB_ROOT}/third_party/cpp-httplib" "${WLB_ROOT}/third_party/json/include")
target_link_libraries(wlb_harness PUBLIC wlb_net wlb_proto)
target_compile_features(wlb_harness PUBLIC cxx_std_26)
target_compile_options(wlb_harness PRIVATE ${WLB_SAFETY_FLAGS})
add_executable(harness_unittests
  "${WLB_HARNESS_SRC}/bot_loop_unittest.cc"
  "${WLB_HARNESS_SRC}/decide_unittest.cc"
  "${WLB_HARNESS_SRC}/decision_log_unittest.cc"
  "${WLB_HARNESS_SRC}/file_util_unittest.cc"
  "${WLB_HARNESS_SRC}/llm_client_unittest.cc"
  "${WLB_HARNESS_SRC}/module_connection_unittest.cc"
  "${WLB_HARNESS_SRC}/plan_runner_unittest.cc"
  "${WLB_HARNESS_SRC}/plan_unittest.cc"
  "${WLB_HARNESS_SRC}/prompt_unittest.cc"
  "${WLB_HARNESS_SRC}/run_unittest.cc"
  "${WLB_HARNESS_SRC}/scheduler_unittest.cc"
  "${WLB_HARNESS_SRC}/screen_renderer_unittest.cc"
  "${WLB_HARNESS_SRC}/triggers_unittest.cc")
target_link_libraries(harness_unittests PRIVATE wlb_harness GTest::gtest_main)
target_compile_definitions(harness_unittests PRIVATE
  WLB_HARNESS_TESTDATA="${WLB_ROOT}/harness/testdata")
target_compile_options(harness_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# One name for every unit test suite: `ninja unit_tests`, run by `tools/wlb test unit`.
add_custom_target(unit_tests
  DEPENDS wlb_common_unittests llm_brain_unittests harness_unittests)

# The program: `harness decide <snapshot.pb>` (out/common/harness).
add_executable(harness "${WLB_HARNESS_SRC}/main.cc")
target_link_libraries(harness PRIVATE wlb_harness)
target_compile_options(harness PRIVATE ${WLB_SAFETY_FLAGS})
