#!/usr/bin/env bash
# AC-6 的可重复验证：真管线跑一次录制，回放 3 次，同一 case 的根因/指标必须一致。
#
# 分工（这一点必须说清，否则会把结论读错）：
#   1) 录制：真 cued 子进程 + 真工具层 + 合成推流（scripted agent 不碰网络，但观测是真实的，
#      fps/分位/累计计数每轮都会抖）；
#   2) 回放：只读录制文件，不 spawn cued、不连 endpoint、不碰 casssette 网络。
# 所以这里证明的是「评测链路的确定性」——同一份观测必然得到同一判定，不是
# 「真实系统多次跑出同样数值」（后者在真管线上本来就不成立，那正是需要录制的原因）。
#
# 断言：
#   a) 录制 run 里每个 case 都有 scored 记录，root_cause 等于 case 期望；
#   b) 回放 REPEAT 次，每次 root_cause 与录制完全相同且等于期望，scored 记录齐全；
#   c) REPEAT 次之间的 root_cause / final_fps / final_dropped 逐字节一致；
#   d) 回放没有「启动 cued」的日志（证明真的没有后端被拉起）；
#   e) 负向：把录制里某个 set_param 的 key 改掉，回放必须报错（参数不符），不能静默给默认值。
#
# 用法：
#   BUILD_DIR=build ./scripts/test-replay-consistency.sh
#   REPEAT=3 BUILD_DIR=build ./scripts/test-replay-consistency.sh
#
# 环境变量：BUILD_DIR（默认 build）、REPEAT（默认 3）、SUITE（默认 suites/m4-diagnosis.json）、
#           WORK_ROOT（默认 runs/ac6-replay）、PUSH_SECONDS（每次推流秒数，默认 2）
#
# 前置：node（scripts/test-client.mjs 合成推流）、python3（状态断言）。
# 端口：cue-eval 的子进程后端把 cued 的 WS/probe 端口固定在 9000/9001，被占用时直接失败，
#       不换端口（换了就不是同一个 SUT 了）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
REPEAT="${REPEAT:-3}"
SUITE="${SUITE:-suites/m4-diagnosis.json}"
WORK="${WORK_ROOT:-$ROOT/runs/ac6-replay}"
PUSH_SECONDS="${PUSH_SECONDS:-2}"
WS_PORT=9000
PROBE_PORT=9001

CUED="$ROOT/$BUILD_DIR/cued"
EVAL="$ROOT/$BUILD_DIR/cue-eval"
PROBE="$ROOT/$BUILD_DIR/cue-probe"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}
note() { echo "[ac6] $*"; }

[ -x "$EVAL" ] || fail "找不到 $EVAL（先构建：cmake -S . -B $BUILD_DIR -DCMAKE_CXX_COMPILER=g++-14 && cmake --build $BUILD_DIR -j1）"
[ -x "$CUED" ] || fail "找不到 $CUED"
[ -x "$PROBE" ] || fail "找不到 $PROBE"
command -v node >/dev/null 2>&1 || fail "需要 node（scripts/test-client.mjs 合成推流）"
command -v python3 >/dev/null 2>&1 || fail "需要 python3（state.jsonl 断言）"
[ -s "$SUITE" ] || fail "找不到 suite $SUITE"
case "$REPEAT" in
  ''|*[!0-9]*) fail "REPEAT 必须是正整数（收到 $REPEAT）" ;;
esac
[ "$REPEAT" -ge 1 ] || fail "REPEAT 必须 >= 1"

port_busy_tcp() { ss -H -ltn "sport = :$1" 2>/dev/null | grep -q .; }
port_busy_udp() { ss -H -lun "sport = :$1" 2>/dev/null | grep -q .; }

PUSH_PID=""

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ -n "$PUSH_PID" ]; then
    # 推流循环用 setsid 单独成组：杀组才收得掉正在跑的 node/test-client
    kill -TERM -"$PUSH_PID" 2>/dev/null || true
    kill -TERM "$PUSH_PID" 2>/dev/null || true
  fi
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

command -v ss >/dev/null 2>&1 || fail "需要 ss（检查 9000/9001 端口占用）"
port_busy_tcp "$WS_PORT" && fail "TCP :$WS_PORT 已被占用（cue-eval 的 cued 端口写死）"
port_busy_udp "$PROBE_PORT" && fail "UDP :$PROBE_PORT 已被占用（cue-eval 的 probe 端口写死）"

