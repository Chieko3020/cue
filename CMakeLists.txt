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

# ---- sanitizer（SPEC §9 / AC-12：ASan/UBSan/TSan 各一个 CI job）----
# 用法：
#   cmake -S . -B build-asan -DCMAKE_CXX_COMPILER=g++-14 -DCUE_SANITIZE=address,undefined
#   cmake -S . -B build-tsan -DCMAKE_CXX_COMPILER=g++-14 -DCUE_SANITIZE=thread
# ASan 与 UBSan 可以同时开；TSan 与它们互斥（GCC 不允许 -fsanitize=thread,address 组合，
# 运行库占位同一份 shadow 内存），所以 thread 必须单独一个构建目录。
set(CUE_SANITIZE "none" CACHE STRING
  "sanitizer：none | address | undefined | thread | address,undefined（TSan 只能单独开）")
set_property(CACHE CUE_SANITIZE PROPERTY STRINGS none address undefined thread address,undefined)

if(NOT CUE_SANITIZE STREQUAL "none")
  string(REPLACE "," ";" _cue_san_list "${CUE_SANITIZE}")
  set(_cue_san_flags)
  foreach(_s IN LISTS _cue_san_list)
    if(_s STREQUAL "address" OR _s STREQUAL "undefined" OR _s STREQUAL "leak")
      list(APPEND _cue_san_flags "-fsanitize=${_s}")
    elseif(_s STREQUAL "thread")
      list(APPEND _cue_san_flags "-fsanitize=thread")
    else()
      message(FATAL_ERROR "CUE_SANITIZE 不认识 '${_s}'（可选：none/address/undefined/thread，"
                          "ASan+UBSan 写成 address,undefined）")
    endif()
  endforeach()
  if("thread" IN_LIST _cue_san_list AND NOT CUE_SANITIZE STREQUAL "thread")
    message(FATAL_ERROR "TSan 只能单独开：-DCUE_SANITIZE=thread（不能与 address/undefined 组合）")
  endif()
  # 本机 /etc/ld.so.preload 里有 libonion.so（系统级预加载），它会被动态链接器排到
  # libasan/libtsan 之前，运行时报 "ASan runtime does not come first in initial library list"
  # 并直接 abort。这不是代码问题也不是 ASan 的检查要关掉的理由：正确的解法是把 sanitizer
  # 运行时用 LD_PRELOAD 提到最前面（ctest 的 ENVIRONMENT 里做，见文件末尾）。
  foreach(_s IN LISTS _cue_san_list)
    if(_s STREQUAL "address")
      set(_cue_san_runtime_name "libasan.so")
    elseif(_s STREQUAL "thread")
      set(_cue_san_runtime_name "libtsan.so")
    endif()
  endforeach()
  if(_cue_san_runtime_name)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_cue_san_runtime_name}
                    OUTPUT_VARIABLE _cue_san_runtime OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()

  # -fno-omit-frame-pointer：sanitizer 的栈回溯依赖帧指针，RelWithDebInfo 默认会省掉。
  # -g：没有行号时报告只能给地址，定位不到源码行。
  add_compile_options(${_cue_san_flags} -fno-omit-frame-pointer -g)
  add_link_options(${_cue_san_flags})
  # UBSan 默认「报告后继续」，那样 ctest 会绿着漏过 UB；这里让 undefined 直接终止，
  # AC-12 的「ASan/UBSan 无报告」才有可执行的判据。ASan 本身就是致命信号，无需重复。
  if("undefined" IN_LIST _cue_san_list)
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
  # sanitizer 下关闭 -Werror：第三方头（Boost/FFmpeg 宏）的告警不是本次要修的对象，
  # 让它们把真 bug 的构建挡在门外只会掩盖问题。CUE_WERROR 的常规口径不受影响。
  if(CUE_WERROR)
    message(STATUS "CUE_SANITIZE=${CUE_SANITIZE}：强制关闭 CUE_WERROR（避免告警阻塞 sanitizer 验证）")
  endif()
  set(CUE_WERROR OFF)
  message(STATUS "CUE_SANITIZE=${CUE_SANITIZE}：flags=${_cue_san_flags} -fno-omit-frame-pointer -g")
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
  src/pipeline/service_pipeline.cpp
  src/pipeline/http_pipeline.cpp
  src/pipeline/tools.cpp)
# libcurl：HttpServicePipeline（REST 后端）是 pipeline 层的一部分，链接随之公开
target_link_libraries(cue_pipeline PUBLIC
  cue_proto nlohmann_json::nlohmann_json PkgConfig::CURL Threads::Threads)

add_library(cue_agent STATIC
  src/llm/client.cpp
  src/llm/cassette.cpp
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

# ---- M5 控制台后端 ----
# 独立 target（而非 cue 的子命令）：serve 是长期驻留进程，信号/退出码语义与一次性 CLI 不同；
# 它复用 cue_pipeline（事件源）与 cue_agent（Registry/工具/审批），不另起一套管线控制。
add_executable(cue-serve apps/cue-serve.cpp)
target_link_libraries(cue-serve PRIVATE cue_agent cue_pipeline Boost::system Threads::Threads)

# ---- M7 假域服务（AC-16）----
# 只依赖 Beast + nlohmann：不链接 cue_pipeline（指标 JSON 字段是按 metrics_json 的键名
# 手写的，这样它才是"外部服务"，而不是拿被测侧的实现给自己出题），也不依赖 cued/FFmpeg。
add_executable(cue-mock-svc apps/cue-mock-svc.cpp)
target_link_libraries(cue-mock-svc PRIVATE cue_proto nlohmann_json::nlohmann_json
  Boost::system Threads::Threads)

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

# M5 控制台纯逻辑（SSE hub / 审批队列 / report.md 解析）
add_executable(test_console tests/test_console.cpp)
target_include_directories(test_console PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
target_link_libraries(test_console PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME console COMMAND test_console)
set_tests_properties(console PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# M6 cassette：key 归一化 / 录制（真实 HTTP 假端点）/ 回放 / miss
add_executable(test_cassette tests/test_cassette.cpp)
target_link_libraries(test_cassette PRIVATE cue_agent)
add_test(NAME cassette COMMAND test_cassette)

# ---- sanitizer 运行参数（ctest）----
# 报告带行号、出错即终止（不静默继续）。ASan 的泄漏检查保持默认开启：
# 若第三方全局对象造成误报，应在报告处逐条说明，而不是整体 detect_leaks=0。
if(NOT CUE_SANITIZE STREQUAL "none")
  set(CUE_SAN_ENV
    "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1"
    "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
    "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")
  # 见上文：本机 /etc/ld.so.preload 预加载了 libonion.so，必须把 sanitizer 运行时再提到它前面
  if(_cue_san_runtime AND EXISTS "${_cue_san_runtime}")
    list(APPEND CUE_SAN_ENV "LD_PRELOAD=${_cue_san_runtime}")
  endif()
  foreach(_t wire agent context eval state compare console cassette)
    set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${CUE_SAN_ENV}")
  endforeach()
endif()
