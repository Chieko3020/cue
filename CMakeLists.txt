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
  # 调试信息级别用 CUE_SAN_DEBUG_FLAGS 控制，默认 -g：
  #   · -g  完整调试信息，本地开发用；
  #   · -g1 只有行表、没有局部变量与宏，仍能报「文件:行号」——CI 的 runner 有
  #          MemoryHigh=700M 的 cgroup 软限制，完整 -g 会让每个 TU 的 .o 膨胀近 20 倍
  #         （实测 0.52 MB → 9.43 MB），构建贴着限制被节流到跑不完。
  # 之所以做成变量而不是让外部用 CMAKE_CXX_FLAGS 覆盖：add_compile_options 排在后面，
  # 硬编码的 -g 会把外部传的 -g1 顶掉（这个坑实际踩过，构建日志里同时出现 -g -g1）。
  set(CUE_SAN_DEBUG_FLAGS "-g" CACHE STRING "sanitizer 构建的调试信息级别（-g 或 -g1）")
  add_compile_options(${_cue_san_flags} -fno-omit-frame-pointer ${CUE_SAN_DEBUG_FLAGS})
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
  message(STATUS "CUE_SANITIZE=${CUE_SANITIZE}：flags=${_cue_san_flags} -fno-omit-frame-pointer ${CUE_SAN_DEBUG_FLAGS}")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
if(CUE_WERROR)
  add_compile_options(-Werror)
endif()

find_package(Threads REQUIRED)
# Boost.System 从 1.69 起是 header-only：官方教程不再提供编译库，msys2 的 boost 包里
# 也没有 libboost_system*。但不少发行版（含 Ubuntu 的 libboost-system-dev）仍然带，
# 而 find_package(... COMPONENTS system REQUIRED) 在缺库的环境会直接失败。
# 所以：先按无组件找 Boost，再 QUIET 查 system 组件——有就链，没有就定义
# BOOST_ERROR_CODE_HEADER_ONLY（两种方式功能等价）。这样 Linux 与 Windows/MinGW
# 都能配置成功，且 Linux 上仍然走编译库（行为不变）。
# Windows/MinGW：Boost.Asio 的 winsock 后端需要显式链 ws2_32（MSVC 由 pragma 自动带，
# MinGW 不会）。放在 find_package(Boost) 之前用 link_libraries 全局生效——
# cued / cue-probe / cue-serve / cue-capture 都走 Beast，逐个 target 加容易漏。
if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00)  # Win10+，winsock 的 inet_pton 等需要
  link_libraries(ws2_32)
endif()

find_package(Boost 1.83 REQUIRED)
find_package(Boost 1.83 QUIET COMPONENTS system)
if(TARGET Boost::system)
  set(CUE_BOOST_SYSTEM_LIB Boost::system)
  message(STATUS "Boost ${Boost_VERSION}：使用编译版 Boost.System")
else()
  set(CUE_BOOST_SYSTEM_LIB "")
  add_compile_definitions(BOOST_ERROR_CODE_HEADER_ONLY)
  message(STATUS "Boost ${Boost_VERSION}：无 Boost.System 编译库，改用 header-only（BOOST_ERROR_CODE_HEADER_ONLY）")
endif()
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

# libswresample：采集端的音频重采样固定走 swr_convert（不手写线性插值）
pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil libswscale libswresample x264)
if(NOT FFMPEG_FOUND)
  message(FATAL_ERROR
    "缺少 FFmpeg / x264 开发包，执行：\n"
    "  sudo apt install -y libavcodec-dev libavutil-dev libswscale-dev libswresample-dev libx264-dev")
endif()

# libopus：采集端音频编码（48 kHz mono / 20 ms / 48 kbps，与浏览器 AudioEncoder 一致）
pkg_check_modules(OPUS IMPORTED_TARGET opus)
if(NOT OPUS_FOUND)
  message(FATAL_ERROR "缺少 libopus 开发包，执行：\n  sudo apt install -y libopus-dev")
endif()

pkg_check_modules(CURL IMPORTED_TARGET libcurl)
if(NOT CURL_FOUND)
  message(FATAL_ERROR "缺少 libcurl 开发包，执行：\n  sudo apt install -y libcurl4-openssl-dev")
endif()

# spdlog（FR-OBS.1：异步 sink + 文件轮转）+ 它的 fmt。
# 用系统包而不是 header-only：Ubuntu 的 libspdlog-dev 是**编译版**且 tweakme.h 强制
# SPDLOG_FMT_EXTERNAL（同包的 libfmt9），pkg-config 会带出 -DSPDLOG_COMPILED_LIB /
# -DSPDLOG_FMT_EXTERNAL / -lspdlog -lfmt。混用 header-only 会 ODR 冲突，别试。
pkg_check_modules(SPDLOG IMPORTED_TARGET spdlog)
if(NOT SPDLOG_FOUND)
  message(FATAL_ERROR "缺少 spdlog 开发包，执行：\n  sudo apt install -y libspdlog-dev libfmt-dev")
endif()

