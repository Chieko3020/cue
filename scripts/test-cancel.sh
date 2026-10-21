#!/usr/bin/env bash
# AC-8 的可重复验证：kill -TERM cue 之后，进程与子进程是否都在 5 秒内消失。
#
# 三个场景都把进程卡在 LLM 调用里（假端点收完请求故意不回），区别是「管线在哪」：
#   llm-only       没有管线在跑：cued 起来就退出，监督线程重启预算耗尽 → 纯 LLM 阻塞
#   pipeline       真实 cued 在跑 → 取消必须把它（及其进程组）一起收掉
#   eval-endpoint  cue-eval 走 --endpoint 指向假域（HTTP 后端）→ 评测路径的取消同样要成立
#
# 断言：
#   1) 从 kill -TERM 到进程消失 < 5000 ms
#   2) 退出码 130 且（cue run）日志里有明确的「已取消」
#   3) ps 里没有 cued / cue-probe 残留
#
# 用法：
#   BUILD_DIR=build-a8 ./scripts/test-cancel.sh
#   STALL_S=60 BUILD_DIR=build-a8 ./scripts/test-cancel.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-a8}"
CUE="$ROOT/$BUILD_DIR/cue"
CUED="$ROOT/$BUILD_DIR/cued"
LIMIT_MS=5000
STALL_S="${STALL_S:-60}"
WS_PORT=9000
PROBE_PORT=9001

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

now_ms() { date +%s%3N; }

# 进程存在且不是僵尸：cue 是脚本的子进程，退出后会变僵尸直到 wait，
# 只靠 kill -0 会把「已经退出」误判成「还活着」。
is_running() {
  local st
  st="$(ps -o stat= -p "$1" 2>/dev/null || true)"
  st="${st// /}"
  [ -n "$st" ] && [ "${st:0:1}" != "Z" ]
}

descendants() { pgrep -P "$1" 2>/dev/null || true; }

wait_for_line() { # <file> <pattern> <timeout_s>
  local file="$1" pattern="$2" deadline
  deadline=$(( $(now_ms) + $3 * 1000 ))
  while [ "$(now_ms)" -lt "$deadline" ]; do
    if grep -q -- "$pattern" "$file" 2>/dev/null; then return 0; fi
    sleep 0.05
  done
  return 1
}

CUE_PID=""
LLM_PID=""
MOCK_PIDS=()
TMPDIRS=()

cleanup() {
  trap - EXIT INT TERM
  local p
  for p in "$CUE_PID" "$LLM_PID"; do
    if [ -n "$p" ]; then kill -TERM "$p" 2>/dev/null || true; fi
  done
  for p in ${MOCK_PIDS[@]+"${MOCK_PIDS[@]}"}; do
    kill -TERM "$p" 2>/dev/null || true
  done
  sleep 0.2
  for p in "$CUE_PID" "$LLM_PID"; do
    if [ -n "$p" ]; then kill -KILL "$p" 2>/dev/null || true; fi
  done
  for p in ${MOCK_PIDS[@]+"${MOCK_PIDS[@]}"}; do
    kill -KILL "$p" 2>/dev/null || true
  done
  local d
  for d in "${TMPDIRS[@]}"; do rm -rf "$d"; done
}
trap cleanup EXIT INT TERM

[ -x "$CUE" ] || fail "找不到可执行文件 $CUE（先构建 $BUILD_DIR）"
command -v python3 >/dev/null 2>&1 || fail "需要 python3（scripts/fake-llm.py）"

# 端口预检：cue run 把 cued 的 ws/probe 端口写死在 9000/9001；被占用会让
# 「管线在跑」这个场景根本起不来，报出来的失败会指向错误的方向。
for port in "$WS_PORT" "$PROBE_PORT"; do
  if ! python3 -c 'import socket,sys
s = socket.socket()
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()' "$port"; then
    fail "端口 $port 已被占用（cue run 的 cued 端口写死为 $WS_PORT/$PROBE_PORT）"
  fi
done

LLM_PORT="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"
[ -n "$LLM_PORT" ] || fail "无法分配假端点端口"

start_fake_llm() { # <log>
  : > "$1"
  python3 "$ROOT/scripts/fake-llm.py" --port "$LLM_PORT" --mode eval --stall-s "$STALL_S" \
    > "$1" 2>&1 &
  LLM_PID=$!
  wait_for_line "$1" "stall_s=" 10 || { cat "$1" >&2; fail "假端点没起来"; }
}

stop_fake_llm() {
  if [ -n "$LLM_PID" ]; then kill -TERM "$LLM_PID" 2>/dev/null || true; fi
  wait "$LLM_PID" 2>/dev/null || true
  LLM_PID=""
}

