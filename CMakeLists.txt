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
# media/jitter_buffer：cued 与 probe 共用同一份实现（两侧的「buffer」段口径必须一致，
# 否则两张图上的抖动缓冲延迟没法互相对照）。
add_executable(cued apps/cued.cpp src/media/h264_decoder.cpp src/media/encoder.cpp
  src/media/jitter_buffer.cpp)
target_link_libraries(cued PRIVATE cue_proto cue_control Boost::system Threads::Threads
  PkgConfig::FFMPEG)
target_compile_options(cued PRIVATE -Wno-deprecated-declarations)

# ---- cue-probe：下行接收端（UDP 重组 + H.264 软解 + 帧统计/端到端指标）----
# probe 复用 media/h264_decoder（不另写一个解码器）：同一份 libavcodec 封装在
# cued 与 probe 两侧的行为必须一致，否则两边的"解码耗时"没法对照。
add_executable(cue-probe apps/cue-probe.cpp src/media/h264_decoder.cpp
  src/media/jitter_buffer.cpp)
target_link_libraries(cue-probe PRIVATE cue_proto Threads::Threads PkgConfig::FFMPEG)
target_compile_options(cue-probe PRIVATE -Wno-deprecated-declarations)

# ---- 静态资源目录（供本地 http server 使用）----
add_custom_target(web ALL SOURCES web/index.html web/app.js)

# ---- 沙箱层（AC-11）----
# 独立成库：白名单 + landlock + rlimit 不依赖管线/agent，单测可以只链它，
# 不必为了测「写被拒」而把假管线搬进来。
add_library(cue_sandbox STATIC src/sandbox/sandbox.cpp)
target_link_libraries(cue_sandbox PUBLIC cue_proto Threads::Threads)

# ---- 控制通道（cued ⇄ cue 的 UDS 控制面，技术设计 §4.3）----
# 独立成库：传输层（seqpacket/stream 分帧、64 KiB 上限、id 回填）与控制语义
# （运行期参数存储、op 分发、订阅广播）都要能在没有 cued/FFmpeg 的情况下测。
# cued 与 cue_pipeline 共用同一份实现——两端各写一份，迟早有一边漏掉上限。
add_library(cue_control STATIC
  src/pipeline/control_channel.cpp
  src/pipeline/control_plane.cpp)
target_link_libraries(cue_control PUBLIC
  cue_proto nlohmann_json::nlohmann_json Threads::Threads)

# ---- 分层静态库（SPEC §7.3）----
# pipeline 只向下依赖 proto；agent 不认识 media/pipeline；eval 依赖两者。
add_library(cue_pipeline STATIC
  src/pipeline/process_control.cpp
  src/pipeline/service_pipeline.cpp
  src/pipeline/http_pipeline.cpp
  src/pipeline/recorded_pipeline.cpp
  src/pipeline/tools.cpp)
# libcurl：HttpServicePipeline（REST 后端）是 pipeline 层的一部分，链接随之公开
target_link_libraries(cue_pipeline PUBLIC
  cue_proto cue_control nlohmann_json::nlohmann_json PkgConfig::CURL Threads::Threads)
# run_shell 的实现依赖沙箱层；头文件不暴露 sandbox，故 PRIVATE
target_link_libraries(cue_pipeline PRIVATE cue_sandbox)

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
  src/eval/generator.cpp
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

# FR-EVAL.5：case 生成器的矩阵展开 / id 唯一 / 非法组合被拒 / 继承字段
# 纯函数（不读文件、不起管线），毫秒级跑完。
add_executable(test_generator tests/test_generator.cpp)
target_link_libraries(test_generator PRIVATE cue_eval)
add_test(NAME generator COMMAND test_generator)
set_tests_properties(generator PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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

# T-02/T-03：ProbeMetrics 的无样本/有样本序列化（NaN ←→ null）与 SLA 字段向后兼容
add_executable(test_probe tests/test_probe.cpp)
target_link_libraries(test_probe PRIVATE cue_eval cue_pipeline nlohmann_json::nlohmann_json)
add_test(NAME probe COMMAND test_probe)
set_tests_properties(probe PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# AC-11 沙箱：白名单 / 真实执行 / landlock 写拒绝 / 超时 / 取消 / 输出截断
add_executable(test_sandbox tests/test_sandbox.cpp)
target_link_libraries(test_sandbox PRIVATE cue_sandbox)
add_test(NAME sandbox COMMAND test_sandbox)

# media/jitter_buffer（技术设计文档 §13.1）：乱序重排 / 缺帧等待预算 / 超时跳帧 / 深度自适应
# + 关键帧保护。纯组件（无线程、不读时钟），所以直接链实现文件，不需要额外静态库。
add_executable(test_jitter_buffer tests/test_jitter_buffer.cpp src/media/jitter_buffer.cpp)
target_link_libraries(test_jitter_buffer PRIVATE cue_proto)
add_test(NAME jitter_buffer COMMAND test_jitter_buffer)

# T-05 UDS 控制通道：两种 socket 类型的往返/id 回填、坏请求不带走服务端、
# set_param 不重建编码器、超长消息被拒但连接保留、重连后 id 从 1 重新开始。
# 只链 cue_control：不需要 cued/FFmpeg，所以 ctest 里毫秒级跑完。
add_executable(test_control_channel tests/test_control_channel.cpp)
target_link_libraries(test_control_channel PRIVATE cue_control)
add_test(NAME control_channel COMMAND test_control_channel)

# T-06：FR-TOOL.1 的 8 个工具（含 lifecycle）必须真的注册在 agent 侧，
# profile 必须真的映射成 set_param。只链 cue_pipeline + cue_agent 的工具层，
# 不需要 cued/FFmpeg。
add_executable(test_tools tests/test_tools.cpp)
target_link_libraries(test_tools PRIVATE cue_pipeline cue_agent)
add_test(NAME tools COMMAND test_tools)

# T-10：延迟注入的决策序列（同一 seed 可复现、单侧均匀、注入值变化即重置）。
# 纯算术，不起 cued。
add_executable(test_delay_injector tests/test_delay_injector.cpp)
target_link_libraries(test_delay_injector PRIVATE cue_proto)
add_test(NAME delay_injector COMMAND test_delay_injector)

# T-09：NACK 令牌桶限速边界（8 包/秒、窗口 8 包）、坏窗口冷却 1 s、单包探测与恢复。
# 纯决策对象（不碰 socket/时钟），毫秒级跑完。
add_executable(test_nack tests/test_nack.cpp)
target_link_libraries(test_nack PRIVATE cue_proto)
add_test(NAME nack COMMAND test_nack)

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
  # test_sandbox 也在这个列表里：沙箱子进程现在走 execve + **显式环境白名单**
  # （src/sandbox/sandbox.cpp），不再继承 LD_PRELOAD，所以既不会出现
  # "ASan runtime does not come first"，也不会被 ASan 的 shadow 映射撞坏 RLIMIT_AS。
  # 这个改动同时修掉一个真实缺陷：之前 `run_shell(cmd="env")` 能读到 harness 的全部
  # 环境变量，包括 CUE_API_KEY。
  foreach(_t wire agent context eval state compare console cassette probe sandbox jitter_buffer
            control_channel tools delay_injector nack)
    set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${CUE_SAN_ENV}")
  endforeach()
endif()