# ---- 分层 include 视图（SPEC §7.3 / FR-PIPE.15）----
# 物理隔离的难点：头文件全在 include/cue/<层>/ 下，而 `-I` 只能按**目录**粒度限制。
# 做法是在构建目录里生成"分层视图"：include-layers/<层>/cue/<子目录> 是指回真实头文件
# 目录的符号链接，target 只拿到自己这层的视图。于是 `#include "cue/pipeline/..."`
# 在 agent/tool/eval 的编译单元里根本不存在（编译期 No such file），而不是靠约定 +
# 配置期扫描。视图在构建目录里生成，不进 git、不改仓库布局。
function(cue_layer_include_root out_var layer)
  set(_root ${CMAKE_BINARY_DIR}/include-layers/${layer})
  file(MAKE_DIRECTORY ${_root}/cue)
  foreach(_dir IN LISTS ARGN)
    set(_link ${_root}/cue/${_dir})
    if(EXISTS ${_link} OR IS_SYMLINK ${_link})
      file(REMOVE ${_link})
    endif()
    file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/include/cue/${_dir} ${_link} SYMBOLIC)
  endforeach()
  set(${out_var} ${_root} PARENT_SCOPE)
endfunction()

# 「能看见全部头文件」的接口目标：可执行文件、测试、以及确实需要 media/ 的库显式链接它。
# 只有这些目标允许看见 media/ 与 pipeline/——上层（agent/tool/eval）不能链接它。
add_library(cue_headers_all INTERFACE)
target_include_directories(cue_headers_all INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 上层视图：agent / tool / eval / llm / cli 只允许 common 与彼此（+ 经 cue_proto 拿到的
# proto）。不含 media、pipeline。
cue_layer_include_root(CUE_UPPER_INCLUDE upper
  common tool agent llm eval cli)
# proto 是纯头文件层，只需要自己的目录；common 是跨层共享类型，一并给出。
cue_layer_include_root(CUE_PROTO_INCLUDE proto common)

# ---- proto：纯头文件，编解码规则集中在一处 ----
add_library(cue_proto INTERFACE)
target_include_directories(cue_proto INTERFACE ${CUE_PROTO_INCLUDE})

# ---- media 侧可执行：WS 接收 → 重组 → UDP 发送 ----
# media/jitter_buffer：cued 与 probe 共用同一份实现（两侧的「buffer」段口径必须一致，
# 否则两张图上的抖动缓冲延迟没法互相对照）。
# media/audio_decoder：上行音频（type=0x02）的 Opus 解码，PCM 只用来算指标、不下行。
# media/token_bucket：SPEC §7.2 / FR-PIPE.8 的下行限速。实际在 apps/cued.cpp 里内联过，
# N-07 抽到 media 层（纯组件、时间从参数进来），单测见 tests/test_token_bucket.cpp。
add_executable(cued apps/cued.cpp src/media/h264_decoder.cpp src/media/encoder.cpp
  src/media/jitter_buffer.cpp src/media/audio_decoder.cpp src/media/token_bucket.cpp)
target_link_libraries(cued PRIVATE cue_proto cue_control cue_headers_all ${CUE_BOOST_SYSTEM_LIB}
  Threads::Threads PkgConfig::FFMPEG PkgConfig::OPUS)
target_compile_options(cued PRIVATE -Wno-deprecated-declarations)

# ---- cue-probe：下行接收端（UDP 重组 + H.264 软解 + 帧统计/端到端指标）----
# probe 复用 media/h264_decoder（不另写一个解码器）：同一份 libavcodec 封装在
# cued 与 probe 两侧的行为必须一致，否则两边的"解码耗时"没法对照。
# 音频解码器现在是 probe 的真依赖（下行有音频了，见 apps/cued.cpp 的"音频下行"），
# 所以连 audio_decoder.cpp 与 opus 一起链进来——之前 probe 不解音频，链接里没有这些。
# 渲染/播放（"能看能听"）：SDL2 是**可选**依赖。无头环境（CI、服务器）没有它，
# probe 照常工作，只是 --render 在运行期明确报错。
option(CUE_ENABLE_SDL2 "探测 SDL2 并为 probe 启用渲染/播放（未找到时仅算指标）" ON)
find_package(PkgConfig QUIET)
if(CUE_ENABLE_SDL2 AND PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
endif()
if(CUE_ENABLE_SDL2 AND SDL2_FOUND)
  message(STATUS "SDL2 ${SDL2_VERSION}：probe 支持 --render（窗口 + 音频）")
elseif(NOT CUE_ENABLE_SDL2)
  message(STATUS "CUE_ENABLE_SDL2=OFF：probe 的 --render 不可用（只算指标）")
else()
  message(STATUS "SDL2 未找到：probe 的 --render 不可用（只算指标）")
endif()

add_executable(cue-probe apps/cue-probe.cpp apps/probe_render.cpp src/media/h264_decoder.cpp
  src/media/audio_decoder.cpp
  src/media/audio_receiver.cpp
  src/media/jitter_buffer.cpp)
target_link_libraries(cue-probe PRIVATE cue_proto cue_headers_all Threads::Threads
  PkgConfig::FFMPEG PkgConfig::OPUS)
if(CUE_ENABLE_SDL2 AND SDL2_FOUND)
  target_compile_definitions(cue-probe PRIVATE CUE_HAVE_SDL2=1)
  target_include_directories(cue-probe PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(cue-probe PRIVATE ${SDL2_LIBRARIES})
endif()
target_compile_options(cue-probe PRIVATE -Wno-deprecated-declarations)

# ---- 采集端（T-11 / FR-CAP.1–6）----
# 采集组件独立成静态库：重采样（libswresample）与 Opus（libopus）都是纯组件，
# tests/test_capture.cpp 不需要起 ffmpeg 子进程就能验证 44.1k→48k 的样本数。
# FFmpeg/Opus 随库公开链接：编码器（src/media/encoder.cpp）与 app 都直接用它们的头。
add_library(cue_capture STATIC
  src/media/audio_resampler.cpp
  src/media/opus_encoder.cpp)
target_link_libraries(cue_capture PUBLIC cue_proto PkgConfig::FFMPEG PkgConfig::OPUS)
target_link_libraries(cue_capture PRIVATE cue_headers_all)

# cue-capture：三后端（lavfi / ffmpeg 子进程 / v4l2）+ 上行 WS 客户端。
# 复用 media/encoder（X264Encoder）而不是再写一份 x264 封装——cued 的转码链与采集端
# 对同一组 x264 参数必须得到同一个码流形状，两处各写一份迟早漂移。
add_executable(cue-capture apps/cue-capture.cpp src/media/encoder.cpp)
target_link_libraries(cue-capture PRIVATE cue_capture cue_headers_all ${CUE_BOOST_SYSTEM_LIB}
  Threads::Threads)
target_compile_options(cue-capture PRIVATE -Wno-deprecated-declarations)
if(WIN32)
  target_link_libraries(cue-probe PRIVATE shell32)
  target_link_libraries(cue-capture PRIVATE shell32)
endif()

# ---- 静态资源目录（供本地 http server 使用）----
add_custom_target(web ALL SOURCES web/index.html web/app.js)

# ---- 沙箱层（AC-11）----
# 独立成库：白名单 + landlock + rlimit 不依赖管线/agent，单测可以只链它，
# 不必为了测「写被拒」而把假管线搬进来。
add_library(cue_sandbox STATIC src/sandbox/sandbox.cpp)
target_link_libraries(cue_sandbox PUBLIC cue_proto Threads::Threads)
target_link_libraries(cue_sandbox PRIVATE cue_headers_all)

# ---- 控制通道（cued ⇄ cue 的 UDS 控制面，技术设计 §4.3）----
# 独立成库：传输层（seqpacket/stream 分帧、64 KiB 上限、id 回填）与控制语义
# （运行期参数存储、op 分发、订阅广播）都要能在没有 cued/FFmpeg 的情况下测。
# cued 与 cue_pipeline 共用同一份实现——两端各写一份，迟早有一边漏掉上限。
add_library(cue_control STATIC
  src/pipeline/control_channel.cpp
  src/pipeline/control_plane.cpp
  # T-22 / FR-PIPE.12：设备会话表（注册/能力协商/租约/多设备）。放在这一层是因为
  # cued 与 cue_pipeline 都要用它，而它不依赖 FFmpeg/Beast——单测能毫秒级跑完。
  src/pipeline/session_registry.cpp)
target_link_libraries(cue_control PUBLIC
  cue_proto nlohmann_json::nlohmann_json Threads::Threads)
target_link_libraries(cue_control PRIVATE cue_headers_all)

# ---- 结构化日志（FR-OBS.1 / 技术设计 §12.1）----
# 独立成库：spdlog 的头只出现在 src/common/log.cpp 一处，别处只 include cue/common/log.h。
# cued/cue-probe 不链它——它们的 stdout 是 harness 的解析输入，保持原格式（见 log.h 头注释）。
add_library(cue_log STATIC src/common/log.cpp)
target_link_libraries(cue_log PUBLIC
  cue_proto nlohmann_json::nlohmann_json PkgConfig::SPDLOG Threads::Threads)
target_link_libraries(cue_log PRIVATE cue_headers_all)

# ---- 配置文件（FR-OBS.5）----
# 只有配置解析与校验：JSON 用 nlohmann（已是硬依赖），行号换算与合法范围都在这里。
add_library(cue_config STATIC src/common/config.cpp)
target_link_libraries(cue_config PUBLIC cue_log nlohmann_json::nlohmann_json)
target_link_libraries(cue_config PRIVATE cue_headers_all)

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
target_link_libraries(cue_pipeline PRIVATE cue_headers_all)

# 上层（agent/tool/llm）：include 视图只有 upper 子目录，看不见 media/ 与 pipeline/。
# 这不是注释约定——越界 include 在这里就是编译错误（见 ctest 的 layer_isolation）。
add_library(cue_agent STATIC
  src/llm/client.cpp
  src/llm/cassette.cpp
  src/tool/registry.cpp
  src/agent/runner.cpp
  src/agent/session.cpp
  src/common/sha256.cpp)
target_link_libraries(cue_agent PUBLIC
  cue_proto nlohmann_json::nlohmann_json PkgConfig::CURL Threads::Threads)
target_include_directories(cue_agent PUBLIC ${CUE_UPPER_INCLUDE})

# 评测层同样只拿 upper 视图：它经由 IServicePipeline / make_pipeline 使用管线，
# 不再 include pipeline/process_control.h（那是 T-20 修掉的唯一一处越界）。
# 链接 cue_pipeline 是**实现**依赖（工厂与工具层的 .cpp 在那里），不是头文件依赖。
add_library(cue_eval STATIC
  src/eval/eval.cpp
  src/eval/generator.cpp
  src/eval/state.cpp
  src/eval/compare.cpp)
target_link_libraries(cue_eval PUBLIC cue_agent cue_pipeline PkgConfig::CURL)
target_include_directories(cue_eval PUBLIC ${CUE_UPPER_INCLUDE})

# ---- CLI 选项（FR-OBS.5 的优先级规则在这里落地）----
# 拆两个库是为了不让 cue-serve 被拖上 cue_cli_eval（评测的 CLI 选项结构体里有 RunConfig，
# 那是 cue-eval 的入参，控制台不需要）。cue_eval 本身现在被 cue-serve 直接链接：
# GET /api/diff 要复用 eval::diff_runs，见下面 cue-serve 那段的说明。
add_library(cue_cli_serve STATIC src/cli/serve_options.cpp)
target_link_libraries(cue_cli_serve PUBLIC cue_config)
target_include_directories(cue_cli_serve PUBLIC ${CUE_UPPER_INCLUDE})

add_library(cue_cli_eval STATIC src/cli/eval_options.cpp)
target_link_libraries(cue_cli_eval PUBLIC cue_config cue_eval)
target_include_directories(cue_cli_eval PUBLIC ${CUE_UPPER_INCLUDE})

# SPEC §7.3 的越界检查不再靠"配置期扫源码文本"（那是约定），而是靠上面的
# include 视图：agent/tool/eval 的编译单元里根本没有 cue/media 与 cue/pipeline。
# 可执行判据是 ctest 的 layer_isolation（见文件末尾）。

# ---- CLI ----
add_executable(cue apps/cue.cpp)
target_link_libraries(cue PRIVATE cue_agent cue_pipeline cue_headers_all)

add_executable(cue-eval apps/cue-eval.cpp)
target_link_libraries(cue-eval PRIVATE cue_eval cue_cli_eval cue_headers_all)

# ---- M5 控制台后端 ----
# 独立 target（而非 cue 的子命令）：serve 是长期驻留进程，信号/退出码语义与一次性 CLI 不同；
# 它复用 cue_pipeline（事件源）与 cue_agent（Registry/工具/审批），不另起一套管线控制。
add_executable(cue-serve apps/cue-serve.cpp)
# 为什么 serve 也链接 cue_eval：GET /api/diff 要复用 eval::diff_runs。对比分类
# （run_error 算不算新增失败、指纹不同要先声明不可比）只能有一份实现——前端自己算
# 一份、cue-eval --diff 再算一份，两边迟早会漂。cue_cli_eval 仍然不链（它带 RunConfig
# 选项结构体，那是 CLI 的东西，serve 不需要）。
target_link_libraries(cue-serve PRIVATE cue_agent cue_pipeline cue_eval cue_cli_serve cue_headers_all
  ${CUE_BOOST_SYSTEM_LIB} Threads::Threads)

# ---- M7 假域服务（AC-16）----
# 只依赖 Beast + nlohmann：不链接 cue_pipeline（指标 JSON 字段是按 metrics_json 的键名
# 手写的，这样它才是"外部服务"，而不是拿被测侧的实现给自己出题），也不依赖 cued/FFmpeg。
add_executable(cue-mock-svc apps/cue-mock-svc.cpp)
target_link_libraries(cue-mock-svc PRIVATE cue_proto nlohmann_json::nlohmann_json
  ${CUE_BOOST_SYSTEM_LIB} Threads::Threads)

# ---- 测试 ----
enable_testing()
add_executable(test_wire tests/test_wire.cpp)
target_link_libraries(test_wire PRIVATE cue_proto)
add_test(NAME wire COMMAND test_wire)

# 即使常规构建装有 SDL2，也编译并验证实际的无 SDL 分支。
add_executable(test_probe_render_headless tests/test_probe_render_headless.cpp apps/probe_render.cpp)
target_include_directories(test_probe_render_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
add_test(NAME probe_render_headless COMMAND test_probe_render_headless)

add_executable(test_audio_playback tests/test_audio_playback.cpp)
target_include_directories(test_audio_playback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
add_test(NAME audio_playback COMMAND test_audio_playback)
if(CUE_ENABLE_SDL2 AND SDL2_FOUND)
  add_executable(test_probe_render_sdl tests/test_probe_render_sdl.cpp apps/probe_render.cpp)
  target_include_directories(test_probe_render_sdl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps
    ${SDL2_INCLUDE_DIRS})
  target_compile_definitions(test_probe_render_sdl PRIVATE CUE_HAVE_SDL2=1)
  target_link_libraries(test_probe_render_sdl PRIVATE cue_headers_all PkgConfig::FFMPEG
    ${SDL2_LIBRARIES})
  add_test(NAME probe_render_sdl COMMAND test_probe_render_sdl)
  set_tests_properties(probe_render_sdl PROPERTIES TIMEOUT 40)
endif()

add_executable(test_capture_process tests/test_capture_process.cpp)
target_include_directories(test_capture_process PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
target_link_libraries(test_capture_process PRIVATE cue_headers_all Threads::Threads)
if(WIN32)
  target_link_libraries(test_capture_process PRIVATE shell32)
endif()
add_test(NAME capture_process COMMAND test_capture_process)

add_executable(test_probe_network tests/test_probe_network.cpp)
target_include_directories(test_probe_network PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
target_link_libraries(test_probe_network PRIVATE Threads::Threads)
add_test(NAME probe_network COMMAND test_probe_network)
set_tests_properties(probe_network capture_process PROPERTIES TIMEOUT 30)

# proto/frame_guard（技术设计文档 §13.1 的同名行，N-08）：幽灵帧守卫的两处调用点
# （FEC 校验包 / 迟到的视频分片与 NACK 重传）抽成纯函数后，用单测钉住四条布尔入参的
# 语义——已完成帧无槽位即丢、槽位还在不丢、未来帧不丢、首帧保护（别挡 IDR）、
# 水位边界是闭区间、uint32_t 回绕时比较方向不反。纯头文件、无副作用，只编一个 TU。
add_executable(test_frame_guard tests/test_frame_guard.cpp)
target_link_libraries(test_frame_guard PRIVATE cue_proto)
add_test(NAME frame_guard COMMAND test_frame_guard)

# proto/reassembly_budget（N-10 / FR-PIPE.14）：下行的 WS 重组表此前只有 2 s TTL 与
# 单帧 kMaxFragPerFrame=1024 两道防线，**帧数**无界（每帧发 1 片、帧号递增，2 s 内能攒出
# 任意多槽位）。帧数/字节双上限的判据在这里抽成纯函数，用单测钉住边界：恰好等于上限不淘汰、
# 第 +1 帧淘汰 1 帧、多 1 字节就要淘汰、病态单帧只拒收不清队列、一次算出要淘汰的帧数。
# 纯头文件、无副作用、不碰时钟，毫秒级跑完。
add_executable(test_reassembly_budget tests/test_reassembly_budget.cpp)
target_link_libraries(test_reassembly_budget PRIVATE cue_proto)
add_test(NAME reassembly_budget COMMAND test_reassembly_budget)

add_executable(test_upstream_reassembly tests/test_upstream_reassembly.cpp)
target_link_libraries(test_upstream_reassembly PRIVATE cue_proto Threads::Threads)
add_test(NAME upstream_reassembly COMMAND test_upstream_reassembly)

add_executable(test_agent tests/test_agent.cpp)
target_link_libraries(test_agent PRIVATE cue_agent)
add_test(NAME agent COMMAND test_agent)

# T-19（FR-AGENT.5）：会话持久化与中断恢复——六类记录往返 / 尾部残行容错 / 预算续算 /
# 阶段复位 / 已结束会话拒绝续写 / 单写者锁 / 未回填 tool_call 的合成修复。
# 全是本地文件读写 + ScriptedClient，毫秒级跑完（不联网、不起管线）。
add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE cue_agent)
add_test(NAME session COMMAND test_session)

add_executable(test_session_registry tests/test_session_registry.cpp)
# session_registry.cpp 在 cue_control 里，而它的 include 是 PRIVATE（分层隔离的代价）。
# 测试要看 cue/pipeline/ 的头，得显式链 cue_headers_all —— 与其它测试同一口径。
# cue_pipeline 是为了 T-23 起的那组断言：会话表经 IServicePipeline::sessions_json
# 到达控制台，而「HTTP/回放后端必须说没有」的实现在 cue_pipeline 里
# （service_pipeline / http_pipeline / recorded_pipeline）。它不发请求、不起进程，
# 所以这里仍是一秒内跑完的纯单测。
target_link_libraries(test_session_registry PRIVATE cue_control cue_pipeline cue_headers_all)
add_test(NAME session_registry COMMAND test_session_registry)

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

# T-08 报告字段：失败模式分布 / run_notes 分离、稳定性（总体标准差）、report.md 两节、
# 终端摘要。全是纯函数（summarize_failure_modes / compute_stability / render_*），
# 不读磁盘、不起 cued，所以能在 ctest 里毫秒级跑完。
add_executable(test_report tests/test_report.cpp)
target_link_libraries(test_report PRIVATE cue_eval)
add_test(NAME report COMMAND test_report)
set_tests_properties(report PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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
# 沙箱用例用相对路径代表"项目根"（src/main.cpp 只读、runs/sandbox 可写），
# 因此必须在仓库根下跑；ctest 的默认工作目录是构建目录，那里没有 src/main.cpp。
set_tests_properties(sandbox PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# media/jitter_buffer（技术设计文档 §13.1）：乱序重排 / 缺帧等待预算 / 超时跳帧 / 深度自适应
# + 关键帧保护。纯组件（无线程、不读时钟），所以直接链实现文件，不需要额外静态库。
add_executable(test_jitter_buffer tests/test_jitter_buffer.cpp src/media/jitter_buffer.cpp)
target_link_libraries(test_jitter_buffer PRIVATE cue_proto)
add_test(NAME jitter_buffer COMMAND test_jitter_buffer)

# media/token_bucket（技术设计文档 §13.1 的同名行，N-07）：速率生效 / 非关键帧不足即丢
# （且失败不扣令牌）/ 关键帧透支上限 −burst / 先丢非关键帧 / 时间回拨不产生负增长 /
# 首次调用不按 0→now 补令牌 / set_rate 立刻生效且不重置突发、不丢令牌。
# 同样是纯组件：不读时钟、不起线程，假时钟显式传入，毫秒级跑完。
add_executable(test_token_bucket tests/test_token_bucket.cpp src/media/token_bucket.cpp)
target_link_libraries(test_token_bucket PRIVATE cue_proto)
add_test(NAME token_bucket COMMAND test_token_bucket)

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

# N-14：tool/schema 一致性。schema() 是**发给模型**的契约，而工具的真实声明在 ToolSpec 里；
# 此前 tests/ 中没有任何一处调用 .schema()，"声明的参数"与"发给模型的 schema"可以静默漂移
# （漏 desc、enum 写错、required 与 has_range 矛盾），模型侧只会拿到坏 schema。
# 这里用真实注册表（build_real_tools）造 Registry，把 schema() 与 reg.specs() 逐项比对，
# 期望值全部取自 ToolSpec 本身（不另抄一份，否则就是自证）。
# 与 test_tools 同一口径：只链 cue_pipeline + cue_agent，不起 cued/FFmpeg。
add_executable(test_tool_schema tests/test_tool_schema.cpp)
target_link_libraries(test_tool_schema PRIVATE cue_pipeline cue_agent)
add_test(NAME tool_schema COMMAND test_tool_schema)

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

# T-11：上行包头分片边界（0/16 KiB/16 KiB+1/超大）、Annex-B 起始码（3/4 字节）、
# 访问单元分帧、swr 重采样样本数、ffmpeg 子进程命令行（dshow/avfoundation 在 Linux 上也能断言）、
# Opus 20 ms 帧。全部是纯函数/纯组件，毫秒级跑完。
add_executable(test_capture tests/test_capture.cpp)
target_link_libraries(test_capture PRIVATE cue_capture)
add_test(NAME capture COMMAND test_capture)

# N-09：分辨率 → AVC level 选择表。两个边界（414720 = L3.0 上限、2097152 = L4.0 上限）
# 都是闭区间，两侧各测一条；另有常见档位、0/负尺寸的**现行为**、INT_MAX 溢出，
# 以及 pick_avc_level → avc_level_name 的文案衔接（30→"3.0"、40→"4.0"、0→"auto"）。
# 表在 media 层（include/cue/media/avc_level.h）：原来写在 apps/cue-capture.cpp 的匿名
# 命名空间里，tests/ 链接不到 app，所以这两个边界一直只有人工证据。纯头文件、一个 TU，
# 毫秒级跑完，不需要摄像头也不需要 ffmpeg。
add_executable(test_avc_level tests/test_avc_level.cpp)
add_test(NAME avc_level COMMAND test_avc_level)

# T-30（FR-DEV.6）：Opus 解码往返（440 Hz 正弦 vs 数字静音，RMS 差 > 30 dB）、损坏包被拒
# 且计入错误、空包/超长包在进解码器之前就被拒、20 ms @48k = 960 样本、50 帧连续解码可对账。
# 编码侧复用 cue_capture（同一个 OpusAudioEncoder），解码侧链新组件；
# 纯 libopus 往返，不起 ffmpeg、不需要声卡。
add_executable(test_audio_decoder tests/test_audio_decoder.cpp src/media/audio_decoder.cpp)
target_link_libraries(test_audio_decoder PRIVATE cue_capture)
add_test(NAME audio_decoder COMMAND test_audio_decoder)

add_executable(test_audio_receiver tests/test_audio_receiver.cpp src/media/audio_receiver.cpp
  src/media/audio_decoder.cpp)
target_link_libraries(test_audio_receiver PRIVATE cue_capture)
add_test(NAME audio_receiver COMMAND test_audio_receiver)
set_tests_properties(audio_receiver PROPERTIES TIMEOUT 30)

# T-31：H.264 解码器的内存契约。这条测试是为一个真实事故加的——旧实现把上一张输出帧
# 的引用留到下一次 drain 才放（而 avcodec_send_packet 在那之前就已经引用了它），于是每帧
# 漏一整个帧缓冲：1080p ≈3.1 MB/帧，cued 与 probe 都涨到 GB 级后被 cgroup OOM 杀掉。
# 功能断言在故障时**全部通过**（decoded 一直在涨、decode_errors 一直是 0），只有量 RSS
# 才抓得到，这正是它存在的理由。不依赖外部样本：内嵌一张 64x64 IDR 反复解码。
add_executable(test_h264_decoder tests/test_h264_decoder.cpp src/media/h264_decoder.cpp)
target_link_libraries(test_h264_decoder PRIVATE cue_headers_all PkgConfig::FFMPEG)
add_test(NAME h264_decoder COMMAND test_h264_decoder)

# T-15（FR-OBS.5）：配置文件解析、命令行覆盖、行号正确性、互斥项、未知键。
# 全是纯函数（临时文件读写，不起进程），失败信息里直接写出"期望第 N 行"。
add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE cue_cli_serve cue_cli_eval)
add_test(NAME config COMMAND test_config)
set_tests_properties(config PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# T-12/T-13（FR-OBS.1/3）：/metrics 的 JSON 结构与 /proc 解析，以及 JSON-line 的字段集、
# 1 万行异步不丢非 debug、级别过滤。不起 HTTP，也不依赖本机有没有起 cued。
add_executable(test_metrics tests/test_metrics.cpp)
target_include_directories(test_metrics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
target_link_libraries(test_metrics PRIVATE cue_pipeline cue_log
  nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME metrics COMMAND test_metrics)
set_tests_properties(metrics PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ---- 测试的 include 视图 ----
# 测试不属于任何一层：它们要能同时看见所有头文件（包括故意写错的越界组合，
# 那是测试的职责）。统一在这里补全，避免 20 多个 target 各写一遍。
set(CUE_TEST_TARGETS
  test_wire test_agent test_session test_context test_eval test_generator test_state test_compare
  test_report test_console test_cassette test_probe test_sandbox test_jitter_buffer
  test_token_bucket test_frame_guard test_avc_level test_reassembly_budget test_upstream_reassembly
  test_control_channel test_tools test_tool_schema test_delay_injector test_nack test_capture
  test_audio_decoder test_audio_receiver test_audio_playback test_h264_decoder test_config test_metrics)
foreach(_t IN LISTS CUE_TEST_TARGETS)
  target_link_libraries(${_t} PRIVATE cue_headers_all)
endforeach()

# 分层隔离的可执行判据（FR-PIPE.15 / SPEC §7.3）：用 cue_agent/cue_eval 实际拿到的
# include 视图（只有 upper 子目录）编译两段 TU——正例必须过、越界 include 必须失败。
# 这条测试的意义是"隔离真的存在"，而不是"配置里写了几个 -I"。
add_test(NAME layer_isolation
  COMMAND ${CMAKE_COMMAND}
    -DCOMPILER=${CMAKE_CXX_COMPILER}
    -DUPPER_ROOT=${CUE_UPPER_INCLUDE}
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-layer-isolation.cmake)

# ---- Python 驱动的两个测试（T-17a / T-18a）----
# python3 是仓库脚本的既有依赖（test-real-faults.sh、test-control-channel.py 都要它），
# 不新增依赖。找不到时只告警、不注册：ctest 少两个测试，比在没装 python3 的机器上
# 直接把 configure 打挂更符合"CI 稳定性优先"，但会明确打印缺了什么。
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  if(CUE_ENABLE_SDL2 AND SDL2_FOUND AND CUE_SANITIZE STREQUAL "none")
    add_test(NAME probe_audio_recovery
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_probe_audio_recovery.py
        --probe $<TARGET_FILE:cue-probe>)
    set_tests_properties(probe_audio_recovery PROPERTIES TIMEOUT 90)
  elseif(CUE_ENABLE_SDL2 AND SDL2_FOUND)
    message(STATUS "CUE_SANITIZE=${CUE_SANITIZE}: probe_audio_recovery runs only in regular builds")
  endif()
  # AC-13 的可执行证据：前端源码与产物里不得出现外部 URL（不依赖浏览器/网络，秒级）。
  # 与 e2e-console 的关系：e2e 缺 web/dist 或 playwright 时会跳过，这条永远在跑。
  add_test(NAME no_external_cdn
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-no-external-cdn.py)
  set_tests_properties(no_external_cdn PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  if(CUE_SANITIZE STREQUAL "none")
    # 技术设计 §13.2 的三条端到端判据（真 cued/cue-probe/cue-capture/cue-serve）。
    # 只在常规构建里跑：sanitizer 构建下 ctest 的 LD_PRELOAD（libasan/libtsan）会传给
    # 所有子进程，2 vCPU 上再叠加 ASan 的 shadow 映射与 TSan 的内存开销，跑一条 27 s 的
    # 多进程测试只会得到不稳定结果，而它测的不是内存安全（那是 unit test 的职责）。
    # ASan/TSan job 仍跑全部单测，AC-12 的证据不受影响。
    if(UNIX AND NOT APPLE)
      add_test(NAME eval_producer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eval_producer.py
          --eval $<TARGET_FILE:cue-eval> --cued $<TARGET_FILE:cued>
          --probe $<TARGET_FILE:cue-probe> --capture $<TARGET_FILE:cue-capture>)
      set_tests_properties(eval_producer PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
    add_test(NAME cued_reassembly
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cued_reassembly.py
        --cued $<TARGET_FILE:cued>)
    set_tests_properties(cued_reassembly PROPERTIES TIMEOUT 60)
    add_test(NAME integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py)
    set_tests_properties(integration PROPERTIES
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      TIMEOUT 120
      ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}")
  else()
    message(STATUS "CUE_SANITIZE=${CUE_SANITIZE}：不注册 integration（起真进程的端到端测试只在常规构建跑）")
  endif()
else()
  message(WARNING "找不到 python3：不注册 integration / no_external_cdn 测试（scripts/*.py 也需要它）")
  if(CUE_ENABLE_SDL2 AND SDL2_FOUND)
    message(WARNING "Python3 interpreter missing: probe_audio_recovery is not registered")
  endif()
endif()

# ---- Node 驱动的一致性检查（N-09）----
# 分辨率 → AVC level 有两份平行实现：C++ 的 include/cue/media/avc_level.h 与浏览器侧的
# web/app.js 的 pickCodec。C++ 那份的边界由 test_avc_level 钉住，但"两份不漂"必须
# 跨文件比较——这条测试只读源码、不启动浏览器、不发网络请求，秒级跑完；需要真 chromium
# 的才叫 e2e（scripts/e2e-*.cjs）。node 是前端构建的既有依赖，找不到时只告警、不注册，
# 与上面 python3 的处理同一口径。
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME avc_level_consistency
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-avc-level.mjs)
  set_tests_properties(avc_level_consistency PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
  message(WARNING "找不到 node：不注册 avc_level_consistency 测试（scripts/*.mjs 需要它）")
endif()

# ---- libFuzzer（可选，默认关；技术设计 §13.4 / T-18b）----
# 为什么默认关、为什么必须 clang：
#   * -fsanitize=fuzzer 是 Clang 的运行时（GCC 14 直接报 unrecognized argument），
#     libFuzzer 随编译器分发，所以这里不需要引入任何第三方库；
#   * 60 s × 3 个入口 ≈ 3 分钟，不适合放进每次 push 的 CI（另见 ci.yml 的手动 job）；
#   * 解析器是纯头文件，fuzz 目标不链接项目库，只编一个 TU。
#
# 用法：
#   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++-18 -DCUE_FUZZ=ON
#   cmake --build build-fuzz --target fuzz_decode_media -j1
#   ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 \
#     ./build-fuzz/fuzz_decode_media tests/fuzz/corpus/decode_media -max_total_time=60 -max_len=18432
#   （decode_media 的 FragmentTooLarge 分支要求输入 >16416 字节，libFuzzer 默认上限 4096 到不了；
#     本机 /etc/ld.so.preload 预加载 libonion.so 会把 libasan 挤到第二位，
#     verify_asan_link_order=0 是这台机器上唯一需要的让步，见 CMakeLists 开头的说明。）
option(CUE_FUZZ "构建 proto 的 libFuzzer 目标（需要 clang++，默认关）" OFF)
if(CUE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "CUE_FUZZ=ON 需要 clang++：libFuzzer 随 Clang 提供，GCC 的 -fsanitize= 没有 fuzzer 运行时。\n"
      "  cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCUE_FUZZ=ON\n"
      "  cmake --build build-fuzz --target fuzz_decode_media -j1")
  endif()
  # 一个 libFuzzer 进程只驱动一个 LLVMFuzzerTestOneInput，所以三个入口各一个 target。
  # ASan 抓越界读、UBSan 抓未定义算术；-fno-sanitize-recover=undefined 让 UB 直接终止，
  # 否则 libFuzzer 会把它当"正常返回"继续跑，覆盖数字好看但结论是假的。
  function(cue_add_fuzz_target target macro)
    add_executable(${target} tests/fuzz/proto_fuzz.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE ${macro}=1)
    target_compile_options(${target} PRIVATE
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
      -fno-omit-frame-pointer -O1 -g)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  endfunction()
  cue_add_fuzz_target(fuzz_decode_media CUE_FUZZ_TARGET_DECODE_MEDIA)
  cue_add_fuzz_target(fuzz_decode_datagram CUE_FUZZ_TARGET_DECODE_DATAGRAM)
  cue_add_fuzz_target(fuzz_decode_fec_header CUE_FUZZ_TARGET_FEC_HEADER)
endif()

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
  # N-12：这份名单此前漏了 frame_guard / token_bucket / reassembly_budget —— 它们是
  # N-08/N-07/N-10 新增的单测，守卫当时还没加上（守卫本身也是 N-12 前后才有的），
  # 于是 build-asan / build-tsan 各自停在旧配置上跑 20 / 10 项而没人发现。
  # 名单需覆盖全部 C++ 测试；非 C++ 脚本检查在下面显式豁免。
  set(_cue_san_tests wire agent session context eval state compare console cassette probe sandbox
                     probe_render_headless audio_playback capture_process probe_network
                     jitter_buffer token_bucket frame_guard reassembly_budget upstream_reassembly
                     control_channel tools tool_schema delay_injector nack capture config metrics
                     session_registry generator report audio_decoder audio_receiver h264_decoder avc_level)
  if(TARGET test_probe_render_sdl)
    list(APPEND _cue_san_tests probe_render_sdl)
  endif()
  foreach(_t ${_cue_san_tests})
    set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${CUE_SAN_ENV}")
  endforeach()

  # h264_decoder 有一条断言"连续解码不得让 RSS 线性增长"（防帧没被及时释放）。
  # ASan 默认 quarantine_size_mb=256 会把**已释放**的块留在隔离区、不还给分配器，于是
  # 这条断言在默认 ASan 下恒失败：实测 6000 帧后 RSS 增长 197796 kB（≈193 MB，正好落在
  # 隔离区内）；把隔离区调到 8 MB 后同一条断言只剩 1872 kB 并通过。即它测到的是隔离区
  # 大小，不是泄漏。真泄漏由 LeakSanitizer 单独报（下面仍保留 detect_leaks=1），所以这里
  # 只调小这一个测试的隔离区，既不关泄漏检测，也不放宽断言阈值。
  set(_cue_san_env_low_quarantine "${CUE_SAN_ENV}")
  list(FILTER _cue_san_env_low_quarantine EXCLUDE REGEX "^ASAN_OPTIONS=")
  set_tests_properties(h264_decoder PROPERTIES ENVIRONMENT
    "${_cue_san_env_low_quarantine};ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1:quarantine_size_mb=8")

  # 守卫：这份名单必须覆盖**所有**测试。
  # 漏一个的话，它只会在 CI 的 ASan job 里以 "ASan runtime does not come first
  # in initial library list" 失败——因为本机 /etc/ld.so.preload 预加载了 libonion，
  # 而本地开发通常不跑 ASan 构建，所以这个坑每次都由 CI 来发现（已经发生两次：
  # 第一次是 sandbox，第二次是 session_registry / generator / report / audio_decoder）。
  # 与其靠记性，不如让 configure 直接报错。
  # 豁免：不是 C++ 二进制，不加载 sanitizer 运行时，LD_PRELOAD 对它们没意义。
  #   layer_isolation 是 cmake -P 脚本测试；no_external_cdn 是 Python 脚本；
  #   avc_level_consistency 是 node 脚本（只读源码，不加载 C++ 运行时）。
  set(_cue_san_exempt layer_isolation no_external_cdn avc_level_consistency)
  get_property(_cue_all_tests DIRECTORY PROPERTY TESTS)
  set(_cue_missing "")
  foreach(_t ${_cue_all_tests})
    if(_t IN_LIST _cue_san_exempt)
      continue()
    endif()
    if(NOT _t IN_LIST _cue_san_tests)
      list(APPEND _cue_missing ${_t})
    endif()
  endforeach()
  if(_cue_missing)
    message(FATAL_ERROR
      "sanitizer 环境名单漏了这些测试：${_cue_missing}\n"
      "把它们加进上面 set(_cue_san_tests ...)：否则它们只会在 CI 的 ASan job 里"
      "以「ASan runtime does not come first」失败（本地看不出来）。")
  endif()
endif()
