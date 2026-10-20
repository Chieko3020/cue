#!/usr/bin/env bash
# Cue 一键脚本：构建后起「probe + cued + 静态页」，或跑一次脚本化 agent 演示。
#
# 用法：
#   ./scripts/run.sh              # 等价 demo-min：起媒体链路，Ctrl-C 结束
#   ./scripts/run.sh demo-min     # 同上
#   ./scripts/run.sh demo         # 构建后跑 <BUILD_DIR>/cue demo（脚本化 LLM，不联网、不花钱）
#
# 环境变量：BIND（静态页监听地址，默认 127.0.0.1）、WS_PORT、PROBE_PORT、HTTP_PORT、JOBS、
#           BUILD_DIR（构建目录，默认 build）、CXX（默认 g++-14，找不到时退回 g++）
#
# 安全口径（如实说明）：cued 目前没有 --bind 参数，WS 端口无条件 bind 0.0.0.0；
# BIND 只作用于静态页。即 WS 端口（默认 9000）对可达网络开放，跨机演示请自行限制防火墙。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="${1:-demo-min}"
case "$MODE" in
  demo-min | demo) ;;
  *)
    echo "[cue] 未知子命令：$MODE（可用：demo-min、demo）" >&2
    exit 2
    ;;
esac

BIND="${BIND:-127.0.0.1}"
WS_PORT="${WS_PORT:-9000}"
PROBE_PORT="${PROBE_PORT:-9001}"
HTTP_PORT="${HTTP_PORT:-8080}"
JOBS="${JOBS:-$(nproc)}"
BUILD_DIR="${BUILD_DIR:-build}"
CXX="${CXX:-$(command -v g++-14 || command -v g++)}"

PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  local pid
  # 只杀自己记下且仍存活的 PID；绝不能把未赋值的变量交给 kill（kill 0 = 整个进程组）
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do
    if [[ "$pid" -gt 0 ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  wait 2>/dev/null || true
  exit "$rc"
}
# trap 必须最先装：构建期间 Ctrl-C 也要能干净退出
trap cleanup EXIT INT TERM

port_busy_tcp() {
  command -v ss >/dev/null && ss -H -ltn "sport = :$1" 2>/dev/null | grep -q .
}
port_busy_udp() {
  command -v ss >/dev/null && ss -H -lun "sport = :$1" 2>/dev/null | grep -q .
}

for spec in "WS_PORT:$WS_PORT:tcp" "HTTP_PORT:$HTTP_PORT:tcp" "PROBE_PORT:$PROBE_PORT:udp"; do
  name="${spec%%:*}"
  rest="${spec#*:}"
  port="${rest%%:*}"
  proto="${rest##*:}"
  if [[ "$proto" == tcp ]] && port_busy_tcp "$port"; then
    echo "[cue] TCP :$port 已被占用（$name），先停掉占用的进程或换端口" >&2
    exit 1
  fi
  if [[ "$proto" == udp ]] && port_busy_udp "$port"; then
    echo "[cue] UDP :$port 已被占用（$name），先停掉占用的进程或换端口" >&2
    exit 1
  fi
done

# CMake 不允许在已有 build 目录里原地更换编译器：缓存里若还是 g++（13），
# 直接传 g++-14 只会在 configure 阶段报一堆"变量已改变"，看不懂。先给出可执行的处置。
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  CACHED_CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$BUILD_DIR/CMakeCache.txt" | head -1)"
  if [[ -n "$CACHED_CXX" ]] && [[ "$(basename "$CACHED_CXX")" != "$(basename "$CXX")" ]]; then
    echo "[cue] $BUILD_DIR 是用 $CACHED_CXX 配置的，与 CXX=$CXX 不一致；" >&2
    echo "[cue] CMake 不能原地换编译器，请先 rm -rf $BUILD_DIR 再重跑，或 CXX=$CACHED_CXX ./scripts/run.sh" >&2
    exit 1
  fi
fi

echo "[cue] build（$CXX）"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER="$CXX" >/dev/null
cmake --build "$BUILD_DIR" -j"$JOBS" 2>&1 | tail -2

if [[ "$MODE" == demo ]]; then
  echo "[cue] demo：脚本化 LLM 跑一次完整诊断（离线）"
  "$BUILD_DIR/cue" demo
  exit 0
fi

echo "[cue] probe  udp :$PROBE_PORT"
"$BUILD_DIR/cue-probe" --port "$PROBE_PORT" &
PROBE_PID=$!
PIDS+=("$PROBE_PID")

echo "[cue] cued   ws  :$WS_PORT  -> udp :$PROBE_PORT"
echo "[cue] 注意：cued 无 --bind，WS :$WS_PORT 监听 0.0.0.0（BIND 仅作用于静态页）"
"$BUILD_DIR/cued" --ws-port "$WS_PORT" --probe-port "$PROBE_PORT" &
MEDIA_PID=$!
PIDS+=("$MEDIA_PID")

echo "[cue] web    http :$HTTP_PORT (bind $BIND)"
python3 -m http.server "$HTTP_PORT" --bind "$BIND" --directory web >/dev/null 2>&1 &
HTTP_PID=$!
PIDS+=("$HTTP_PID")

# 后台启动失败不会触发 set -e，必须显式复核
sleep 0.3
for spec in "probe:$PROBE_PID" "cued:$MEDIA_PID" "http:$HTTP_PID"; do
  name="${spec%%:*}"
  pid="${spec#*:}"
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "[cue] $name 启动失败（端口被占或二进制缺失），已退出" >&2
    exit 1
  fi
done

HOST_FOR_URL="${BIND/0.0.0.0/127.0.0.1}"
cat <<EOF

────────────────────────────────────────────────────────
  打开  http://${HOST_FOR_URL}:${HTTP_PORT}/?ws_port=${WS_PORT}
  点「开始推流」，观察终端里的 probe 指标
  Ctrl-C 结束
────────────────────────────────────────────────────────
EOF

wait