# AC-8 的判据是「ps 里没有残留的 cued/cue-probe 进程」。这里按进程名（comm）判定，
# 而不是按命令行子串：同一台机器上别人的构建会产生 `g++ ... apps/cued.cpp.o`、
# `gmake ... CMakeFiles/cued.dir/build` 这类行，它们不是残留进程，按子串判会假红。
check_no_residue() {
  local raw residual
  raw="$(ps -eo pid,comm,args | grep -E '(cued|cue-probe)' | grep -v grep || true)"
  # 两种残留形态都要抓：进程名就是 cued（execv 后 comm 变成 cued），
  # 以及由 sh 之类的解释器执行的 ./build/cued 脚本（此时 comm=sh，argv[0] 才是线索）
  residual="$(printf '%s\n' "$raw" | awk '
    $2 == "cued" || $2 == "cue-probe" { print; next }
    { n = split($3, p, "/"); if (p[n] == "cued" || p[n] == "cue-probe") print }')"
  if [ -n "$residual" ]; then
    fail "存在残留进程：
$residual"
  fi
  if [ -n "$raw" ]; then
    local noise
    noise="$(printf '%s\n' "$raw" | grep -c . || true)"
    echo "  （ps 另有 ${noise} 行命令行里带 cued/cue-probe 的非目标进程：并发构建的编译器/生成器，不计入残留）"
  fi
}

# <场景名> <pipeline|llm-only>
run_case() {
  local name="$1" mode="$2"
  local tmp
  tmp="$(mktemp -d)"
  TMPDIRS+=("$tmp")
  mkdir -p "$tmp/build"

  if [ "$mode" = pipeline ]; then
    ln -s "$CUED" "$tmp/build/cued"
  else
    # 假 cued：exec 握手成功但立刻退出 → 监督线程重启 3 次后放弃。
    # 结果是「没有管线在跑」，cue 只剩 LLM 这一条阻塞路径。
    printf '#!/bin/sh\nexit 0\n' > "$tmp/build/cued"
    chmod +x "$tmp/build/cued"
  fi

  local llm_log="$tmp/fake-llm.log"
  local cue_log="$tmp/cue.log"
  start_fake_llm "$llm_log"

  # cue run 的 cued 路径是相对 cwd 的 ./build/cued，所以在临时目录里跑
  ( cd "$tmp" && exec env CUE_API_KEY=test-key \
      CUE_BASE_URL="http://127.0.0.1:$LLM_PORT" \
      "$CUE" run --goal "取消测试：卡在 LLM 调用里" --max-turns 5 --max-wall 300 ) \
    > "$cue_log" 2>&1 &
  CUE_PID=$!

  if ! wait_for_line "$llm_log" "request received" 20; then
    cat "$cue_log" >&2 || true
    cat "$llm_log" >&2 || true
    fail "[$name] cue 没有发起 LLM 请求（日志见上）"
  fi
  sleep 0.2
  is_running "$CUE_PID" || { cat "$cue_log" >&2; fail "[$name] cue 在信号前就退出了"; }

  # 等场景成立：pipeline 场景要有活着的 cued；llm-only 场景要没有子进程
  local i
  if [ "$mode" = pipeline ]; then
    for i in $(seq 1 50); do
      [ -n "$(descendants "$CUE_PID")" ] && break
      sleep 0.1
    done
    [ -n "$(descendants "$CUE_PID")" ] || { cat "$cue_log" >&2; fail "[$name] cued 没在跑"; }
  else
    for i in $(seq 1 50); do
      [ -z "$(descendants "$CUE_PID")" ] && break
      sleep 0.1
    done
    [ -z "$(descendants "$CUE_PID")" ] || fail "[$name] 管线仍在跑，场景不成立"
  fi

  local t0 t1 elapsed rc=0
  t0="$(now_ms)"
  kill -TERM "$CUE_PID"
  while is_running "$CUE_PID"; do
    if [ $(( $(now_ms) - t0 )) -gt 15000 ]; then
      fail "[$name] kill -TERM 后 15 s 仍未退出"
    fi
    sleep 0.02
  done
  t1="$(now_ms)"
  elapsed=$(( t1 - t0 ))

  wait "$CUE_PID" 2>/dev/null || rc=$?
  CUE_PID=""

  [ "$rc" -eq 130 ] || { cat "$cue_log" >&2; fail "[$name] 退出码 $rc（期望 130）"; }
  grep -q "已取消" "$cue_log" || { cat "$cue_log" >&2; fail "[$name] 日志里没有「已取消」"; }
  # 走的是 Runner 的取消通道（run.end error=cancelled），不是 handler 之外被打穿栈
  grep -q '"error":"cancelled"' "$cue_log" \
    || { cat "$cue_log" >&2; fail "[$name] run.end 没有 error=cancelled"; }
  [ "$elapsed" -lt "$LIMIT_MS" ] || { cat "$cue_log" >&2; fail "[$name] ${elapsed} ms ≥ ${LIMIT_MS} ms"; }
  check_no_residue

  echo "[$name] SIGTERM → 进程消失：${elapsed} ms（退出码 ${rc}，cued/cue-probe 残留 0）"

  stop_fake_llm
}

