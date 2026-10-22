#!/usr/bin/env bash
# Cue 一键脚本：构建后起「probe + cued + 静态页」，或跑一次脚本化 agent 演示。
#
# 用法：
#   ./scripts/run.sh              # 等价 demo-min：起媒体链路，Ctrl-C 结束
#   ./scripts/run.sh demo-min     # 同上
#   ./scripts/run.sh demo         # 构建后跑 <BUILD_DIR>/cue demo（脚本化 LLM，不联网、不花钱）；
#                                 # 只构建 cue 一个 target，报告写到 runs/demo-<时间>.txt
#
# 环境变量：BIND（静态页与 cued 的监听地址，默认 127.0.0.1）、WS_PORT、PROBE_PORT、HTTP_PORT、JOBS、
#           BUILD_DIR（构建目录，默认 build）、CXX（默认 g++-14，找不到时退回 g++）、
#           REPORT_DIR（demo 报告目录，默认 runs）、
#           CUE_PRODUCER（采集源：auto | capture | node | page，默认 auto：
#                         有 cue-capture 就用它的 lavfi 后端，没有则回退 Node 模拟端；
#                         page = 不自动产帧，由浏览器页面推流）、
#           CAPTURE_SIZE / CAPTURE_BITRATE / CAPTURE_AUDIO（cue-capture 参数）
#
# 安全口径（如实说明）：cued 有 --bind 参数，默认 127.0.0.1（仅本机）；
# 本脚本把 BIND 同时传给静态页与 cued，跨机演示时 BIND=0.0.0.0 才会对可达网络开放。
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
# 默认 -j1：本机 2 vCPU / 1.9 GiB，-j2 同时编两个 libavcodec 相关 TU 会 OOM。
# 内存更大的机器可以 JOBS=n 覆盖。
JOBS="${JOBS:-1}"
BUILD_DIR="${BUILD_DIR:-build}"
CXX="${CXX:-$(command -v g++-14 || command -v g++)}"
REPORT_DIR="${REPORT_DIR:-runs}"

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

# demo 不起监听端口，不做端口检查：否则本机 control 台占着 8080 时连离线演示都跑不了。
if [[ "$MODE" != demo ]]; then
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
fi

# CMake 不允许在已有 build 目录里原地更换编译器：缓存里若还是 g++（13），
# 直接传 g++-14 只会在 configure 阶段报一堆"变量已改变"，看不懂。先给出可执行的处置。
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  CACHED_CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Za-z]*=//p' "$BUILD_DIR/CMakeCache.txt" | head -1)"
  if [[ -n "$CACHED_CXX" ]] && [[ "$(basename "$CACHED_CXX")" != "$(basename "$CXX")" ]]; then
    echo "[cue] $BUILD_DIR 是用 $CACHED_CXX 配置的，与 CXX=$CXX 不一致；" >&2
    echo "[cue] CMake 不能原地换编译器，请先 rm -rf $BUILD_DIR 再重跑，或 CXX=$CACHED_CXX ./scripts/run.sh" >&2
    exit 1
  fi
fi

echo "[cue] build（$CXX，-j$JOBS）"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER="$CXX" >/dev/null

if [[ "$MODE" == demo ]]; then
  # demo 只需要 cue（agent 内核 + 工具层 + 沙箱），不编译 cued / FFmpeg 相关 TU / 9 个测试：
  # 全量 -j1 在本机约 7 分钟，而 demo 的 5 分钟预算（AC-17）只覆盖这一条入口。
  cmake --build "$BUILD_DIR" --target cue -j"$JOBS" 2>&1 | tail -2
  echo "[cue] demo：脚本化 LLM 跑一次完整诊断（离线，不碰网络，不启动管线）"
  mkdir -p "$REPORT_DIR"
  REPORT="$REPORT_DIR/demo-$(date +%Y%m%d-%H%M%S).txt"
  # pipefail：cue 非 0 时脚本也非 0（报告照写，便于排查）
  "$BUILD_DIR/cue" demo 2>&1 | tee "$REPORT"
  echo "[cue] 报告：$REPORT"
  exit 0
fi

cmake --build "$BUILD_DIR" -j"$JOBS" 2>&1 | tail -2

