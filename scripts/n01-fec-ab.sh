#!/usr/bin/env bash
# N-01 验收：Opus 弱网三件套（in-band FEC + PLC）在**真链路**上的 A/B。
#
# 为什么必须真起进程：单测能证明"decode(FecRecover) 能解出上一帧"，但证明不了
# 「cued 把音频透传下来、probe 用帧号缺口触发恢复、播放队列真的少一个洞」这条接线。
# 这里跑的是 scripts/run.sh 的那套拓扑（cue-capture lavfi → cued UDP → cue-probe），
# 只是把 probe 的渲染关掉（本机无声卡），指标全部取自 probe.metric / cued METRICS。
#
# 四个组合 = {FEC/PLC 都关, 都开} × {注入 0%, 5%}，每轮 20 s：
#   关 = cue-probe --audio-fec off --audio-plc off（全部缺口只计入 unconcealed，基线）
#   开 = cue-probe --audio-fec on  --audio-plc on （默认值，不用显式写也行）
#   注入 = cued --inject-drop N（N-01 起音频下行也参与注入，见 apps/cued.cpp 的音频分支）
#
# 采集端保持默认（in-band FEC on / loss perc 5% / DTX on）：A/B 的两侧必须是**同一份码流**，
# 否则"恢复"和"编码端加冗余"会混成一个变量，说不清收益来自哪一侧。
#
# 下行目标用 --probe-host 127.0.0.1 写死（与 tests/test_integration.py 同一条路径）。
# 不在这里走 CUEHELLO/--probe-host 0.0.0.0 的自动对端：那条路径要求 cued 绑已知端口，
# 而 probe 已经占着同一个 UDP 端口，本机回环上会抢绑；HELLO 解决的问题（NAT 外部地址）
# 在回环上不存在，与本次要测的东西无关。
#
# 用法：
#   ./scripts/n01-fec-ab.sh                 # 4 轮 × 20 s，结果写 runs/n01-fec/
#   DURATION=40 ./scripts/n01-fec-ab.sh     # 加长窗口（丢包更稳，代价是更慢）
#   BUILD_DIR=build-asan ./scripts/n01-fec-ab.sh
#
# 环境变量：BUILD_DIR（默认 build）、DURATION（默认 20 秒）、DROP（默认 5，注入百分比）、
#           OUT（默认 runs/n01-fec）
#
# 本机是 2 vCPU：一轮里同时跑 x264 编码、cued 透传、probe 解码，别再并行开别的重活。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
DURATION="${DURATION:-20}"
DROP="${DROP:-5}"
OUT="${OUT:-runs/n01-fec}"

for bin in cue-capture cued cue-probe; do
  if [[ ! -x "$BUILD_DIR/$bin" ]]; then
    echo "[n01] 缺少 $BUILD_DIR/$bin，先构建：cmake --build $BUILD_DIR -j1" >&2
    exit 1
  fi
done

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/summary.txt "$OUT"/summary.json 2>/dev/null || true

# 向内核要一个空闲端口（TCP/UDP）。释放到子进程使用之间理论上有 TOCTOU 窗口，
# 但本脚本串行跑、端口段随机，实测稳定（与 tests/test_integration.py 同一做法）。
pick_port() {
  python3 - "$1" <<'PY'
import socket, sys
kind = socket.SOCK_STREAM if sys.argv[1] == "tcp" else socket.SOCK_DGRAM
for _ in range(50):
    s = socket.socket(socket.AF_INET, kind)
    try:
        s.bind(("127.0.0.1", 0))
        print(s.getsockname()[1])
        break
    finally:
        s.close()
PY
}

