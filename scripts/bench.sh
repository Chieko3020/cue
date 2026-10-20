#!/usr/bin/env bash
# Cue benchmark：六段耗时分位、端到端延迟分位、5% 丢包下的帧完成率与 FEC/NACK 恢复率。
#
# 为什么用 shell 驱动 + 一个小 Node 客户端，而不是 apps/cue-bench.cpp：
#   1. 被测量的是「已经跑起来的 cued + probe 进程」，bench 只负责起停与采样，
#      把测量逻辑编进 C++ 反而会出现「bench 与被测进程抢 CPU」的干扰（本机 2 vCPU）；
#   2. 六段分位/端到端分位都是被测进程自己打印的（cued 的 METRICS 行、probe 的
#      probe.metric 事件），bench 只做解析，不重算、不估算，数字可逐行复核；
#   3. 丢包注入必须走 cued 真实的 --inject-drop（分片后、sendto 前的确定性 splitmix64），
#      用 C++ 另写一份注入器就测不到同一条代码路径了。
#
# 用法：
#   scripts/bench.sh                          # 默认 build-ci，每场景 12 s
#   BUILD_DIR=build-ci SECONDS_PER_RUN=20 scripts/bench.sh
#   SKIP_LOSS=1 scripts/bench.sh              # 只跑无故障两档
#
# 输出：bench/<timestamp>.md（Markdown 表格）+ bench/<timestamp>/raw/*.log（原始日志）
#       bench/ 已被 .gitignore 忽略（数据不入库），脚本本身入库。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ci}"
SECONDS_PER_RUN="${SECONDS_PER_RUN:-12}"
FPS="${FPS:-30}"
DROP_PCT="${DROP_PCT:-5}"
SETTLE_SECS="${SETTLE_SECS:-3}"
PORTS_BASE="${PORTS_BASE:-9400}"
WS_PORT="$PORTS_BASE"
PROBE_PORT=$((PORTS_BASE + 1))
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_ROOT="${OUT_ROOT:-$ROOT/bench}"
RUN_DIR="$OUT_ROOT/$STAMP"
RAW="$RUN_DIR/raw"
SAMPLE="${SAMPLE:-/tmp/cue-bench/sample.h264}"
SKIP_LOSS="${SKIP_LOSS:-0}"

die() { echo "[bench] $*" >&2; exit 1; }

[ -x "$BUILD_DIR/cued" ] || die "找不到 $BUILD_DIR/cued（先构建：cmake -S . -B build-ci -DCMAKE_CXX_COMPILER=g++-14 && cmake --build build-ci -j1）"
[ -x "$BUILD_DIR/cue-probe" ] || die "找不到 $BUILD_DIR/cue-probe"
command -v node >/dev/null || die "需要 node"
command -v ffmpeg >/dev/null || die "需要 ffmpeg（生成测试码流）"

mkdir -p "$RAW"

# ---- 测试码流：真实 x264 编码，每帧数 KB → 多个下行数据报，FEC/NACK 才有意义 ----
# aud=1：每个访问单元带 AUD，客户端据此切帧（只靠 start code 无法区分 slice 与帧边界）。
if [ ! -s "$SAMPLE" ] || [ "${REGEN_SAMPLE:-0}" = "1" ]; then
  mkdir -p "$(dirname "$SAMPLE")"
  echo "[bench] 生成测试码流 $SAMPLE"
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=640x360:rate=30:duration=2" \
    -c:v libx264 -profile:v main -pix_fmt yuv420p -preset veryfast -tune zerolatency \
    -b:v 1500k -maxrate 1500k -bufsize 3000k -g 30 -keyint_min 30 -sc_threshold 0 \
    -x264-params "aud=1:sliced-threads=0" -f h264 "$SAMPLE" \
    || die "ffmpeg 生成样本失败"
fi
[ -s "$SAMPLE" ] || die "样本为空: $SAMPLE"

PIDS=()
cleanup() {
  for p in "${PIDS[@]:-}"; do kill -TERM "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
}
trap cleanup EXIT

port_listening() {  # $1=port $2=t(u|t)
  ss -ln"$2" 2>/dev/null | awk '{print $4}' | grep -qE "[:.]$1\$"
}

wait_port() {  # $1=port $2=tu
  for _ in $(seq 1 100); do
    port_listening "$1" "$2" && return 0
    sleep 0.1
  done
  return 1
}

# 场景：label|transcode|fec|nack|注入丢包%
SCENARIOS=(
  "pass-plain|off|on|on|0"
  "pass-transcode|on|on|on|0"
)
if [ "$SKIP_LOSS" != "1" ]; then
  SCENARIOS+=(
    "loss-none|off|off|off|$DROP_PCT"
    "loss-fec|off|on|off|$DROP_PCT"
    "loss-nack|off|off|on|$DROP_PCT"
    "loss-fec-nack|off|on|on|$DROP_PCT"
  )
fi

