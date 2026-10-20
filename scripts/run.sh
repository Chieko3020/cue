#!/usr/bin/env bash
# Cue M1：一键起「probe + cued + 静态页」，然后打开浏览器点「开始推流」
# 用法：
#   ./scripts/run.sh              # 仅本机可访问（默认）
#   BIND=0.0.0.0 ./scripts/run.sh # 允许其它机器访问（跨机演示时用，注意防火墙）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BIND="${BIND:-127.0.0.1}"
WS_PORT="${WS_PORT:-9000}"
PROBE_PORT="${PROBE_PORT:-9001}"
HTTP_PORT="${HTTP_PORT:-8080}"

echo "[cue] build"
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build -j"$(nproc)" 2>&1 | tail -2

cleanup() {
  local rc=$?
  kill "${PROBE_PID:-0}" "${MEDIA_PID:-0}" "${HTTP_PID:-0}" 2>/dev/null || true
  wait 2>/dev/null || true
  exit $rc
}
trap cleanup EXIT INT TERM

echo "[cue] probe  udp :$PROBE_PORT"
./build/cue-probe --port "$PROBE_PORT" &
PROBE_PID=$!

echo "[cue] cued   ws  :$WS_PORT  -> udp :$PROBE_PORT"
./build/cued --ws-port "$WS_PORT" --probe-port "$PROBE_PORT" &
MEDIA_PID=$!

echo "[cue] web    http :$HTTP_PORT (bind $BIND)"
python3 -m http.server "$HTTP_PORT" --bind "$BIND" --directory web >/dev/null 2>&1 &
HTTP_PID=$!

sleep 0.8
cat <<EOF

────────────────────────────────────────────────────────
  打开  http://${BIND/0.0.0.0/127.0.0.1}:${HTTP_PORT}/
  点「开始推流」，观察终端里的 probe 指标
  Ctrl-C 结束
────────────────────────────────────────────────────────
EOF

wait
