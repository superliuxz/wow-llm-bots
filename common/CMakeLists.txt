# Shared C++ code used by both the harness and the server module:
#   wlb_base: threads (default and epoll IO message pumps),
#             task runners, callbacks, WeakPtr, TimeDelta/TimeTicks.
#   wlb_net:  length-prefixed framing, Unix socket server/client (on IO threads).
#   wlb_snapshot: module <-> harness snapshot/command structs
#             (snapshot/snapshot.h, at the repo root) and Glaze BEVE/JSON codecs
#             (snapshot/snapshot_wire.cc).
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

# Protocol library: plain C++20 structs in snapshot/snapshot.h, serialized to BEVE
# (wire) and JSON (fixtures/tools) via Glaze (C++23, confined to snapshot_wire.cc
# so C++20 server_module never includes Glaze templates).
set(WLB_SNAPSHOT "${WLB_ROOT}/snapshot")
add_library(wlb_snapshot STATIC "${WLB_SNAPSHOT}/snapshot_wire.cc")
# The repo root: "snapshot/snapshot.h".
target_include_directories(wlb_snapshot PUBLIC "${WLB_ROOT}")
target_include_directories(wlb_snapshot SYSTEM PRIVATE
  "${WLB_ROOT}/third_party/glaze/include")
target_link_libraries(wlb_snapshot PUBLIC wlb_base)
target_compile_features(wlb_snapshot PRIVATE cxx_std_23)
target_compile_options(wlb_snapshot PRIVATE ${WLB_SAFETY_FLAGS} -Wno-missing-braces)

add_executable(snapshot_check "${WLB_SNAPSHOT}/snapshot_check.cc")
target_link_libraries(snapshot_check PRIVATE wlb_snapshot)
target_compile_options(snapshot_check PRIVATE ${WLB_SAFETY_FLAGS})

add_library(wlb_wire_py SHARED "${WLB_SNAPSHOT}/snapshot_wire_c.cc")
target_link_libraries(wlb_wire_py PRIVATE wlb_snapshot)
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
  net/unix_socket_unittest.cc
  "${WLB_SNAPSHOT}/snapshot_unittest.cc")
target_link_libraries(wlb_common_unittests PRIVATE wlb_net wlb_snapshot
                      GTest::gtest_main)
