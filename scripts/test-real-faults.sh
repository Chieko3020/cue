#!/usr/bin/env bash
# AC-15 的可重复验证：real-faults 的每一类故障都用**真 cued 管线**跑一遍
# 「故障态 → 修复态」，repeat 次，断言根因与 SLA 判定逐次一致。
#
# 为什么用 --record-tools：scripted 模式在不给 --record-tools/--endpoint/--replay-tools
# 时走的是「写死观测」的自检路径（fps 恒 30.3），拿它校准 SLA 等于自己骗自己。
# 给了 --record-tools 才是 scripted 假 agent + **真管线**（真 cued 子进程、真工具层、
# 真指标、真重测），只是不调 LLM。
#
# 每类的「故障态」读数取自 state.jsonl 里 query_metrics 的真实观测（注入后会等 4 条
# 新采样再取证，见 eval.cpp），修复态读数取自 harness 重测的 final_fps。
#
# 断言：
#   a) 每个 case 的 repeat 次都 scored（无 run_error）、root_cause 全部等于 expect；
#   b) repeat 次的 SLA 判定一致且为通过（阈值由本机实测校准：故障态 11~21 fps、
#      修复态 28.4~28.7 fps，五类统一 fps_min=25，两侧各留 ≥3 fps）；
#   c) 每类的**诊断特征**在故障态成立（这是 AC-15「可从工具面诊断」的可执行判据）：
#        network_loss         injected>0, dropped=0, frames_in=frames_out
#        bandwidth_insufficient budget_drops>0, dropped>0, injected=0
#        queue_backlog        frames_in-frames_out≥10, dropped=0, budget_drops=0
#        encoder_too_slow     encode_p50>33ms, op_p50<1ms, budget_drops=0
#        resolution_too_high  encode_p50>33ms, op_p50>1.5ms, budget_drops=0
#   d) 故障态 fps 的 repeat 间离散度 ≤30%，修复态 fps 的离散度 ≤10%（阈值没抖）。
#
# 用法：
#   BUILD_DIR=build ./scripts/test-real-faults.sh
#   REPEAT=3 BUILD_DIR=build SUITE=suites/real-faults.json ./scripts/test-real-faults.sh
#
# 环境变量：BUILD_DIR（默认 build；有自己的构建目录时用 BUILD_DIR 指定）、REPEAT（默认 3）、
#           SUITE（默认 suites/real-faults.json）、WORK_ROOT（默认 runs/ac15-real-faults）、
#           PUSH_SECONDS（每次推流秒数，默认 3）
#
# 前置：node（scripts/test-client.mjs 合成推流）、python3（state.jsonl 断言）、ss。
# 端口：cue-eval 的子进程后端把 cued 的 WS/probe 端口固定在 9000/9001；
#       被占用时直接失败，不换端口（换了就不是同一个 SUT 了）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
REPEAT="${REPEAT:-3}"
SUITE="${SUITE:-suites/real-faults.json}"
WORK="${WORK_ROOT:-$ROOT/runs/ac15-real-faults}"
PUSH_SECONDS="${PUSH_SECONDS:-10}"
WS_PORT=9000
PROBE_PORT=9001

CUED="$ROOT/$BUILD_DIR/cued"
EVAL="$ROOT/$BUILD_DIR/cue-eval"
PROBE="$ROOT/$BUILD_DIR/cue-probe"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}
note() { echo "[ac15] $*"; }

[ -x "$EVAL" ] || fail "找不到 $EVAL（先构建：cmake -S . -B $BUILD_DIR -DCMAKE_CXX_COMPILER=g++-14 && cmake --build $BUILD_DIR -j1）"
[ -x "$CUED" ] || fail "找不到 $CUED"
[ -x "$PROBE" ] || fail "找不到 $PROBE"
command -v node >/dev/null 2>&1 || fail "需要 node（scripts/test-client.mjs 合成推流）"
command -v python3 >/dev/null 2>&1 || fail "需要 python3（state.jsonl 断言）"
command -v ss >/dev/null 2>&1 || fail "需要 ss（检查 9000/9001 端口占用）"
[ -s "$SUITE" ] || fail "找不到 suite $SUITE"
case "$REPEAT" in
  ''|*[!0-9]*) fail "REPEAT 必须是正整数（收到 $REPEAT）" ;;
