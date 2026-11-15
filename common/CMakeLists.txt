# Shared C++ code used by both the harness and the server module:
#   wlb_base: threads (default and epoll IO message pumps),
#             task runners, callbacks, WeakPtr, TimeDelta/TimeTicks.
#   wlb_net:  length-prefixed framing, Unix socket server/client (on IO threads).
#   wlb_proto: C++ generated from server_module/mod-llm-brain/proto/*.proto
#             (protobuf lite runtime; protobuf and abseil from third_party/).
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
# hardened) is built with WLB_HARDENED=OFF (tools/build-server.sh).
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

# Protobuf (lite runtime) and its abseil dependency, from the pinned submodules,
# built with this build's flags (sanitizers need every byte instrumented).
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
add_subdirectory("${WLB_ROOT}/third_party/abseil-cpp" abseil EXCLUDE_FROM_ALL)
set(protobuf_BUILD_TESTS OFF)
set(protobuf_BUILD_CONFORMANCE OFF)
set(protobuf_BUILD_EXAMPLES OFF)
set(protobuf_BUILD_PROTOC_BINARIES OFF)
set(protobuf_BUILD_LIBUPB OFF)
set(protobuf_INSTALL OFF)
set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
set(protobuf_WITH_ZLIB OFF)
add_subdirectory("${WLB_ROOT}/third_party/protobuf" protobuf EXCLUDE_FROM_ALL)

# Generated code comes from protoc built separately (tools/build-protoc.sh) from
# the same pinned protobuf, so generator and runtime versions always match.
set(WLB_PROTOC "${WLB_ROOT}/out/protoc/protobuf/protoc" CACHE FILEPATH "protoc")
set(WLB_PROTO_ROOT "${WLB_ROOT}/server_module/mod-llm-brain")
set(WLB_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_custom_command(
  OUTPUT "${WLB_PROTO_OUT}/proto/snapshot.pb.cc"
         "${WLB_PROTO_OUT}/proto/snapshot.pb.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${WLB_PROTO_OUT}"
  COMMAND "${WLB_PROTOC}" "-I${WLB_PROTO_ROOT}"
          "-I${WLB_ROOT}/third_party/protobuf/src"
          "--cpp_out=${WLB_PROTO_OUT}" proto/snapshot.proto
  WORKING_DIRECTORY "${WLB_PROTO_ROOT}"
  DEPENDS "${WLB_PROTO_ROOT}/proto/snapshot.proto"
          "${WLB_PROTO_ROOT}/proto/snapshot_options.proto" "${WLB_PROTOC}"
  COMMENT "protoc proto/snapshot.proto")
add_library(wlb_proto STATIC "${WLB_PROTO_OUT}/proto/snapshot.pb.cc")
target_include_directories(wlb_proto PUBLIC "${WLB_PROTO_OUT}")
target_link_libraries(wlb_proto PUBLIC protobuf::libprotobuf-lite)

# Unit tests.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory("${WLB_ROOT}/third_party/googletest" googletest EXCLUDE_FROM_ALL)
add_executable(wlb_common_unittests
  base/functional/callback_unittest.cc
  base/memory/raw_ptr_unittest.cc
  base/task/task_queue_unittest.cc
  base/threading/thread_unittest.cc
  base/synchronization/waitable_event_unittest.cc
  base/time/time_unittest.cc
  net/frame_codec_unittest.cc
  net/unix_socket_unittest.cc)
target_link_libraries(wlb_common_unittests PRIVATE wlb_net GTest::gtest_main)
target_compile_options(wlb_common_unittests PRIVATE ${WLB_SAFETY_FLAGS})

# Unit tests for our AzerothCore module's AzerothCore-independent parts. They
# sit next to the code; mod-llm-brain.cmake keeps them out of the server.
set(WLB_MODULE_SRC "${WLB_ROOT}/server_module/mod-llm-brain/src")
add_executable(llm_brain_unittests
  "${WLB_MODULE_SRC}/brain_bridge.cc"
  "${WLB_MODULE_SRC}/brain_bridge_unittest.cc"
  "${WLB_MODULE_SRC}/config_parsing.cc"
  "${WLB_MODULE_SRC}/config_parsing_unittest.cc"
  "${WLB_PROTO_ROOT}/proto/snapshot_unittest.cc")
target_include_directories(llm_brain_unittests PRIVATE "${WLB_MODULE_SRC}")
target_link_libraries(llm_brain_unittests PRIVATE wlb_net wlb_proto
                      GTest::gtest_main)
target_compile_options(llm_brain_unittests PRIVATE ${WLB_SAFETY_FLAGS})
