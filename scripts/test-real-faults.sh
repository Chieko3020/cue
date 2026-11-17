#!/usr/bin/env bash
# AC-15 / FR-EVAL.13 的可重复验证：real-faults 的每一类故障都用**真 cued 管线**跑一遍
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
# 断言按**类别**（expect.root_cause）而不是 case id 组织——这是扩到 6 类 × 3 个
# case 后的硬要求：同一类下 severity/档位不同，但物理指纹必须一致。
#   a) 每个 case 的 repeat 次都 scored（无 run_error）、root_cause 全部等于 expect；
#   b) repeat 次的 SLA 判定一致且为通过（阈值由本机实测校准：故障态明显低于阈值、
#      修复态 29.4~30.2 fps）；
#   c) 每类的**诊断特征**在故障态成立（这是 AC-15「可从工具面诊断」的可执行判据）：
#        network_loss         ingress_drop 注入；injected>0, dropped=0, budget_drops=0
#        bandwidth_insufficient budget_drops>0, dropped>0, injected=0, encode<33ms
#        queue_backlog        frames_in-frames_out≥10, dropped=0, budget_drops=0, injected=0
#        encoder_too_slow     encode_p50>33ms, op_p50<1ms, budget_drops=dropped=0
#        resolution_too_high  encode_p50>33ms, op_p50>1.5ms, budget_drops=dropped=0
#        network_delay        send_p99 ≥ 0.8×注入值, delayed_pkts>0, 三类丢包计数全 0,
#                             ingress_late=0、故障窗口 e2e p99 越过声明阈值而修复后回落
#                             （延迟不降 fps，这正是它与丢包的区别）
#   d) 修复态 fps 的 repeat 间离散度 ≤10%（阈值不能踩在抖动边缘）。故障态的离散度
#      **只记录不设阈值**：编码/分辨率受限时 cued 的 1 s 窗口读数本身能抖到 ±40%
#      （实测 res-4k-medium 43%），真正要求一致的是逐次的 SLA 判定与每类指纹；
#      时延类不降 fps，反过来要求故障窗口仍然达标；
#   e) **全量模式**下 suite 必须满足 FR-EVAL.13：≥5 类故障、每类 ≥3 个 case。
#
# 用法：
#   BUILD_DIR=build ./scripts/test-real-faults.sh
#   REPEAT=3 BUILD_DIR=build SUITE=suites/real-faults.json ./scripts/test-real-faults.sh
#   ./scripts/test-real-faults.sh --fast     # 每类只抽样第一个 case（6 个；未给 REPEAT 时 repeat=1），
#                                            # 默认全量 18 个 case × REPEAT=3
#
# 环境变量：BUILD_DIR（默认 build；有自己的构建目录时用 BUILD_DIR 指定）、REPEAT（默认 3）、
#           SUITE（默认 suites/real-faults.json）、WORK_ROOT（默认 runs/ac15-real-faults）、
#           PUSH_SECONDS（每次推流秒数，默认 10）、PRODUCER（默认 lavfi）、
#           PRODUCER_SIZE（默认 640x360）、PRODUCER_BITRATE（默认 1200 kbps）
#
# 产帧源（N-28）：默认 `PRODUCER=lavfi`，由 cue-eval 自己按 case 起 cue-capture（真码率源）。
# 这不是洁癖：旧的 `scripts/test-client.mjs` 每帧发同一个 64×64 静态 IDR，transcode 后码率
# 远低于 `--send-mbps` 的注入档位（下限 0.1 Mbps），令牌桶永不耗尽，`bw-starved-*` 的
# `budget_drops > 0` 断言永远不成立——SLA 反而是恒真的。`PRODUCER=none` 可退回旧行为
# （外部推流），但那样这 3 个 case 只会在 cue-eval 的「注入未生效」判定处失败。
#
# 前置：node（scripts/test-client.mjs 合成推流）、python3（state.jsonl 断言）、ss。
# 端口：cue-eval 的子进程后端把 cued 的 WS/probe 端口固定在 9000/9001；
#       被占用时直接失败，不换端口（换了就不是同一个 SUT 了）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
REPEAT_ENV="${REPEAT:-}"  # 记住「用户有没有显式给 REPEAT」：fast 模式未显式给时默认 1
REPEAT="${REPEAT:-3}"
SUITE="${SUITE:-suites/real-faults.json}"
WORK="${WORK_ROOT:-$ROOT/runs/ac15-real-faults}"
PUSH_SECONDS="${PUSH_SECONDS:-10}"
PRODUCER="${PRODUCER:-lavfi}"
PRODUCER_SIZE="${PRODUCER_SIZE:-640x360}"
PRODUCER_BITRATE="${PRODUCER_BITRATE:-1200}"
WS_PORT=9000
PROBE_PORT=9001

