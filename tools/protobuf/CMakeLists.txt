# Builds protoc from third_party/protobuf against our pinned third_party/abseil-cpp
# (protobuf uses an existing absl::strings target instead of fetching its own).
cmake_minimum_required(VERSION 3.20)
project(wlb_protoc CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${REPO_ROOT}/third_party/abseil-cpp abseil EXCLUDE_FROM_ALL)

set(protobuf_BUILD_TESTS OFF)
set(protobuf_BUILD_CONFORMANCE OFF)
set(protobuf_BUILD_EXAMPLES OFF)
set(protobuf_INSTALL OFF)
set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
set(protobuf_WITH_ZLIB OFF)
add_subdirectory(${REPO_ROOT}/third_party/protobuf protobuf EXCLUDE_FROM_ALL)
