# Included by AzerothCore's modules/CMakeLists.txt after the `modules` target
# exists. Links our shared C++ (common/: wlb_base, wlb_net), built beforehand
# into out/common-server (not hardened) by tools/build-server.sh.
get_filename_component(WLB_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
get_filename_component(WLB_ROOT "${WLB_MODULE_DIR}/../.." REALPATH)
target_include_directories(modules PRIVATE "${WLB_ROOT}/common")
target_link_libraries(modules PRIVATE
  "${WLB_ROOT}/out/common-server/libwlb_net.a"
  "${WLB_ROOT}/out/common-server/libwlb_base.a")

# Our sources (not AzerothCore's or playerbots'): thread-safety analysis as
# errors. No _GLIBCXX_ASSERTIONS here: hardening only works for a whole
# program, and these objects link into AzerothCore binaries built without it
# (mixing let lld pull our objects into dbimport for hardened std::string
# instantiations). Hardening applies to common/ tests and the harness.
file(GLOB WLB_MODULE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.cc")
list(FILTER WLB_MODULE_SOURCES EXCLUDE REGEX "_unittest\\.cc$")
set_source_files_properties(${WLB_MODULE_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-Wthread-safety;-Werror=thread-safety")

# Unit tests sit next to the code (Chromium-style) but must not be compiled into
# the server: AzerothCore globs every .cc in a module directory, so drop them
# from the `modules` target. common/CMakeLists.txt builds them as tests.
get_target_property(WLB_MODULES_SOURCES modules SOURCES)
list(FILTER WLB_MODULES_SOURCES EXCLUDE REGEX "/mod-llm-brain/src/[^/]*_unittest\\.cc$")
set_property(TARGET modules PROPERTY SOURCES ${WLB_MODULES_SOURCES})
