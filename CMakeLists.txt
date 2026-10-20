cmake_minimum_required(VERSION 3.20)
project(cue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CUE_WERROR "把警告当错误（CI 口径，SPEC §9）" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR
    "需要 GCC >= 14（C++23 std::expected）；当前 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
    "重新配置并显式指定编译器：cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14\n"
    "若 build/ 里已有旧编译器的缓存，先 rm -rf build（CMake 不允许原地更换编译器）")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
if(CUE_WERROR)
  add_compile_options(-Werror)
endif()

find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale x264)
if(NOT FFMPEG_FOUND)
  message(FATAL_ERROR
    "缺少 FFmpeg / x264 开发包，执行：\n"
    "  sudo apt install -y libavcodec-dev libavutil-dev libswscale-dev libx264-dev")
endif()

pkg_check_modules(CURL IMPORTED_TARGET libcurl)
if(NOT CURL_FOUND)
  message(FATAL_ERROR "缺少 libcurl 开发包，执行：\n  sudo apt install -y libcurl4-openssl-dev")
endif()

# ---- proto：纯头文件，编解码规则集中在一处 ----
add_library(cue_proto INTERFACE)
target_include_directories(cue_proto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- media 侧可执行：WS 接收 → 重组 → UDP 发送 ----
add_executable(cued apps/cued.cpp src/media/h264_decoder.cpp src/media/encoder.cpp)
target_link_libraries(cued PRIVATE cue_proto Boost::system Threads::Threads PkgConfig::FFMPEG)
target_compile_options(cued PRIVATE -Wno-deprecated-declarations)

# ---- cue-probe：下行接收端（UDP 重组 + 指标）----
add_executable(cue-probe apps/cue-probe.cpp)
target_link_libraries(cue-probe PRIVATE cue_proto Threads::Threads)

# ---- 静态资源目录（供本地 http server 使用）----
add_custom_target(web ALL SOURCES web/index.html web/app.js)

# ---- 分层静态库（SPEC §7.3）----
# pipeline 只向下依赖 proto；agent 不认识 media/pipeline；eval 依赖两者。
add_library(cue_pipeline STATIC
  src/pipeline/process_control.cpp
  src/pipeline/tools.cpp)
target_link_libraries(cue_pipeline PUBLIC cue_proto nlohmann_json::nlohmann_json Threads::Threads)

add_library(cue_agent STATIC
  src/llm/client.cpp
  src/tool/registry.cpp
  src/agent/runner.cpp
  src/common/sha256.cpp)
target_link_libraries(cue_agent PUBLIC
  cue_proto nlohmann_json::nlohmann_json PkgConfig::CURL Threads::Threads)

# 已知分层债：include/cue/eval/eval.h 直接 include pipeline/process_control.h，
# 修它需要 C++ 侧引入 IPipelineControl 接口，故 eval 只能同时依赖两层。
add_library(cue_eval STATIC
  src/eval/eval.cpp
  src/eval/state.cpp
  src/eval/compare.cpp)
target_link_libraries(cue_eval PUBLIC cue_agent cue_pipeline PkgConfig::CURL)

# SPEC §7.3：agent/tool/llm 不得 include media/ 或 pipeline/ 头文件。
# include/ 目前共用一个根目录，无法靠 target_include_directories 物理隔离，
# 所以在配置期扫描（eval 的分层债见上，暂不纳入）。
set(CUE_UPPER_LAYER_FILES)
file(GLOB_RECURSE CUE_UPPER_LAYER_FILES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cue/agent/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cue/tool/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/cue/llm/*.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/agent/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tool/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/llm/*.cpp)
foreach(_f IN LISTS CUE_UPPER_LAYER_FILES)
  file(STRINGS "${_f}" _violation REGEX "#[ \t]*include[ \t]*[<\"]cue/(media|pipeline)/")
  if(_violation)
    message(FATAL_ERROR "分层违规：${_f} include 了 media/pipeline 头文件（SPEC §7.3）")
  endif()
endforeach()

# ---- CLI ----
add_executable(cue apps/cue.cpp)
target_link_libraries(cue PRIVATE cue_agent cue_pipeline)

add_executable(cue-eval apps/cue-eval.cpp)
target_link_libraries(cue-eval PRIVATE cue_eval)

# ---- 测试 ----
enable_testing()
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire PRIVATE cue_proto)
add_test(NAME wire COMMAND test_wire)

add_executable(test_agent tests/test_agent.cpp)
target_link_libraries(test_agent PRIVATE cue_agent)
add_test(NAME agent COMMAND test_agent)

add_executable(test_context tests/test_context.cpp)
target_link_libraries(test_context PRIVATE cue_agent)
add_test(NAME context COMMAND test_context)

add_executable(test_eval tests/test_eval.cpp)
target_link_libraries(test_eval PRIVATE cue_eval)
add_test(NAME eval COMMAND test_eval)
set_tests_properties(eval PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_state tests/test_state.cpp)
target_link_libraries(test_state PRIVATE cue_eval)
add_test(NAME state COMMAND test_state)
set_tests_properties(state PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_compare tests/test_compare.cpp)
target_link_libraries(test_compare PRIVATE cue_eval)
add_test(NAME compare COMMAND test_compare)
set_tests_properties(compare PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
