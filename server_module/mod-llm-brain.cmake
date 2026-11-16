# Included by AzerothCore's modules/CMakeLists.txt after the `modules` target
# exists. Links our shared C++ (common/: wlb_base, wlb_net, wlb_proto) and the
# protobuf lite runtime with the abseil libraries it uses, all built beforehand
# into out/common-server (not hardened) by tools/build-server.sh.
get_filename_component(WLB_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(WLB_ROOT "${WLB_MODULE_DIR}/.." REALPATH)
set(WLB_SERVER_OUT "${WLB_ROOT}/out/common-server")
target_include_directories(modules PRIVATE
  "${WLB_ROOT}/common"
  "${WLB_SERVER_OUT}/gen"
  "${WLB_ROOT}/third_party/protobuf/src"
  "${WLB_ROOT}/third_party/abseil-cpp")
# Only the protobuf / abseil libraries wlb_proto needs are built there
# (EXCLUDE_FROM_ALL), so take every archive under those directories. abseil's
# archives depend on each other in no fixed order: link them as one group.
file(GLOB_RECURSE WLB_DEP_ARCHIVES
  "${WLB_SERVER_OUT}/protobuf/*.a" "${WLB_SERVER_OUT}/abseil/*.a")
if(NOT WLB_DEP_ARCHIVES)
  message(FATAL_ERROR "mod-llm-brain: build out/common-server first "
                      "(tools/build-common.sh --server)")
endif()
target_link_libraries(modules PRIVATE
  "${WLB_SERVER_OUT}/libwlb_proto.a"
  "${WLB_SERVER_OUT}/libwlb_net.a"
  "${WLB_SERVER_OUT}/libwlb_base.a"
  -Wl,--start-group ${WLB_DEP_ARCHIVES} -Wl,--end-group)

# Our sources (not AzerothCore's or playerbots'): thread-safety analysis as
# errors. No _GLIBCXX_ASSERTIONS here: hardening only works for a whole
# program, and these objects link into AzerothCore binaries built without it
# (mixing let lld pull our objects into dbimport for hardened std::string
# instantiations). Hardening applies to common/ tests and the harness.
file(GLOB_RECURSE WLB_MODULE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.cc")
list(FILTER WLB_MODULE_SOURCES EXCLUDE REGEX "_unittest\\.cc$")
set_source_files_properties(${WLB_MODULE_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-Wthread-safety;-Werror=thread-safety")

# Unit tests sit next to the code but must not be compiled into
# the server: AzerothCore globs every .cc in a module directory, so drop them
# from the `modules` target. common/CMakeLists.txt builds them as tests.
get_target_property(WLB_MODULES_SOURCES modules SOURCES)
# Anchored on this directory, whatever name AzerothCore's modules/ link has.
# AzerothCore may list our sources through the link or the real path: drop both.
string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" WLB_DIR_RE_LINK "${CMAKE_CURRENT_LIST_DIR}")
string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" WLB_DIR_RE_REAL "${WLB_MODULE_DIR}")
set(WLB_OURS_RE "^(${WLB_DIR_RE_LINK}|${WLB_DIR_RE_REAL})/")
list(FILTER WLB_MODULES_SOURCES EXCLUDE REGEX "${WLB_OURS_RE}.*_unittest\\.cc$")

# Test-only code (src/test/, e.g. the fixture reset) is only compiled into test
# builds of the server (tools/build-server.sh --test); production binaries
# don't contain it.
option(LLM_BRAIN_TEST_HOOKS "Compile mod-llm-brain's test-only code" OFF)
if(LLM_BRAIN_TEST_HOOKS)
  target_compile_definitions(modules PRIVATE LLM_BRAIN_TEST_HOOKS)
else()
  list(FILTER WLB_MODULES_SOURCES EXCLUDE REGEX "${WLB_OURS_RE}src/test/")
endif()
set_property(TARGET modules PROPERTY SOURCES ${WLB_MODULES_SOURCES})
