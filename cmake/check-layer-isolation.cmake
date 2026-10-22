# 分层隔离自检（FR-PIPE.15 / SPEC §7.3）——由 ctest 的 layer_isolation 调用。
#
# 为什么要有这条测试：include 目录被物理收窄之后，"越界就该编不过"这件事本身也需要
# 一个可执行判据。光在 CMake 里写 target_include_directories 只是配置；这里拿
# cue_agent / cue_eval 实际拿到的那份视图（UPPER_ROOT）真的编两段 TU：
#   1) 正例 `#include "cue/common/clock.h"`            → 必须成功（证明视图本身是好的，
#      否则反例失败可能只是参数写错，属于假绿）
#   2) 反例 `#include "cue/pipeline/process_control.h"` → 必须失败，且错误必须是
#      "No such file or directory"（而不是语法错误之类）
#   3) 反例 `#include "cue/media/encoder.h"`           → 同上
#
# 只用 -I ${UPPER_ROOT}，不引第三方 include 路径：正例故意选一个只依赖标准库的头，
# 这样这条测试在任何机器上都不依赖 nlohmann/FFmpeg 的安装位置。

set(_work ${WORK_DIR}/layer_isolation)
file(MAKE_DIRECTORY ${_work})

function(_compile snippet out_var)
  file(WRITE ${_work}/tu.cpp "${snippet}\n")
  # LC_ALL=C：断言要匹配编译器的英文报错。中文 locale 下 gcc 会把
  # "No such file or directory" 本地化，匹配不上就会被误判成"不是找不到文件"。
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C LANG=C
            ${COMPILER} -std=c++23 -fsyntax-only -I${UPPER_ROOT} ${_work}/tu.cpp
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  set(${out_var}_rc ${_rc} PARENT_SCOPE)
  set(${out_var}_err "${_err}" PARENT_SCOPE)
  set(${out_var}_out "${_out}" PARENT_SCOPE)
endfunction()

_compile("#include \"cue/common/clock.h\"\nint main() { return 0; }" positive)
if(NOT positive_rc EQUAL 0)
  message(FATAL_ERROR
    "分层隔离自检的正例失败：cue/common/clock.h 在 upper 视图里编不过。\n"
    "这说明 include 视图（${UPPER_ROOT}）本身有问题，反例结果不可信。\n${positive_err}")
endif()
message(STATUS "正例通过：cue/common/clock.h 在 upper 视图内可编译")

foreach(_hdr "cue/pipeline/process_control.h" "cue/media/encoder.h")
  _compile("#include \"${_hdr}\"\nint main() { return 0; }" bad)
  if(bad_rc EQUAL 0)
    message(FATAL_ERROR
      "分层隔离失效：${_hdr} 在 cue_agent/cue_eval 的 include 视图里能被找到并编译通过。\n"
      "视图目录：${UPPER_ROOT}")
  endif()
  if(NOT bad_err MATCHES "No such file or directory")
    message(FATAL_ERROR
      "越界 include 失败了，但原因不是「找不到文件」（可能是编译参数/头文件本身坏了）：\n${bad_err}")
  endif()
  message(STATUS "越界 include 被拒绝：${_hdr}\n${bad_err}")
endforeach()