FAST=0
for arg in "$@"; do
  case "$arg" in
    --fast) FAST=1 ;;
    --all) FAST=0 ;;
    *) echo "未知参数: $arg（可用：--fast / --all）" >&2; exit 2 ;;
  esac
done
# fast 模式没显式给 REPEAT 时按 1 次跑：抽样是冒烟用，不做 pass^k。
if [ "$FAST" = "1" ] && [ -z "$REPEAT_ENV" ]; then
  REPEAT=1
fi

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

rm -rf "$WORK"
mkdir -p "$WORK"

# --fast：每类只保留 suite 里的第一个 case，写一份过滤后的 suite 到工作目录。
# 默认**不抽样**：全量 6 类 × 3 个 case 才是 FR-EVAL.13 的口径，抽样只用于快速冒烟。
if [ "$FAST" = "1" ]; then
  FILTERED="$WORK/suite-fast.json"
  FILTERED_N=$(python3 - "$SUITE" "$FILTERED" <<'PY'
import json, sys
suite = json.load(open(sys.argv[1], encoding="utf-8"))
seen, kept = set(), []
for c in suite["cases"]:
    root = c["expect"]["root_cause"]
    if root in seen:
        continue
    seen.add(root)
    kept.append(c)
suite["cases"] = kept
json.dump(suite, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print(len(kept))
PY
)
  SUITE="$FILTERED"
  note "fast 模式：每类抽样 1 个 case（$FILTERED_N 个）；默认全跑请去掉 --fast"
fi

note "工作目录 $WORK（BUILD_DIR=$BUILD_DIR REPEAT=$REPEAT SUITE=$SUITE）"

# ---- 合成推流：真 WS 帧 → cued 解码 → 指标里就有真实抖动 ----
# probe 不再由本脚本启动：T-02 起它由 harness（cue-eval 的 ProcessPipelineControl）
# 自己托管——先起 probe 再起 cued，倒序停。这里只要求端口空闲，
# 否则 harness 的 probe 绑不上，case 会明确报 run_error=probe_unavailable。
if [[ "$PRODUCER" == "none" ]]; then
  setsid bash -c '
    while :; do
      timeout "$3" node "$1/scripts/test-client.mjs" "ws://127.0.0.1:$2/" "$3" >/dev/null 2>&1 || true
      sleep 0.1
    done
   ' _ "$ROOT" "$WS_PORT" "$PUSH_SECONDS" >"$WORK/pusher.log" 2>&1 &
  PUSH_PID=$!
  sleep 0.3
else
  # 由 cue-eval 按 case 起 cue-capture：这里必须**不**再起外部推流，否则两个客户端抢
  # 同一个 ws 端口（cued 一次只接一个），会随机出现「有时有流有时没有」。
  PUSH_PID=""
  note "产帧源：cue-eval 自起 cue-capture（$PRODUCER_SIZE @ ${PRODUCER_BITRATE}kbps）"
fi

# ---- 真管线 + scripted 假 agent：每 case 跑「故障态 → 修复态」REPEAT 次 ----
# -j 不用：cue-eval 内部串行；这里只保证同一时刻只有一条 cued（端口写死）。
note "跑 suite（scripted + 真 cued，repeat=$REPEAT）：$SUITE"
T0=$(date +%s)
PRODUCER_ARGS=()
if [[ "$PRODUCER" != "none" ]]; then
  PRODUCER_ARGS=(--producer "$PRODUCER" --producer-size "$PRODUCER_SIZE"
                 --producer-bitrate "$PRODUCER_BITRATE" --capture "$ROOT/$BUILD_DIR/cue-capture")
fi
if ! "$EVAL" --suite "$SUITE" --mode scripted --repeat "$REPEAT" --record-tools "$WORK/tools" \
      "${PRODUCER_ARGS[@]}" --cued "$CUED" --out "$WORK/out" >"$WORK/eval.log" 2>&1; then
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
note "断言：根因 / SLA / 每类诊断特征 / 离散度"
python3 - "$WORK" "$REPEAT" "$SUITE" "$FAST" <<'PY'
import glob
import json
import sys

work, repeat, suite_path, fast = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] == "1"
suite = json.load(open(suite_path, encoding="utf-8"))
cases = {c["id"]: c for c in suite["cases"]}