echo "[cue] probe  udp :$PROBE_PORT"
"$BUILD_DIR/cue-probe" --port "$PROBE_PORT" &
PROBE_PID=$!
PIDS+=("$PROBE_PID")

echo "[cue] cued   ws  :$WS_PORT  -> udp :$PROBE_PORT"
echo "[cue] cued bind $BIND  ws :$WS_PORT  udp :$PROBE_PORT"
"$BUILD_DIR/cued" --ws-port "$WS_PORT" --probe-port "$PROBE_PORT" --bind "$BIND" &
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

# ---- 采集源（T-11 / FR-CAP.3）----
# 默认用 cue-capture 的 lavfi 后端：零采购真实源（testsrc2 画面 + sine 音频），
# 而且走的是与浏览器**同一套**上行协议——demo-min 因此也能证明"协议不分叉"。
# 没有该二进制时回退到 Node 模拟端（旧行为，向后兼容）；两者都没有就只起管线。
PRODUCER="${CUE_PRODUCER:-auto}"
CAPTURE_PID=""
PRODUCER_NOTE=""
case "$PRODUCER" in
  auto | capture | node | page) ;;
  *)
    echo "[cue] CUE_PRODUCER=$PRODUCER 不认识（可用 auto|capture|node|page）" >&2
    exit 2
    ;;
esac
if [[ "$PRODUCER" == "page" ]]; then
  PRODUCER_NOTE="浏览器页面（不自动产帧）"
elif [[ "$PRODUCER" == "capture" || "$PRODUCER" == "auto" ]] && [[ -x "$BUILD_DIR/cue-capture" ]]; then
  "$BUILD_DIR/cue-capture" --backend lavfi \
    --ws "ws://${HOST_FOR_URL}:${WS_PORT}/" \
    --size "${CAPTURE_SIZE:-640x360}" --bitrate "${CAPTURE_BITRATE:-1200}" \
    --audio "${CAPTURE_AUDIO:-on}" --duration 0 &
  CAPTURE_PID=$!
  PIDS+=("$CAPTURE_PID")
  PRODUCER_NOTE="cue-capture（backend=lavfi：testsrc2 + sine）"
elif [[ "$PRODUCER" != "capture" ]] && command -v node >/dev/null 2>&1 && [[ -f scripts/test-client.mjs ]]; then
  # 86400 s：demo 期间一直产帧，Ctrl-C 时由 cleanup 收掉（test-client 自己按秒数退出）
  node scripts/test-client.mjs "ws://${HOST_FOR_URL}:${WS_PORT}/" 86400 &
  CAPTURE_PID=$!
  PIDS+=("$CAPTURE_PID")
  PRODUCER_NOTE="scripts/test-client.mjs（Node 模拟浏览器上行；未找到 cue-capture 的向后兼容回退）"
elif [[ "$PRODUCER" == "capture" ]]; then
  echo "[cue] CUE_PRODUCER=capture 但 $BUILD_DIR/cue-capture 不存在；先构建：" >&2
  echo "[cue]   cmake --build $BUILD_DIR --target cue-capture -j$JOBS" >&2
  exit 1
else
  PRODUCER_NOTE="无（打开下方页面点「开始推流」，或先构建 cue-capture）"
fi
echo "[cue] 采集源：$PRODUCER_NOTE"
if [[ -n "$CAPTURE_PID" ]] && ! kill -0 "$CAPTURE_PID" 2>/dev/null; then
  echo "[cue] 采集端启动失败（cue-capture / test-client 立刻退出）" >&2
  exit 1
fi
if [[ -n "$CAPTURE_PID" ]]; then
  PRODUCER_HINT="  点「开始推流」也可以，但 cued 一次只接一个客户端：当前连接已被采集端占用，"
  PRODUCER_HINT+=$'\n'"  要用浏览器推流页请 CUE_PRODUCER=page ./scripts/run.sh demo-min"
else
  PRODUCER_HINT="  点「开始推流」，观察终端里的 probe 指标"
fi
cat <<EOF

────────────────────────────────────────────────────────
  采集源：${PRODUCER_NOTE}
  Web 页：http://${HOST_FOR_URL}:${HTTP_PORT}/?ws_port=${WS_PORT}
${PRODUCER_HINT}
  Ctrl-C 结束
────────────────────────────────────────────────────────
EOF

wait