wait_tcp() {
  local port="$1" i
  for i in $(seq 1 100); do
    if python3 -c "import socket,sys; socket.create_connection(('127.0.0.1',$port),0.2).close()" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

# 记录每轮的元数据，最后交给 python 统一聚合（日志解析只写一处，免得 bash 里再写一遍 JSON）
META="$OUT/rounds.tsv"
: >"$META"

run_round() {
  local name="$1" drop="$2" fec="$3" plc="$4"
  local ws udp
  ws="$(pick_port tcp)"
  udp="$(pick_port udp)"
  echo "[n01] 轮次 $name：inject=${drop}% fec=$fec plc=$plc（ws=$ws udp=$udp，${DURATION}s）"

  "$BUILD_DIR/cue-probe" --port "$udp" --bind 127.0.0.1 \
    --audio-fec "$fec" --audio-plc "$plc" \
    >"$OUT/$name.probe.log" 2>&1 &
  local probe_pid=$!

  "$BUILD_DIR/cued" --ws-port "$ws" --probe-port "$udp" --probe-host 127.0.0.1 \
    --transcode off --fps 30 --inject-drop "$drop" \
    >"$OUT/$name.cued.log" 2>&1 &
  local cued_pid=$!

  if ! wait_tcp "$ws"; then
    echo "[n01] 轮次 $name：cued 没起来（看 $OUT/$name.cued.log）" >&2
  fi

  # 采集端前台跑：--duration 到了自己退，退出时由 ChildProcess 析构收掉 ffmpeg 子进程。
  "$BUILD_DIR/cue-capture" --backend lavfi --size 640x360 --fps 30 \
    --audio on --duration "$DURATION" \
    --ws "ws://127.0.0.1:$ws/" >"$OUT/$name.capture.log" 2>&1
  local cap_rc=$?

  # probe 的指标行是 1 s 窗口：等最后一窗（含收尾）落盘再杀，否则最后的计数会少一截。
  sleep 2
  kill "$cued_pid" "$probe_pid" 2>/dev/null
  wait "$cued_pid" 2>/dev/null
  wait "$probe_pid" 2>/dev/null

  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$drop" "$fec" "$plc" "$cap_rc" >>"$META"
}

run_round fec-off-drop0 0 off off
run_round fec-off-drop5 "$DROP" off off
run_round fec-on-drop0 0 on on
run_round fec-on-drop5 "$DROP" on on

# 聚合：probe 的计数是**进程累计**（不是窗口值），所以取整段日志里的最大值；
# cued 的 audio_* 同样累计。视频侧 decoded/lost 也取最大值当对照。
python3 - "$OUT" <<'PY' | tee "$OUT/summary.txt"
import json, sys
from pathlib import Path

out = Path(sys.argv[1])
rows = []
for line in (out / "rounds.tsv").read_text().splitlines():
    if not line.strip():
        continue
    name, drop, fec, plc, rc = line.split("\t")
    probe_events, cued_metrics, cap_line = [], [], ""
    cap_audio_sent = 0
    for ln in (out / f"{name}.probe.log").read_text(errors="replace").splitlines():
        if ln.startswith("[cue-event] "):
            try:
                probe_events.append(json.loads(ln[len("[cue-event] "):]))
            except json.JSONDecodeError:
                pass
    for ln in (out / f"{name}.cued.log").read_text(errors="replace").splitlines():
        if ln.startswith("METRICS "):
            try:
                cued_metrics.append(json.loads(ln[len("METRICS "):]))
            except json.JSONDecodeError:
                pass

    def pmax(key):
        if not probe_events:
            return 0
        return int(max(((e.get("data") or {}).get(key) or 0) for e in probe_events))

    def cmax(k):
        return max((int(m.get(k) or 0) for m in cued_metrics), default=0)

    # 采集端日志里那行"实际生效值"：拿它证明 FEC 编码侧真的开着（参数写了 ≠ 生效）。
    # 同时取最后一条 CAPTURE 行里的 audio_frames_out：它是"采集端到底发了多少音频包"的
    # 权威值。cued 的 audio_* 计数来自 1 Hz 会话快照，会话结束时最后一秒的量不会进快照
    # （实测 1001 发出 / cued 报 953），所以缺口率的分母用 probe 侧的 audio_datagrams，
    # 采集端这个值只用来核对"发送量对得上"。
    for ln in (out / f"{name}.capture.log").read_text(errors="replace").splitlines():
        if "libopus" in ln and "loss_perc" in ln and not cap_line:
            cap_line = ln.strip()
        if ln.startswith("CAPTURE "):
            try:
                cap_audio_sent = int(json.loads(ln[len("CAPTURE "):]).get("audio_frames_out") or 0)
            except json.JSONDecodeError:
                pass

    datagrams = pmax("audio_datagrams")
    gaps = pmax("audio_seq_gaps")
    decoded = pmax("audio_decoded_frames")
    fec_rec = pmax("audio_fec_recovered")
    plc = pmax("audio_plc_frames")
    unconcealed = pmax("audio_unconcealed_frames")
    rows.append({
        "name": name,
        "drop": int(drop),
        "fec": fec,
        "plc": plc,
        "capture_rc": int(rc),
        "probe_fec_enabled": bool((probe_events[-1].get("data") or {}).get("audio_fec_enabled")) if probe_events else None,
        "probe_plc_enabled": bool((probe_events[-1].get("data") or {}).get("audio_plc_enabled")) if probe_events else None,
        "audio_datagrams": datagrams,
        "audio_seq_gaps": gaps,
        "audio_decoded_frames": decoded,
        "audio_fec_recovered": fec_rec,
        "audio_plc_frames": plc,
        "audio_unconcealed_frames": unconcealed,
        "audio_injected_drops": cmax("audio_injected_drops"),
        "audio_frames_in": cmax("audio_frames_in"),
        "audio_frames_out": cmax("audio_frames_out"),
        "video_decoded": pmax("decoded"),
        "video_lost": pmax("lost"),
        "video_injected": cmax("injected"),
        "capture_audio_sent": cap_audio_sent,
        "capture_audio_line": cap_line,
        # 两个比率：loss = 缺口/收到的音频包；unmasked = 未掩盖缺口/全部缺口。
        "loss_pct": (100.0 * gaps / datagrams) if datagrams else 0.0,
        "unmasked_pct": (100.0 * unconcealed / gaps) if gaps else 0.0,
        "fec_share_pct": (100.0 * fec_rec / gaps) if gaps else 0.0,
    })

(out / "summary.json").write_text(json.dumps(rows, ensure_ascii=False, indent=2) + "\n")

hdr = ("轮次", "注入%", "FEC/PLC", "音频包", "缺口", "解码帧", "FEC恢复", "PLC帧",
       "未掩盖", "音频注入丢", "视频解码", "视频丢失", "缺口率%", "未掩盖/缺口%")
print("\t".join(hdr))
for r in rows:
    print("\t".join(str(x) for x in (
        r["name"], r["drop"], f'{r["fec"]}/{r["plc"]}', r["audio_datagrams"],
        r["audio_seq_gaps"], r["audio_decoded_frames"], r["audio_fec_recovered"],
        r["audio_plc_frames"], r["audio_unconcealed_frames"], r["audio_injected_drops"],
        r["video_decoded"], r["video_lost"], f'{r["loss_pct"]:.2f}', f'{r["unmasked_pct"]:.1f}')))

print()
base = next((r for r in rows if r["drop"] > 0 and r["fec"] == "off"), None)
on = next((r for r in rows if r["drop"] > 0 and r["fec"] == "on"), None)
for r in rows:
    print(f'[{r["name"]}] 采集端：{r["capture_audio_line"] or "(日志里没有音频初始化行)"}')
    print(f'           采集发送 {r["capture_audio_sent"]} 包 / cued 记 {r["audio_frames_in"]} 包 / '
          f'probe 收 {r["audio_datagrams"]} 包（cued 计数来自 1 Hz 会话快照，尾秒不进快照，'
          f'账本以 probe 侧为准）')
    if r["probe_fec_enabled"] is not None:
        print(f'           probe 生效开关：fec={r["probe_fec_enabled"]} plc={r["probe_plc_enabled"]}')
if base and on:
    print()
    print(f'{base["drop"]}% 注入：未掩盖缺口 {base["unmasked_pct"]:.1f}% → {on["unmasked_pct"]:.1f}%'
          f'（缺口 {base["audio_seq_gaps"]} → {on["audio_seq_gaps"]}，'
          f'其中 FEC 真恢复 {on["audio_fec_recovered"]} 帧、PLC 外推 {on["audio_plc_frames"]} 帧）')
    # 对账等式：decoded + fec + plc + unconcealed == 可解包数 + 缺口（两个注入轮次都应成立）
    for r in (base, on):
        lhs = (r["audio_decoded_frames"] + r["audio_fec_recovered"] +
               r["audio_plc_frames"] + r["audio_unconcealed_frames"])
        rhs = r["audio_datagrams"] + r["audio_seq_gaps"]
        print(f'  对账 [{r["name"]}] {lhs} == {rhs} → {"成立" if lhs == rhs else "不成立"}')
PY

echo "[n01] 原始日志：$OUT/*.log；结构化结果：$OUT/summary.json"