def sla_of(c):
    return c["expect"].get("sla") or {}


def fps_min_of(c):
    s = sla_of(c)
    v = s.get("fps_min", s.get("usable_fps_min", 0))
    return float(v or 0)


def fault_of(c):
    """(kind, value)：注入量按 kind 从 params 里取，单位随类而不同。"""
    f = c.get("fault") or {}
    kind = f.get("kind") or "none"
    params = f.get("params") or {}
    key = {"drop": "drop_pct", "ingress_drop": "inject_ingress_pct", "rate": "rate_mbps",
           "backlog": "queue_fps", "delay": "delay_ms", "jitter": "jitter_ms"}.get(kind)
    value = float(params.get(key, 0)) if key else 0.0
    return kind, value


# FR-EVAL.13：全量模式下 suite 本身必须 ≥5 类、每类 ≥3 个 case。
# 放在断言里而不是只在文档里写：删掉 case 或加一类只给 1 个，这个脚本先红。
classes = {}
for c in suite["cases"]:
    classes.setdefault(c["expect"]["root_cause"], []).append(c["id"])
if not fast:
    assert len(classes) >= 5, f"FR-EVAL.13：只有 {len(classes)} 类故障（要求 ≥5）：{sorted(classes)}"
    thin = {r: len(ids) for r, ids in classes.items() if len(ids) < 3}
    assert not thin, f"FR-EVAL.13：每类至少 3 个 case，不足的：{thin}"

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


def check_signature(c, o, m):
    """AC-15 的「可从工具面诊断」判据：每类在故障态的指纹必须成立。

    按类别（root_cause）分派，case id 不参与判断——同一类下的 3 个 case 只在
    severity/档位上不同，物理指纹必须一致。注入量从 case 里读，不写死。
    """
    cid = c["id"]
    root = c["expect"]["root_cause"]
    kind, value = fault_of(c)
    sla = sla_of(c)
    fps = m["fps"]
    enc = m.get("encode_p50_ms")
    op = m.get("op_p50_ms")
    if root == "network_loss":
        assert kind == "ingress_drop", f"{cid}: network_loss 必须用上行丢包（实际 {kind}）"
        assert m["injected"] > 0, f"{cid}: 上行丢包应让 injected>0，实际 {m['injected']}"
        assert m["dropped"] == 0, f"{cid}: 单分片帧整体丢失不产生 dropped，实际 {m['dropped']}"
        assert m["frames_in"] >= m["frames_out"], f"{cid}: frames_in<frames_out 不合理"
        assert m["budget_drops"] == 0, f"{cid}: 丢包不应触发令牌桶丢帧"
    elif root == "bandwidth_insufficient":
        assert kind == "rate", f"{cid}: bandwidth_insufficient 必须用限速（实际 {kind}）"
        assert m["budget_drops"] > 0, f"{cid}: 限速应让 budget_drops>0，实际 {m['budget_drops']}"
        assert m["dropped"] >= m["budget_drops"], f"{cid}: 令牌桶丢帧应计进 dropped"
        assert m["injected"] == 0, f"{cid}: 限速与注入丢包是两回事，injected 应为 0"
        assert enc is not None and enc < 33.0, f"{cid}: 编码不该是瓶颈，encode_p50={enc}"
    elif root == "queue_backlog":
        assert kind == "backlog", f"{cid}: queue_backlog 必须用排队节奏（实际 {kind}）"
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
        # 不能拿 op 段判缩放：scale 内嵌在 X264Encoder（见 N-20 的现状说明），开销记在
        # encode 段，op 段只剩 passthrough（实测 4K 三条 case 的 op_p50 常在 0.00~0.01ms）。
        # 这里改用**档位族**（`query_metrics` 的 encode_width/height，N-02b 引入）：
        # 4K 类 case 的配置就是 3840×2160，而 encoder_too_slow 类是 1360×768~1920×1080。
        w = m.get("encode_width")
        h = m.get("encode_height")
        assert w is not None and h is not None and w >= 3840 and h >= 2160, (
            f"{cid}: resolution_too_high 应能在观测里读到 4K 档位，实际 {w}x{h}")
        assert m["budget_drops"] == 0 and m["dropped"] == 0, f"{cid}: 分辨率高不是链路问题"
    elif root == "network_delay":
        # 时延类：唯一成立的是"包都发出去了、只是晚了"。fps 不掉、丢包计数全 0，
        # 所以判据只能落在 send 段分位、延迟计数与故障窗口的端到端 p99 上。
        assert kind in ("delay", "jitter"), f"{cid}: network_delay 必须用延迟/抖动（实际 {kind}）"
        send99 = m.get("send_p99_ms")
        assert send99 is not None and send99 >= 0.8 * value, (
            f"{cid}: 注入 {value}ms 但 send_p99={send99}，发送段没有反映出延迟")
        assert m["injected"] == 0, f"{cid}: 时延不是丢包，injected 应为 0（实际 {m['injected']}）"
        assert m["budget_drops"] == 0 and m["dropped"] == 0, (
            f"{cid}: 时延不该触发任何丢帧（budget_drops={m['budget_drops']} dropped={m['dropped']}）")
        assert m.get("delayed_pkts", 0) > 0, f"{cid}: delayed_pkts 应随注入增长"
        assert m.get("ingress_late", 0) == 0, (
            f"{cid}: 下行延迟注入不该让帧在 cued 侧被判迟到（ingress_late={m.get('ingress_late')}）")
        assert fps >= fps_min_of(c), (
            f"{cid}: 时延故障不降 fps，故障窗口应仍然达标（fps={fps} < {fps_min_of(c)}）")
        # 端到端：故障窗口的 p99 必须真的越过声明阈值，修复后必须回落——
        # 否则这个 case 的时延 SLA 就是两条都成立的摆设。
        lat = float(sla.get("lat_p99_ms") or 0)
        assert lat > 0, f"{cid}: 时延类 case 必须声明 lat_p99_ms"
        p99_fault, p99_after = o.get("p99_ms_fault"), o.get("p99_ms_after")
        assert p99_fault is not None and p99_fault > lat, (
            f"{cid}: 故障窗口 e2e p99={p99_fault} 未越过阈值 {lat}ms，时延故障不可判")
        assert p99_after is not None and p99_after < lat, (
            f"{cid}: 修复后 e2e p99={p99_after} 未回落到阈值 {lat}ms 以内")
    else:
        raise SystemExit(f"{cid}: 未知 root_cause {root}")
    # 时延类的那条反命题已在分支里断言（fps 不降），这里只管"故障会让 fps 掉下来"的五类
    if root != "network_delay":
        assert fps < fps_min_of(c), f"{cid}: 故障态 fps={fps} 应低于阈值 {fps_min_of(c)}"