target_compile_options(wlb_common_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# Unit tests for our AzerothCore module's AzerothCore-independent parts. They
# sit next to the code; mod-llm-brain.cmake keeps them out of the server.
set(WLB_MODULE_SRC "${WLB_ROOT}/server_module/src")
add_executable(llm_brain_unittests
  "${WLB_MODULE_SRC}/bot_session.cc"
  "${WLB_MODULE_SRC}/bot_session_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/corpse_run.cc"
  "${WLB_MODULE_SRC}/bot_manager/corpse_run_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/errand_hold.cc"
  "${WLB_MODULE_SRC}/bot_manager/errand_hold_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/human_group_grace.cc"
  "${WLB_MODULE_SRC}/bot_manager/human_group_grace_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/popup_watch.cc"
  "${WLB_MODULE_SRC}/bot_manager/popup_watch_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/pull_discipline.cc"
  "${WLB_MODULE_SRC}/bot_manager/pull_discipline_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/spell_miss_packet.cc"
  "${WLB_MODULE_SRC}/bot_manager/spell_miss_packet_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/tank_swap.cc"
  "${WLB_MODULE_SRC}/bot_manager/tank_swap_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/tick_owner.cc"
  "${WLB_MODULE_SRC}/bot_manager/tick_owner_unittest.cc"
  "${WLB_MODULE_SRC}/bridge/brain_bridge.cc"
  "${WLB_MODULE_SRC}/bridge/brain_bridge_unittest.cc"
  "${WLB_MODULE_SRC}/bridge/latest_wins.cc"
  "${WLB_MODULE_SRC}/bridge/latest_wins_unittest.cc"
  "${WLB_MODULE_SRC}/chat/chat_inbox.cc"
  "${WLB_MODULE_SRC}/chat/chat_inbox_unittest.cc"
  "${WLB_MODULE_SRC}/chat/chat_text.cc"
  "${WLB_MODULE_SRC}/chat/chat_text_unittest.cc"
  "${WLB_MODULE_SRC}/chat/leader_command.cc"
  "${WLB_MODULE_SRC}/chat/leader_command_unittest.cc"
  "${WLB_MODULE_SRC}/config/config_parsing.cc"
  "${WLB_MODULE_SRC}/config/config_parsing_unittest.cc"
  "${WLB_MODULE_SRC}/options/action_types.cc"
  "${WLB_MODULE_SRC}/options/action_types_unittest.cc"
  "${WLB_MODULE_SRC}/options/act_geometry.cc"
  "${WLB_MODULE_SRC}/options/act_geometry_unittest.cc"
  "${WLB_MODULE_SRC}/options/bot_leader_guard.cc"
  "${WLB_MODULE_SRC}/options/bot_leader_guard_unittest.cc"
  "${WLB_MODULE_SRC}/options/group_roles.cc"
  "${WLB_MODULE_SRC}/options/group_roles_unittest.cc"
  "${WLB_MODULE_SRC}/options/leader_travel.cc"
  "${WLB_MODULE_SRC}/options/leader_travel_unittest.cc"
  "${WLB_MODULE_SRC}/options/loot_roll_policy.cc"
  "${WLB_MODULE_SRC}/options/loot_roll_policy_unittest.cc"
  "${WLB_MODULE_SRC}/options/npc_gossip_and_bags.cc"
  "${WLB_MODULE_SRC}/options/npc_gossip_and_bags_unittest.cc"
  "${WLB_MODULE_SRC}/options/options.cc"
  "${WLB_MODULE_SRC}/options/options_unittest.cc"
  "${WLB_MODULE_SRC}/options/position_facts.cc"
  "${WLB_MODULE_SRC}/options/position_facts_unittest.cc"
  "${WLB_MODULE_SRC}/options/quest_helpers.cc"
  "${WLB_MODULE_SRC}/options/quest_helpers_unittest.cc"
  "${WLB_MODULE_SRC}/options/riding_mounts.cc"
  "${WLB_MODULE_SRC}/options/riding_mounts_unittest.cc"
  "${WLB_MODULE_SRC}/options/vehicle_controls.cc"
  "${WLB_MODULE_SRC}/options/vehicle_controls_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/encounter_phase.cc"
  "${WLB_MODULE_SRC}/snapshot/encounter_phase_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/perception.cc"
  "${WLB_MODULE_SRC}/snapshot/perception_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/raid_meta.cc"
  "${WLB_MODULE_SRC}/snapshot/raid_meta_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_schedule.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_schedule_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_stats.cc"
  "${WLB_MODULE_SRC}/snapshot/snapshot_stats_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/behavior_trace.cc"
  "${WLB_MODULE_SRC}/snapshot/behavior_trace_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/spell_text.cc"
  "${WLB_MODULE_SRC}/snapshot/spell_text_unittest.cc"
  "${WLB_MODULE_SRC}/snapshot/ui_state.cc"
  "${WLB_MODULE_SRC}/snapshot/ui_state_unittest.cc"
  "${WLB_MODULE_SRC}/bot_manager/bot_accounts.cc"
  "${WLB_MODULE_SRC}/bot_manager/bot_accounts_unittest.cc")
target_include_directories(llm_brain_unittests PRIVATE "${WLB_MODULE_SRC}")
target_link_libraries(llm_brain_unittests PRIVATE wlb_net wlb_snapshot
                      GTest::gtest_main)
target_compile_options(llm_brain_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# Writes the module's options into saved snapshots (eval and screen fixtures).
add_executable(fixture_options "${WLB_ROOT}/server_module/tools/fixture_options.cc"
               "${WLB_MODULE_SRC}/options/action_types.cc"
               "${WLB_MODULE_SRC}/options/options.cc")
target_include_directories(fixture_options PRIVATE "${WLB_MODULE_SRC}")
target_link_libraries(fixture_options PRIVATE wlb_base wlb_snapshot)
target_compile_options(fixture_options PRIVATE ${WLB_SAFETY_FLAGS})

# The harness (harness/): the brain's process. Its parts that don't need the
# game are built and tested here, with the same flags and sanitizers as the rest.
set(WLB_HARNESS_SRC "${WLB_ROOT}/harness/src")

# The reply grammars, generated from harness/grammar/*.wlb by
# tools/grammar/wlb_gen.py (docs/specs/grammar-idl.md) into
# <build>/gen/grammar/; the generator's runtime is harness/grammar/wlb_runtime.
set(WLB_GRAMMAR_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
function(wlb_grammar wlb sources_var)
  get_filename_component(name "${wlb}" NAME_WE)
  set(out "${WLB_GRAMMAR_GEN}/grammar/${name}.wlb")
  add_custom_command(
    OUTPUT "${out}.h" "${out}.cc"
    COMMAND python3 -I -B "${WLB_ROOT}/tools/grammar/wlb_gen.py" "${wlb}"
            "${WLB_GRAMMAR_GEN}/grammar"
    DEPENDS "${WLB_ROOT}/tools/grammar/wlb_gen.py" "${wlb}"
            "${WLB_ROOT}/harness/grammar/orders.wlb"
    COMMENT "Generating ${name}.wlb.{h,cc}")
  set(${sources_var} ${${sources_var}} "${out}.cc" PARENT_SCOPE)
endfunction()
set(WLB_GRAMMAR_SOURCES)
wlb_grammar("${WLB_ROOT}/harness/grammar/orders.wlb" WLB_GRAMMAR_SOURCES)
wlb_grammar("${WLB_ROOT}/harness/grammar/plan.wlb" WLB_GRAMMAR_SOURCES)
# TEST ONLY: one of each construct, to test the generator itself.
set(WLB_GRAMMAR_TEST_SOURCES)
wlb_grammar("${WLB_ROOT}/harness/grammar/testdata/constructs.wlb"
            WLB_GRAMMAR_TEST_SOURCES)
add_library(wlb_harness STATIC "${WLB_HARNESS_SRC}/llm/llm_client.cc"
            "${WLB_HARNESS_SRC}/llm/scheduler.cc"
            "${WLB_ROOT}/harness/grammar/wlb_runtime.cc"
            "${WLB_ROOT}/harness/grammar/plan_hooks.cc"
            ${WLB_GRAMMAR_SOURCES}
            "${WLB_HARNESS_SRC}/plan/order_evaluator.cc"
            "${WLB_HARNESS_SRC}/plan/orders.cc"
            "${WLB_HARNESS_SRC}/plan/plan.cc"
            "${WLB_HARNESS_SRC}/plan/plan_runner.cc"
            "${WLB_HARNESS_SRC}/plan/triggers.cc"
            "${WLB_HARNESS_SRC}/runtime/bot_loop.cc"
            "${WLB_HARNESS_SRC}/runtime/decide.cc"
            "${WLB_HARNESS_SRC}/runtime/decision_log.cc"
            "${WLB_HARNESS_SRC}/runtime/file_util.cc"
            "${WLB_HARNESS_SRC}/runtime/module_connection.cc"
            "${WLB_HARNESS_SRC}/runtime/replay.cc"
            "${WLB_HARNESS_SRC}/runtime/run.cc"
            "${WLB_HARNESS_SRC}/screen/knowledge.cc"
            "${WLB_HARNESS_SRC}/screen/prompt.cc"
            "${WLB_HARNESS_SRC}/screen/screen_renderer.cc")
target_include_directories(wlb_harness PUBLIC "${WLB_HARNESS_SRC}"
                           "${WLB_ROOT}/harness" "${WLB_GRAMMAR_GEN}")
# cpp-httplib and nlohmann/json are header-only third-party code (their own style,
# so their warnings are not ours).
target_include_directories(wlb_harness SYSTEM PUBLIC
  "${WLB_ROOT}/third_party/cpp-httplib" "${WLB_ROOT}/third_party/json/include")
target_link_libraries(wlb_harness PUBLIC wlb_net wlb_snapshot)
target_compile_features(wlb_harness PUBLIC cxx_std_26)
target_compile_options(wlb_harness PRIVATE ${WLB_SAFETY_FLAGS})
# TEST ONLY: llama.cpp's own grammar code, to check in unit tests that a GBNF
# accepts what our printers write (harness/src/test/gbnf_check.h;
# docs/specs/grammar-idl.md, "Tests"). No model, vocabulary or GGML runtime:
# llama_grammar_stubs.cc stands in for the five functions only used with one.
# llama.cpp's files keep their own style (no warnings of theirs are ours).
set(WLB_LLAMA "${WLB_ROOT}/third_party/llama.cpp")
add_library(wlb_gbnf_check STATIC
  "${WLB_LLAMA}/src/llama-grammar.cpp"
  "${WLB_LLAMA}/src/unicode.cpp"
  "${WLB_LLAMA}/src/unicode-data.cpp"
  "${WLB_HARNESS_SRC}/test/llama_grammar_stubs.cc"
  "${WLB_HARNESS_SRC}/test/gbnf_check.cc")
target_include_directories(wlb_gbnf_check SYSTEM PRIVATE
  "${WLB_LLAMA}/include" "${WLB_LLAMA}/ggml/include" "${WLB_LLAMA}/src")
target_include_directories(wlb_gbnf_check PUBLIC "${WLB_HARNESS_SRC}")
target_compile_features(wlb_gbnf_check PRIVATE cxx_std_17)
set_source_files_properties("${WLB_LLAMA}/src/llama-grammar.cpp"
  "${WLB_LLAMA}/src/unicode.cpp" "${WLB_LLAMA}/src/unicode-data.cpp"
  PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${WLB_HARNESS_SRC}/test/llama_grammar_stubs.cc"
  "${WLB_HARNESS_SRC}/test/gbnf_check.cc"
  PROPERTIES COMPILE_OPTIONS "${WLB_SAFETY_FLAGS}")

add_executable(harness_unittests
  "${WLB_ROOT}/harness/grammar/constructs_unittest.cc"
  "${WLB_ROOT}/harness/grammar/plan_hooks_unittest.cc"
  ${WLB_GRAMMAR_TEST_SOURCES}
  "${WLB_HARNESS_SRC}/llm/llm_client_unittest.cc"
  "${WLB_HARNESS_SRC}/llm/scheduler_unittest.cc"
  "${WLB_HARNESS_SRC}/plan/order_evaluator_unittest.cc"
  "${WLB_HARNESS_SRC}/plan/orders_unittest.cc"
  "${WLB_HARNESS_SRC}/plan/plan_runner_unittest.cc"
  "${WLB_HARNESS_SRC}/plan/plan_unittest.cc"
  "${WLB_HARNESS_SRC}/plan/triggers_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/bot_loop_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/decide_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/decision_log_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/file_util_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/module_connection_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/replay_unittest.cc"
  "${WLB_HARNESS_SRC}/runtime/run_unittest.cc"
  "${WLB_HARNESS_SRC}/screen/knowledge_unittest.cc"
  "${WLB_HARNESS_SRC}/screen/prompt_unittest.cc"
  "${WLB_HARNESS_SRC}/screen/screen_renderer_unittest.cc"
  "${WLB_HARNESS_SRC}/test/gbnf_check_unittest.cc")
target_link_libraries(harness_unittests PRIVATE wlb_harness wlb_gbnf_check
                      GTest::gtest_main)
target_compile_definitions(harness_unittests PRIVATE
  WLB_HARNESS_TESTDATA="${WLB_ROOT}/harness/testdata"
  WLB_SPECS_DIR="${WLB_ROOT}/docs/specs")
target_compile_options(harness_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# One name for every unit test suite: `ninja unit_tests`, run by `tools/wlb test unit`.
add_custom_target(unit_tests
  DEPENDS wlb_common_unittests llm_brain_unittests harness_unittests)

# The program: `harness decide <snapshot.pb>` (out/common/harness).
add_executable(harness "${WLB_HARNESS_SRC}/main.cc")
target_link_libraries(harness PRIVATE wlb_harness)
target_compile_options(harness PRIVATE ${WLB_SAFETY_FLAGS})