esac
[ "$REPEAT" -ge 1 ] || fail "REPEAT 必须 >= 1"

port_busy_tcp() { ss -H -ltn "sport = :$1" 2>/dev/null | grep -q .; }
port_busy_udp() { ss -H -lun "sport = :$1" 2>/dev/null | grep -q .; }
port_busy_tcp "$WS_PORT" && fail "TCP :$WS_PORT 已被占用（cue-eval 的 cued 端口写死）"
port_busy_udp "$PROBE_PORT" && fail "UDP :$PROBE_PORT 已被占用（cue-eval 的 probe 端口写死）"

PROBE_PID=""
PUSH_PID=""

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  if [ -n "$PUSH_PID" ]; then
    # 推流循环用 setsid 单独成组：杀组才收得掉正在跑的 node/test-client
    kill -TERM -"$PUSH_PID" 2>/dev/null || true
    kill -TERM "$PUSH_PID" 2>/dev/null || true
  fi
  if [ -n "$PROBE_PID" ]; then kill -TERM "$PROBE_PID" 2>/dev/null || true; fi
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

rm -rf "$WORK"
mkdir -p "$WORK"
note "工作目录 $WORK（BUILD_DIR=$BUILD_DIR REPEAT=$REPEAT SUITE=$SUITE）"

# ---- 合成推流：真 WS 帧 → cued 解码 → 指标里就有真实抖动 ----
"$PROBE" --port "$PROBE_PORT" >"$WORK/probe.log" 2>&1 &
PROBE_PID=$!
for _ in $(seq 1 50); do
  if port_busy_udp "$PROBE_PORT"; then break; fi
  sleep 0.1
done
port_busy_udp "$PROBE_PORT" || fail "cue-probe 没在 :$PROBE_PORT 上监听（见 $WORK/probe.log）"

setsid bash -c '
  while :; do
    timeout "$3" node "$1/scripts/test-client.mjs" "ws://127.0.0.1:$2/" "$3" >/dev/null 2>&1 || true
    sleep 0.1
  done
 ' _ "$ROOT" "$WS_PORT" "$PUSH_SECONDS" >"$WORK/pusher.log" 2>&1 &
PUSH_PID=$!
sleep 0.3

# ---- 真管线 + scripted 假 agent：每 case 跑「故障态 → 修复态」REPEAT 次 ----
# -j 不用：cue-eval 内部串行；这里只保证同一时刻只有一条 cued（端口写死）。
note "跑 suite（scripted + 真 cued，repeat=$REPEAT）：$SUITE"
T0=$(date +%s)
if ! "$EVAL" --suite "$SUITE" --mode scripted --repeat "$REPEAT" --record-tools "$WORK/tools" \
      --cued "$CUED" --out "$WORK/out" >"$WORK/eval.log" 2>&1; then
  tail -60 "$WORK/eval.log" >&2 || true
  fail "cue-eval 未通过（见 $WORK/eval.log）"
fi
T1=$(date +%s)
ELAPSED=$((T1 - T0))
note "cue-eval 退出码 0，耗时 ${ELAPSED}s"