rm -rf "$WORK"
mkdir -p "$WORK"
note "工作目录 $WORK（BUILD_DIR=$BUILD_DIR REPEAT=$REPEAT）"

# ---- 合成推流：真 WS 帧 → cued 解码 → 指标里就有真实抖动 ----
# probe 由 harness 自己托管（T-02 起），本脚本不再单独拉起：先起 probe 再起 cued
# 是 harness 的职责，重复起会撞 :9001 端口、让 case 报 probe_unavailable。
setsid bash -c '
  while :; do
    timeout "$3" node "$1/scripts/test-client.mjs" "ws://127.0.0.1:$2/" >/dev/null 2>&1 || true
    sleep 0.1
  done
 ' _ "$ROOT" "$WS_PORT" "$PUSH_SECONDS" >"$WORK/pusher.log" 2>&1 &
PUSH_PID=$!
sleep 0.3

# ---- 1) 录制：真管线（scripted 只是不调 LLM，工具与管线都是真的） ----
note "录制（真 cued + 真工具层）：$SUITE"
if ! "$EVAL" --suite "$SUITE" --mode scripted --record-tools "$WORK/tools" \
      --cued "$CUED" --out "$WORK/rec" >"$WORK/record.log" 2>&1; then
  tail -40 "$WORK/record.log" >&2 || true
  fail "录制 run 失败（见 $WORK/record.log）"
fi
grep -q "scripted+record-tools" "$WORK/record.log" || fail "录制 run 没走 record-tools 路径（见 $WORK/record.log）"
grep -q "启动 cued" "$WORK/record.log" || fail "录制 run 没有真的启动 cued"

