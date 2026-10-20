cmake_minimum_required(VERSION 3.20)
project(cue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale x264)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

# ---- proto：纯头文件，编解码规则集中在一处 ----
add_library(cue_proto INTERFACE)
target_include_directories(cue_proto INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- cued：媒体管线（WS 接收 → 重组 → UDP 发送）----
add_executable(cued apps/cued.cpp src/media/h264_decoder.cpp src/media/encoder.cpp)
target_link_libraries(cued PRIVATE cue_proto Boost::system Threads::Threads PkgConfig::FFMPEG)
target_include_directories(cued PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(cued PRIVATE -Wno-deprecated-declarations)

# ---- cue-probe：下行接收端（UDP 重组 + 指标）----
add_executable(cue-probe apps/cue-probe.cpp)
target_link_libraries(cue-probe PRIVATE cue_proto Threads::Threads)
target_include_directories(cue-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- 静态资源目录（供本地 http server 使用）----
add_custom_target(web ALL SOURCES web/index.html web/app.js)

# ---- agent 内核（M3）----
add_library(cue_core STATIC
  src/llm/client.cpp
  src/tool/registry.cpp
  src/agent/runner.cpp
  src/pipeline/process_control.cpp
  src/pipeline/tools.cpp
  src/common/sha256.cpp
  src/eval/eval.cpp
  src/eval/state.cpp)
target_link_libraries(cue_core PUBLIC cue_proto PkgConfig::CURL)
target_include_directories(cue_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(cue apps/cue.cpp)
target_link_libraries(cue PRIVATE cue_core)

add_executable(cue-eval apps/cue-eval.cpp)
target_link_libraries(cue-eval PRIVATE cue_core)

# ---- 测试 ----
enable_testing()
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire PRIVATE cue_proto)
add_test(NAME wire COMMAND test_wire)

add_executable(test_agent tests/test_agent.cpp)
target_link_libraries(test_agent PRIVATE cue_core)
add_test(NAME agent COMMAND test_agent)

add_executable(test_context tests/test_context.cpp)
target_link_libraries(test_context PRIVATE cue_core)
add_test(NAME context COMMAND test_context)

add_executable(test_eval tests/test_eval.cpp)
target_link_libraries(test_eval PRIVATE cue_core)
add_test(NAME eval COMMAND test_eval)
set_tests_properties(eval PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_state tests/test_state.cpp)
target_link_libraries(test_state PRIVATE cue_core)
add_test(NAME state COMMAND test_state)
set_tests_properties(state PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
