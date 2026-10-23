#!/usr/bin/env bash
# 抖动估计基准的 A/B 实验（FR-PIPE 抖动缓冲 / probe 侧）。
#
# 为什么需要这个：JitterBuffer 的观察点有两个（first = 首片到达、done = 帧组装完成）。
# done 会混入「整帧在途耗时 ≈ 帧字节数 / 带宽」，理论上大 IDR 会周期性抬高它。
# 但**默认链路（loopback）上带宽充足，帧瞬间传完，差异测不出来**；而 TCP 链路
# （含 SSH 隧道）会把到达间隔抹平，更是恒 0。所以要复现，必须在 lo 上用 tc 限速。
#
# 结论（12 Mbps，8 Mbps 码率，各 2 轮）：
#   - 档位分布完全相同（L0/L1/L2 各占 6/5/5 个采样点）：
#     说明 done 的尖峰是瞬时的，不足以改变 want_level 的决策。
#   - jitter 的中位数 done **更低**（0.50~0.56 vs 0.56~0.60）：大量正常帧被
#     "最后一片"平滑，把中位拉低。
#   - jitter 的 max done **更高**（0.76/0.61 vs 0.61/0.59）：IDR 大帧那一拍的阶跃。
#   => 所以 first 作为默认基准是合理的（不引入帧大小依赖）；done 只抬尾部。
#     20 Mbps（带宽充足）时这个差异完全消失，done 反而全面更低。
#
# 用法：bash scripts/exp-jitter-basis.sh [rate]
#   需要 sudo 的 tc（会改 lo 的 qdisc，脚本退出时自动恢复）。
#   **注意**：限速期间本机 127.0.0.1 的所有流量都会受影响（含本地 Web 服务）。
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
RATE="${1:-12mbit}"
BASE=$((RANDOM % 1000 + 20000))
WSP=$BASE
PP=$((BASE + 1))

cleanup() {
  sudo -n tc qdisc del dev lo root 2>/dev/null || true
  for n in cue-capture cued cue-probe; do p=$(pgrep -x "$n" 2>/dev/null); [ -n "$p" ] && kill $p 2>/dev/null; done
}
trap cleanup EXIT

run_one() {
  local basis="$1" rep="$2"
  for n in cue-capture cued cue-probe; do p=$(pgrep -x "$n" 2>/dev/null); [ -n "$p" ] && kill $p 2>/dev/null; done
  sleep 1
  local pl="/tmp/jitbasis-$basis-$rep-probe.log"
  : > "$pl"
  build/cue-probe --port "$PP" --bind 127.0.0.1 --jitter-basis "$basis" > "$pl" 2>&1 &
  sleep 0.8
  build/cued --ws-port "$WSP" --probe-port "$PP" --bind 127.0.0.1 \
    --transcode on --width 1280 --height 720 --preset veryfast --bitrate 8000 > /dev/null 2>&1 &
  sleep 1.5
  build/cue-capture --backend lavfi --ws "ws://127.0.0.1:$WSP/" --size 1280x720 --fps 30 \
    --bitrate 8000 --audio off --duration 14 > /dev/null 2>&1 &
  sleep 15
  local n; n=$(grep -cE '"jb_level":[0-9]+' "$pl")
  printf '  %-5s/%s: 样本=%-3s level=' "$basis" "$rep" "$n"
  grep -oE '"jb_level":[0-9]+' "$pl" | sed 's/.*://' | sort | uniq -c | awk '{printf "L%s:%d ", $2, $1}'
  local st; st=$(grep -oE '"jb_jitter_frames":[0-9.]+' "$pl" | sed 's/.*://' | sort -n | \
    awk '{a[NR]=$1} END {if(NR) printf "jitter 中位=%.3f max=%.3f", a[int(NR/2)+1], a[NR]}')
  echo " $st"
}

echo "=== lo + netem rate $RATE（唯一变量 = jitter basis）==="
sudo -n tc qdisc del dev lo root 2>/dev/null || true
sudo -n tc qdisc add dev lo root netem rate "$RATE" || { echo "tc 失败（需要 sudo -n tc）"; exit 1; }
for rep in 1 2; do
  run_one first "$rep"
  run_one done "$rep"
done
