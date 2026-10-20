#!/usr/bin/env bash
# 一键起 M5 控制台：缺 web/dist 就先构建 React 产物，再 exec cue-serve。
#
# 用法：
#   ./scripts/serve.sh              # http://127.0.0.1:8080/（审批模式 ask）
#   ./scripts/serve.sh --approval auto
#   PORT=8081 BUILD_DIR=build-m5 ./scripts/serve.sh
#
# 环境变量：BUILD_DIR（默认 build-m5）、PORT（8080）、WS_PORT（9000）、
#           PROBE_PORT（9001）、APPROVAL（ask）、BIND（127.0.0.1）
#           NPM_REGISTRY（默认 npmmirror，国内直连 registry.npmjs.org 慢）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build-m5}"
PORT="${PORT:-8080}"
WS_PORT="${WS_PORT:-9000}"
PROBE_PORT="${PROBE_PORT:-9001}"
APPROVAL="${APPROVAL:-ask}"
BIND="${BIND:-127.0.0.1}"
NPM_REGISTRY="${NPM_REGISTRY:-https://registry.npmmirror.com}"

SERVER="$ROOT/$BUILD_DIR/cue-serve"
CUED="$ROOT/$BUILD_DIR/cued"

if [[ ! -x "$SERVER" || ! -x "$CUED" ]]; then
  cat >&2 <<EOF
[cue-serve] 缺少可执行文件（期望 $BUILD_DIR/{cue-serve,cued}）。
先构建：
  cmake -S . -B $BUILD_DIR -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build $BUILD_DIR -j1
EOF
  exit 2
fi

# React 产物：dist 不在仓库里（.gitignore），首次运行用 npm 构建一次
if [[ ! -f "$ROOT/web/dist/index.html" ]]; then
  echo "[cue-serve] web/dist 缺失，构建控制台前端（registry=$NPM_REGISTRY）…"
  (
    cd "$ROOT/web/console"
    if [[ ! -d node_modules ]]; then
      npm install --registry="$NPM_REGISTRY" --no-audit --no-fund
    fi
    npm run build
  )
fi

echo "[cue-serve] 控制台 http://$BIND:$PORT/ （审批 $APPROVAL，管线 ws=$WS_PORT udp=$PROBE_PORT）"
exec "$SERVER" \
  --bind "$BIND" \
  --port "$PORT" \
  --ws-port "$WS_PORT" \
  --probe-port "$PROBE_PORT" \
  --cued "$CUED" \
  --web "$ROOT/web/dist" \
  --runs "$ROOT/runs" \
  --approval "$APPROVAL" \
  "$@"