run_scenario() {
  local label="$1" transcode="$2" fec="$3" nack="$4" drop="$5"
  local cued_log="$RAW/$label.cued.log"
  local probe_log="$RAW/$label.probe.log"
  local client_log="$RAW/$label.client.log"
  local client_json="$RAW/$label.client.json"

  if port_listening "$WS_PORT" t || port_listening "$PROBE_PORT" u; then
    die "端口 $WS_PORT/$PROBE_PORT 已被占用（换 PORTS_BASE 或先停掉旧进程）"
  fi

  : > "$cued_log"; : > "$probe_log"
  "$BUILD_DIR/cue-probe" --port "$PROBE_PORT" --nack "$nack" >"$probe_log" 2>&1 &
  local probe_pid=$!; PIDS+=("$probe_pid")
  wait_port "$PROBE_PORT" u || die "probe 没在 ${PROBE_PORT} 上监听（见 $probe_log）"

  local cued_cmd=("$BUILD_DIR/cued" --ws-port "$WS_PORT" --probe-port "$PROBE_PORT"
                  --transcode "$transcode" --fec "$fec" --fps 30 --bitrate 1500)
  if [ "$drop" != "0" ]; then
    cued_cmd+=(--inject-drop "$drop" --inject-seed 42 --inject-trace "$RAW/$label.inject.trace")
  fi
  "${cued_cmd[@]}" >"$cued_log" 2>&1 &
  local cued_pid=$!; PIDS+=("$cued_pid")
  wait_port "$WS_PORT" t || die "cued 没在 ${WS_PORT} 上监听（见 $cued_log）"

  echo "[bench] $label: transcode=$transcode fec=$fec nack=$nack drop=${drop}% → ${SECONDS_PER_RUN}s"
  node "$ROOT/scripts/bench-client.cjs" "ws://127.0.0.1:$WS_PORT/" "$SAMPLE" "$SECONDS_PER_RUN" \
    --fps "$FPS" --json "$client_json" >"$client_log" 2>&1 \
    || die "客户端失败（见 $client_log）"

  # 结算窗口：probe 对「2 s 内没凑齐」的帧才判丢，立刻杀掉会把在途帧算成已完成
  sleep "$SETTLE_SECS"

  kill -TERM "$cued_pid" 2>/dev/null || true
  wait "$cued_pid" 2>/dev/null || true
  kill -TERM "$probe_pid" 2>/dev/null || true
  wait "$probe_pid" 2>/dev/null || true
  PIDS=()

  grep -q "METRICS " "$cued_log" || echo "[bench] 警告：$label 没有 METRICS 行" >&2
  grep -q "\[probe\] fps=" "$probe_log" || echo "[bench] 警告：$label 没有 probe 指标行" >&2
}

for sc in "${SCENARIOS[@]}"; do
  IFS='|' read -r label transcode fec nack drop <<< "$sc"
  run_scenario "$label" "$transcode" "$fec" "$nack" "$drop"
done

# ---- run.json：环境 + 场景清单（样本尺寸从客户端摘要里取，客户端真的读了这个文件）----
python3 - "$RUN_DIR" "$STAMP" "$BUILD_DIR" "$SECONDS_PER_RUN" "$FPS" "$SETTLE_SECS" "$DROP_PCT" "$SAMPLE" \
  "${SCENARIOS[@]}" <<'PY'
import json, os, platform, subprocess, sys
run_dir, stamp, build_dir, secs, fps, settle, drop_pct, sample = sys.argv[1:9]
scen_args = sys.argv[9:]
first = json.load(open(os.path.join(run_dir, 'raw', scen_args[0].split('|')[0] + '.client.json')))
scenarios = []
for s in scen_args:
    label, tc, fec, nack, drop = s.split('|')
    scenarios.append({'label': label, 'transcode': tc, 'fec': fec == 'on',
                      'nack': nack == 'on', 'drop_pct': float(drop)})
mem_gib = round(os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / 2**30, 1)
compiler = subprocess.run(['g++-14', '--version'], capture_output=True, text=True).stdout.splitlines()[0].split(') ')[-1]
meta = {
    'stamp': stamp, 'iso': subprocess.run(['date', '-Is'], capture_output=True, text=True).stdout.strip(),
    'host': platform.node(), 'nproc': os.cpu_count(), 'mem_gib': mem_gib,
    'build_dir': build_dir, 'build_type': 'RelWithDebInfo', 'compiler': compiler,
    'seconds_per_run': float(secs), 'fps': float(fps), 'settle_secs': float(settle),
    'drop_pct_for_lossy': float(drop_pct),
    'sample': {'name': os.path.basename(sample), 'width': 640, 'height': 360,
               'fps': float(fps), 'frames': first['sample_frames'],
               'avg_frame_bytes': first['sample_frame_bytes'],
               'avg_fragments': first['sample_fragments_per_frame']},
    'scenarios': scenarios,
}
json.dump(meta, open(os.path.join(run_dir, 'run.json'), 'w'), ensure_ascii=False, indent=2)
PY

mkdir -p "$OUT_ROOT"
node "$ROOT/scripts/bench-report.mjs" "$RUN_DIR" | tee "$OUT_ROOT/$STAMP.md"
echo
echo "[bench] 报告: $OUT_ROOT/$STAMP.md"
echo "[bench] 原始日志: $RUN_DIR/raw"