print()
print("| case | 根因（注入） | 故障态实测 fps / 关键计数 | 修复态 fps | SLA×%d |" % repeat)
print("|---|---|---|---|---|")
verdicts = {}
for cid, c in cases.items():
    got = rows.get(cid, [])
    assert len(got) == repeat, f"{cid}: 期望 {repeat} 次结果，实际 {len(got)}"
    want = c["expect"]["root_cause"]
    kind, value = fault_of(c)
    faults = [fault_metrics(o) for o in got]
    roots = [o["diagnosis"]["root_cause"] for o in got]
    atlas = [bool(o["score"]["sla"]) for o in got]
    finals = [float(o["final_fps"]) for o in got]
    for o in got:
        assert o["scored"], f"{cid}: 存在 run_error: {o.get('error_kind')}"
        assert not o.get("error_kind"), f"{cid}: run_error={o.get('error_kind')}"
    assert set(roots) == {want}, f"{cid}: {repeat} 次根因 {roots} 与期望 {want} 不一致"
    assert set(atlas) == {True}, f"{cid}: SLA 判定在 {repeat} 次里不一致：{atlas}"
    assert len(set(roots)) == 1, f"{cid}: 根因在 {repeat} 次里不一致：{roots}"
    fps_min = fps_min_of(c)
    fps_degraded = want != "network_delay"
    for m in faults:
        if fps_degraded:
            assert m["fps"] <= fps_min - 3.0, (
                f"{cid}: 故障态 fps={m['fps']:.2f} 与阈值 {fps_min} 的余量不足 3 fps")
        else:
            assert m["fps"] >= fps_min, (
                f"{cid}: 时延故障不该降 fps，实际 {m['fps']:.2f} < 阈值 {fps_min}")
    # 诊断签名只在**稳态样本**（最后一条）上校验：故障窗口的第一条 1 Hz 采样可能只覆盖
    # 半个窗口（管线刚起、op/encode 环还没样本），拿它判"op 段应明显"会误报——实测
    # res-4k-medium 第一条采样的 op_p50 是 0.0。与 cue-eval "等满 4 条新采样再取证"同理。
    check_signature(c, got[-1], faults[-1])
    f_base = med([m["fps"] for m in faults])
    if fps_degraded:
        assert f_base <= fps_min - 5.0, (
            f"{cid}: 故障态 fps 中位数 {f_base:.2f} 与阈值 {fps_min} 之间余量不足 5 fps")
    # 修复态：每次都要明显高于阈值，且离散度小（阈值不能踩在抖动边缘）
    for f in finals:
        assert f >= fps_min + 1.0, f"{cid}: 修复态 fps={f:.2f} 未明显高于阈值 {fps_min}"
    # 离散度只在有 ≥2 次采样、且分母非 0 时才有意义：repeat=1（或 --fast 的默认）时
    # 「跨轮离散度」在定义上不存在，硬算会 ZeroDivisionError（本脚本此前就是这么炸的）。
    fspread = None
    if len(finals) >= 2 and med(finals) > 0:
        fspread = (max(finals) - min(finals)) / med(finals)
        assert fspread <= 0.10, f"{cid}: 修复态 fps 离散度 {fspread:.0%} 过大：{finals}"
    fspread_fault = None
    if f_base > 0:
        fspread_fault = (max(m["fps"] for m in faults) - min(m["fps"] for m in faults)) / f_base
    verdicts[cid] = (want, f_base, med(finals), fspread_fault)
    # 用**稳态样本**（最后一条）而不是第一条：故障窗口的第一条 1 Hz 采样可能只覆盖半个
    # 窗口（管线刚起、op/encode 环还没样本），拿它做诊断特征会误报——实测
    # res-4k-medium 的 op_p50 在第一条采样里是 0.0，于是"4K 缩放应明显"必然失败。
    # 与 cue-eval 内部"等满 4 条新采样再取证"是同一条道理。
    m = faults[-1]
    detail = (f"injected={num(m['injected'],0)} dropped={num(m['dropped'],0)} "
              f"budget_drops={num(m['budget_drops'],0)} "
              f"frames_in/out={num(m['frames_in'],0)}/{num(m['frames_out'],0)} "
              f"encode_p50={num(m['encode_p50_ms'])}ms op_p50={num(m['op_p50_ms'])}ms")
    if want == "network_delay":
        detail += (f" send_p50/p99={num(m.get('send_p50_ms'))}/{num(m.get('send_p99_ms'))}ms"
                   f" delayed_pkts={num(m.get('delayed_pkts'),0)}"
                   f" ingress_late={num(m.get('ingress_late'),0)}"
                   f" e2e_p99 故障/修复={num(got[0].get('p99_ms_fault'))}/"
                   f"{num(got[0].get('p99_ms_after'))}ms")
    shown = int(value) if value == int(value) else value
    print(f"| {cid} | {want}（{kind}={shown}） | "
          f"fps={num(med([m['fps'] for m in faults]))}；{detail} | {num(med(finals))} | "
          f"{'/'.join('过' for _ in atlas)} |")