# 录制文件必须存在，且 NaN 写成 null（encode 段在无转码时本来就没有样本）
shopt -s nullglob
TOOLS=("$WORK/tools"/*.tools.jsonl)
shopt -u nullglob
[ "${#TOOLS[@]}" -gt 0 ] || fail "没有产出任何 <case_id>.tools.jsonl"
grep -q '"encode_p50_ms":null' "${TOOLS[@]}" ||
  fail "录制里看不到 NaN→null（encode_p50_ms 应为 null；写 0 会把「没采到」伪装成「0 ms」）"

# 回放不需要推流与 probe：先全部收掉，后面回放若偷偷拉起后端就会暴露
kill -TERM -"$PUSH_PID" 2>/dev/null || true
kill -TERM "$PUSH_PID" 2>/dev/null || true
PUSH_PID=""
wait 2>/dev/null || true

# ---- 2) 回放 REPEAT 次 ----
for i in $(seq 1 "$REPEAT"); do
  note "回放 #$i"
  if ! "$EVAL" --suite "$SUITE" --mode scripted --replay-tools "$WORK/tools" \
        --cued "$CUED" --out "$WORK/replay-$i" >"$WORK/replay-$i.log" 2>&1; then
    tail -40 "$WORK/replay-$i.log" >&2 || true
    fail "回放 #$i 失败（见 $WORK/replay-$i.log）"
  fi
  grep -q "scripted+replay-tools" "$WORK/replay-$i.log" || fail "回放 #$i 没走 replay-tools 路径"
  if grep -q "启动 cued" "$WORK/replay-$i.log"; then
    fail "回放 #$i 竟然启动了 cued（回放必须只读录制，不 spawn 后端）"
  fi
  grep -q "工具回放 miss=0" "$WORK/replay-$i.log" || fail "回放 #$i 出现工具回放 miss（见日志）"
done

# ---- 3) 一致性断言（root_cause 与 case 期望、录制、其它回放逐项一致） ----
note "断言一致性"
python3 - "$WORK" "$REPEAT" "$SUITE" <<'PY'
import glob
import json
import sys

work, repeat, suite_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
cases = {c["id"]: c["expect"]["root_cause"] for c in json.load(open(suite_path, encoding="utf-8"))["cases"]}


def load(run):
    rows = {}
    paths = glob.glob(f"{work}/{run}/run-*/state.jsonl")
    if len(paths) != 1:
        raise SystemExit(f"{run} 下应当恰好有一个 state.jsonl，实际 {paths}")
    with open(paths[0], encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            j = json.loads(line)
            if j.get("t") == "run":
                continue
            o = j["outcome"]
            rows[o["id"]] = o
    return rows


def brief(o):
    return (o["diagnosis"]["root_cause"], round(float(o["final_fps"]), 6), int(o["final_dropped"]))


rec = load("rec")
missing = set(cases) - set(rec)
assert not missing, f"录制 run 缺 case: {sorted(missing)}"
for cid, want in cases.items():
    o = rec[cid]
    assert o["scored"], f"录制 run 的 {cid} 没有 scored 记录"
    assert not o.get("error_kind"), f"录制 run 的 {cid} 是 run_error: {o.get('error_kind')}"
    assert o["diagnosis"]["root_cause"] == want, f"录制 run 的 {cid} root_cause={o['diagnosis']['root_cause']} != {want}"

print(f"  录制 run：{len(rec)} 个 case 全部 scored，root_cause 与期望一致")
for cid in sorted(cases):
    root, fps, dropped = brief(rec[cid])
    print(f"    {cid}: root_cause={root} fps={fps} dropped={dropped}")

replays = []
for i in range(1, repeat + 1):
    run = load(f"replay-{i}")
    assert set(run) == set(cases), f"回放 #{i} 的 case 集与 suite 不一致"
    for cid, want in cases.items():
        o = run[cid]
        assert o["scored"], f"回放 #{i} 的 {cid} 没有 scored 记录"
        assert not o.get("error_kind"), f"回放 #{i} 的 {cid} 是 run_error: {o.get('error_kind')}"
        assert o["diagnosis"]["root_cause"] == want, (
            f"回放 #{i} 的 {cid} root_cause={o['diagnosis']['root_cause']} != 期望 {want}")
    replays.append(run)
    for cid in sorted(cases):
        root, fps, dropped = brief(run[cid])
        print(f"  回放 #{i}  {cid}: root_cause={root} fps={fps} dropped={dropped}")

for cid in sorted(cases):
    base = brief(rec[cid])
    for i, run in enumerate(replays, 1):
        got = brief(run[cid])
        assert got == base, (
            f"{cid} 回放 #{i} 与录制不一致：录制 {base} vs 回放 {got}")
    roots = {brief(run[cid])[0] for run in replays}
    assert len(roots) == 1, f"{cid} 的 {repeat} 次回放 root_cause 不一致: {sorted(roots)}"
    fps_set = {brief(run[cid])[1] for run in replays}
    assert len(fps_set) == 1, f"{cid} 的 {repeat} 次回放 fps 不一致: {sorted(fps_set)}"
print(f"  一致性通过：{repeat} 次回放的 root_cause / fps / dropped 与录制逐项相同")
PY

# ---- 4) 负向：改掉录制里一个 set_param 的 key，回放必须报错 ----
note "负向用例：改 key 后回放必须失败"
cp -r "$WORK/tools" "$WORK/tools-bad"
python3 - "$WORK/tools-bad" <<'PY'
import glob
import sys

bad_dir = sys.argv[1]
for path in sorted(glob.glob(f"{bad_dir}/*.tools.jsonl")):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for i, line in enumerate(lines):
        if '"op":"set_param"' in line and '"key":"bitrate_kbps"' in line:
            lines[i] = line.replace('"key":"bitrate_kbps"', '"key":"gop"', 1)
            with open(path, "w", encoding="utf-8") as out:
                out.write("\n".join(lines) + "\n")
            print(path)
            sys.exit(0)
sys.exit("录制里找不到 set_param(key=bitrate_kbps) 的条目，负向用例无法构造")
PY

NEG_LOG="$WORK/negative.log"
if "$EVAL" --suite "$SUITE" --mode scripted --replay-tools "$WORK/tools-bad" \
      --cued "$CUED" --out "$WORK/negative" >"$NEG_LOG" 2>&1; then
  fail "负向用例竟然通过了：参数不符必须报错，不能静默给默认值"
fi
grep -q "参数不符" "$NEG_LOG" || fail "负向用例失败但没有「参数不符」说明（见 $NEG_LOG）"
grep -q "tools_replay_miss" "$NEG_LOG" || fail "负向用例失败但没有记 tools_replay_miss（见 $NEG_LOG）"
grep -q "run_error\[.*tools_replay_miss" "$NEG_LOG" || fail "负向用例没有把 miss 记成 run_error（见 $NEG_LOG）"
note "负向用例按预期失败（日志：$NEG_LOG）"

note "全部通过。证据：$WORK（record.log / replay-*.log / negative.log / tools/*.tools.jsonl）"