grep -q "scripted+record-tools" "$WORK/eval.log" || fail "没走 scripted+record-tools 路径（见 $WORK/eval.log）"
SPAWNED=$(grep -c "启动 cued" "$WORK/eval.log" || true)
EXPECT_SPAWN=$((REPEAT * $(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['cases']))" "$SUITE")))
[ "$SPAWNED" -ge "$EXPECT_SPAWN" ] ||
  fail "只启动了 $SPAWNED 次 cued（预期 ≥ $EXPECT_SPAWN）：说明没有真的跑管线"

# ---- 断言 + 输出对照表 ----
note "断言：根因 / SLA / 诊断特征 / 离散度"
python3 - "$WORK" "$REPEAT" "$SUITE" <<'PY'
import glob
import json
import sys

work, repeat, suite_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
suite = json.load(open(suite_path, encoding="utf-8"))
cases = {c["id"]: c for c in suite["cases"]}
sla_fps = {cid: c["expect"]["sla"]["fps_min"] for cid, c in cases.items()}

paths = glob.glob(f"{work}/out/run-*/state.jsonl")
if len(paths) != 1:
    raise SystemExit(f"应当恰好有一个 state.jsonl，实际 {paths}")
rows = {cid: [] for cid in cases}
with open(paths[0], encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        j = json.loads(line)
        if j.get("t") == "run":
            continue
        o = j["outcome"]
        rows.setdefault(o["id"], []).append(o)


def fault_metrics(o):
    """故障态读数：取假 agent 那次真 query_metrics 的观测原文。"""
    for ob in o["observations"]:
        if ob["tool"] != "query_metrics":
            continue
        text = ob["text"]
        if text.startswith("metrics="):
            return json.loads(text[len("metrics="):])
    raise SystemExit(f"{o['id']}: 观测里没有可解析的 query_metrics")


def med(vals):
    vals = sorted(vals)
    n = len(vals)
    return vals[n // 2] if n % 2 else 0.5 * (vals[n // 2 - 1] + vals[n // 2])


def num(v, nd=2):
    return "n/a" if v is None else (f"{v:.{nd}f}" if isinstance(v, float) else str(v))


def check_signature(cid, root, m):
    """AC-15 的「可从工具面诊断」判据：每类在故障态的指纹必须成立。"""
    fps = m["fps"]
    enc = m.get("encode_p50_ms")
    op = m.get("op_p50_ms")
    if root == "network_loss":
        assert m["injected"] > 0, f"{cid}: 上行丢包应让 injected>0，实际 {m['injected']}"
        assert m["dropped"] == 0, f"{cid}: 单分片帧整体丢失不产生 dropped，实际 {m['dropped']}"
        assert m["frames_in"] >= m["frames_out"], f"{cid}: frames_in<frames_out 不合理"
        assert m["budget_drops"] == 0, f"{cid}: 丢包不应触发令牌桶丢帧"
    elif root == "bandwidth_insufficient":
        assert m["budget_drops"] > 0, f"{cid}: 限速应让 budget_drops>0，实际 {m['budget_drops']}"
        assert m["dropped"] >= m["budget_drops"], f"{cid}: 令牌桶丢帧应计进 dropped"
        assert m["injected"] == 0, f"{cid}: 限速与注入丢包是两回事，injected 应为 0"
        assert enc is not None and enc < 33.0, f"{cid}: 编码不该是瓶颈，encode_p50={enc}"
    elif root == "queue_backlog":
        gap = m["frames_in"] - m["frames_out"]
        assert gap >= 10, f"{cid}: 排队应让 frames_in-frames_out 增长，实际 {gap}"
        assert m["dropped"] == 0, f"{cid}: 排队模式不丢帧，dropped 应为 0"
        assert m["budget_drops"] == 0, f"{cid}: 排队模式不触发令牌桶，budget_drops 应为 0"
        assert m["injected"] == 0, f"{cid}: 不该有注入丢包"
    elif root == "encoder_too_slow":
        assert enc is not None and enc > 33.0, f"{cid}: 慢 preset 应让 encode_p50>33ms，实际 {enc}"
        assert op is not None and op < 1.0, f"{cid}: 720p 的缩放开销应 <1ms，实际 {op}"
        assert m["budget_drops"] == 0 and m["dropped"] == 0, f"{cid}: 编码慢不是链路问题"
    elif root == "resolution_too_high":
        assert enc is not None and enc > 33.0, f"{cid}: 4K 编码应超预算，实际 {enc}"
        assert op is not None and op > 1.5, f"{cid}: 4K 缩放（op 段）应明显，实际 {op}"
        assert m["budget_drops"] == 0 and m["dropped"] == 0, f"{cid}: 分辨率高不是链路问题"
    else:
        raise SystemExit(f"{cid}: 未知 root_cause {root}")
    assert fps < sla_fps[cid], f"{cid}: 故障态 fps={fps} 应低于阈值 {sla_fps[cid]}"


print()
print("| case | 根因 | 故障态实测（fps / 关键计数） | 修复态 fps | SLA×%d |" % repeat)
print("|---|---|---|---|---|")
verdicts = {}
for cid in cases:
    got = rows.get(cid, [])
    assert len(got) == repeat, f"{cid}: 期望 {repeat} 次结果，实际 {len(got)}"
    want = cases[cid]["expect"]["root_cause"]
    faults = [fault_metrics(o) for o in got]
    roots = [o["diagnosis"]["root_cause"] for o in got]
    atlas = [bool(o["score"]["sla"]) for o in got]
    finals = [float(o["final_fps"]) for o in got]
    for o in got:
        assert o["scored"], f"{cid}: 存在 run_error: {o.get('error_kind')}"
        assert not o.get("error_kind"), f"{cid}: run_error={o.get('error_kind')}"
    assert set(roots) == {want}, f"{cid}: 3 次根因 {roots} 与期望 {want} 不一致"
    assert set(atlas) == {True}, f"{cid}: SLA 判定在 {repeat} 次里不一致：{atlas}"
    assert len(set(roots)) == 1, f"{cid}: 根因在 {repeat} 次里不一致：{roots}"
    # 故障态：每次读数都要明显低于阈值（≥3 fps 余量），且每类的诊断指纹成立。
    # 单次读数是 cued 的 1 s 窗口：编码/限速受限时一窗内只完成 10~18 帧，
    # 窗口读数本身就有 ±30% 的抖动——这正是「余量」必须留够的原因；
    # 真正要一致的是 SLA 判定（逐次断言），不是这一窗的 fps。
    f_fault = [m["fps"] for m in faults]
    for m in faults:
        assert m["fps"] <= sla_fps[cid] - 3.0, (
            f"{cid}: 故障态 fps={m['fps']:.2f} 与阈值 {sla_fps[cid]} 的余量不足 3 fps")
        check_signature(cid, want, m)
    f_base = med(f_fault)
    assert f_base <= sla_fps[cid] - 5.0, (
        f"{cid}: 故障态 fps 中位数 {f_base:.2f} 与阈值 {sla_fps[cid]} 之间余量不足 5 fps")
    # 修复态：每次都要明显高于阈值，且离散度小（阈值不能踩在抖动边缘）
    for f in finals:
        assert f >= sla_fps[cid] + 1.0, f"{cid}: 修复态 fps={f:.2f} 未明显高于阈值 {sla_fps[cid]}"
    fspread = (max(finals) - min(finals)) / med(finals)
    assert fspread <= 0.10, f"{cid}: 修复态 fps 离散度 {fspread:.0%} 过大：{finals}"
    spread = (max(f_fault) - min(f_fault)) / f_base
    verdicts[cid] = ("/".join("过" for _ in atlas), roots, f_base, med(finals), faults[0], spread)
    m = faults[0]
    detail = (
        f"injected={num(m['injected'],0)} dropped={num(m['dropped'],0)} "
        f"budget_drops={num(m['budget_drops'],0)} "
        f"frames_in/out={num(m['frames_in'],0)}/{num(m['frames_out'],0)} "
        f"encode_p50={num(m['encode_p50_ms'])}ms op_p50={num(m['op_p50_ms'])}ms"
    )
    print(f"| {cid} | {want} | fps={num(med(f_fault))}；{detail} | {num(med(finals))} | "
          f"{verdicts[cid][0]} |")

print()
print()
print("一致性：每个 case 的 %d 次 root_cause 完全相同、SLA 判定完全相同（全部通过）。" % repeat)
print("离散度（故障态是 cued 的 1 s 窗口读数，编码/限速受限时窗口本身就抖；修复态约 1 s 一条、取 3 条中位）：")
for cid, v in verdicts.items():
    print(f"  {cid}: 故障态 {v[2]:.2f} fps（repeat 间 {v[5]:.0%}）、修复态 {v[3]:.2f} fps")
print("判据：故障态每次读数都低于阈值 ≥3 fps（中位 ≥5 fps），修复态每次都高于阈值 ≥1 fps。")
PY

note "全部通过。证据：$WORK（eval.log / out/run-*/state.jsonl / tools/*.tools.jsonl）"
note "总耗时 ${ELAPSED}s（含 repeat=$REPEAT）"