print()
print()
print("一致性：每个 case 的 %d 次 root_cause 完全相同、SLA 判定完全相同（全部通过）。" % repeat)
print("离散度（故障态是 cued 的 1 s 窗口读数，编码/限速受限时窗口本身就抖；修复态取 %d 条中位）：" % repeat)
for cid, v in verdicts.items():
    fault_spread = "N/A" if v[3] is None else f"{v[3]:.0%}"
    print(f"  {cid}: [{v[0]}] 故障态 {v[1]:.2f} fps（repeat 间 {fault_spread}）、修复态 {v[2]:.2f} fps")
print("判据：故障态每条读数在丢包/限速/编码/排队类上低于阈值 ≥3 fps（中位 ≥5 fps）；"
      "时延类（network_delay）反过来要求故障态 fps 仍然达标（它不降 fps），"
      "且故障窗口 e2e p99 越过声明阈值、修复后回落。修复态每次都高于阈值 ≥1 fps。")
PY

note "全部通过。证据：$WORK（eval.log / out/run-*/state.jsonl / tools/*.tools.jsonl）"
note "总耗时 ${ELAPSED}s（含 repeat=$REPEAT）"
if [ "$FAST" = "1" ]; then note "注意：本次是 fast 模式（每类抽样 1 个），不是 FR-EVAL.13 的全量口径"; fi
