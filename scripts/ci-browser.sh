#!/usr/bin/env bash
# Existing local dependencies only. A missing prerequisite is explicitly unexecuted.
set -euo pipefail
build="${1:-build-e2e}"
PW_NODE_MODULES="${PW_NODE_MODULES:-/tmp/pwtest/node_modules}"
status="${BROWSER_STATUS_DIR:-${RUNNER_TEMP:-/tmp}/cue-browser-${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}}"
mkdir -p "$status"
reason=''
if [ ! -x web/console/node_modules/.bin/vite ]; then
  reason='existing web/console node_modules/vite missing'
elif ! NODE_PATH="$PW_NODE_MODULES" node -e "require('playwright')" > "$status/dependency.log" 2>&1; then
  reason='existing Playwright module missing or unusable'
fi
if [ -n "$reason" ]; then
  echo "::warning::browser suites NOT EXECUTED: $reason"
  echo "NOT EXECUTED: $reason; a green job is not browser evidence" | tee "$status/skipped.txt"
  exit 0
fi
# Build failures are failures, not missing-dependency skips. Always rebuild current source.
(cd web/console && npm run build) 2>&1 | tee "$status/frontend.log"
cmake -S . -B "$build" -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O0 -DNDEBUG -g0' -DCUE_ENABLE_SDL2=ON -DCUE_SANITIZE=none
cmake --build "$build" --target cue-serve cued cue-probe -j1
NODE_PATH="$PW_NODE_MODULES" BUILD_DIR="$build" SHOT_DIR="$status/console" \
  node scripts/e2e-console.cjs 2>&1 | tee "$status/console.log"
NODE_PATH="$PW_NODE_MODULES" BUILD_DIR="$build" ARTIFACT_DIR="$status/recovery" \
  node scripts/e2e-console-recovery.cjs 2>&1 | tee "$status/recovery.log"
echo 'EXECUTED: both browser suites passed' | tee "$status/passed.txt"