# <场景名>：cue-eval 的 real 路径（--endpoint 指向假域）同样要能在 5 秒内取消。
# 这条在 ac.cancel 接线补齐之后才有意义：eval 与 cue run 共用同一个 Runner，少了那一行
# SIGTERM 后 agent 会卡到 LLM 超时（最坏 3 分钟）。顺带覆盖「HTTP 管线后端 + 取消」两条路径。
run_eval_case() {
  local name="$1" tmp mock_log mock_pid mock_port eval_log i
  tmp="$(mktemp -d)"
  TMPDIRS+=("$tmp")
  mock_log="$tmp/mock.log"
  eval_log="$tmp/eval.log"

  local eval_bin="$ROOT/$BUILD_DIR/cue-eval"
  local mock_bin="$ROOT/$BUILD_DIR/cue-mock-svc"
  [ -x "$eval_bin" ] || fail "[$name] 找不到 $eval_bin（先构建 $BUILD_DIR）"
  [ -x "$mock_bin" ] || fail "[$name] 找不到 $mock_bin（先构建 $BUILD_DIR）"

  mock_port="$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')"
  [ -n "$mock_port" ] || fail "[$name] 无法分配假域端口"

  "$mock_bin" --port "$mock_port" > "$mock_log" 2>&1 &
  mock_pid=$!
  MOCK_PIDS+=("$mock_pid")

  # 等假域真正 listen（不用 sleep 猜：能 connect 才算就绪）
  for i in $(seq 1 100); do
    if python3 -c "import socket
s = socket.socket(); s.settimeout(0.2)
try:
    s.connect(('127.0.0.1', $mock_port))
except OSError:
    raise SystemExit(1)
finally:
    s.close()" >/dev/null 2>&1; then break; fi
    sleep 0.1
  done
  kill -0 "$mock_pid" 2>/dev/null || { cat "$mock_log" >&2 || true; fail "[$name] 假域没起来"; }

  local llm_log="$tmp/fake-llm.log"
  start_fake_llm "$llm_log"

  CUE_API_KEY=test-key CUE_BASE_URL="http://127.0.0.1:$LLM_PORT" \
    "$eval_bin" --suite "$ROOT/suites/m4-diagnosis.json" --mode real --live \
    --endpoint "http://127.0.0.1:$mock_port" --warmup 0 --repeat 1 --out "$tmp/runs" \
    > "$eval_log" 2>&1 &
  CUE_PID=$!

  if ! wait_for_line "$llm_log" "request received" 30; then
    cat "$eval_log" >&2 || true
    cat "$llm_log" >&2 || true
    fail "[$name] cue-eval 没有发起 LLM 请求（日志见上）"
  fi
  sleep 0.2
  is_running "$CUE_PID" || { cat "$eval_log" >&2 || true; fail "[$name] cue-eval 在信号前就退出了"; }

  local t0 t1 elapsed rc=0
  t0="$(now_ms)"
  kill -TERM "$CUE_PID"
  while is_running "$CUE_PID"; do
    if [ $(( $(now_ms) - t0 )) -gt 15000 ]; then
      fail "[$name] kill -TERM 后 15 s 仍未退出"
    fi
    sleep 0.02
  done
  t1="$(now_ms)"
  elapsed=$(( t1 - t0 ))
  wait "$CUE_PID" 2>/dev/null || rc=$?
  CUE_PID=""

  # 130 = 被取消（与交互路径一致）：既不是「用例跑完」，也不是「用例失败」
  [ "$rc" -eq 130 ] || { cat "$eval_log" >&2 || true; fail "[$name] 退出码 $rc（期望 130）"; }
  [ "$elapsed" -lt "$LIMIT_MS" ] || { cat "$eval_log" >&2 || true; fail "[$name] ${elapsed} ms ≥ ${LIMIT_MS} ms"; }
  check_no_residue

  echo "[$name] SIGTERM → 进程消失：${elapsed} ms（退出码 ${rc}，cued/cue-probe 残留 0）"
  stop_fake_llm
}

echo "[test-cancel] BUILD_DIR=$BUILD_DIR stall=${STALL_S}s 假端点 http://127.0.0.1:$LLM_PORT"
run_case "llm-only" llm-only
run_case "pipeline" pipeline
run_eval_case "eval-endpoint"
echo "[test-cancel] PASS：三个场景都在 ${LIMIT_MS} ms 内退出且无残留"
