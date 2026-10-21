#!/usr/bin/env bash
# AC-11 的可重复验收：「白名单外命令被拒且审计有记录」。
#
# 三个场景，全部用假端点/假域，不联网、不花钱、不真的删除任何东西：
#   eval-endpoint  cue-eval --mode real --live --endpoint 假域（AC-16 的真实工具链）
#                  agent 剧本依次尝试 rm / /bin/sh / bash / ls：
#                  rm 等三条必须被拒并且审计里留 denied 行，ls 必须真的执行（ran, exit=0）。
#   cue-run        同一份剧本走交互入口（cue run）：审计落到 runs/audit.jsonl。
#   cancel         run_shell 正在执行 ls -R /usr（timeout 30s）时 SIGTERM：
#                  AC-8 的「5 秒内退出、无残留」必须在**工具执行中**也成立。
#
# 断言只读审计与进程状态；rm 从来没有被执行过（沙箱在 fork 之前就拒绝）。
#
# 用法：
#   BUILD_DIR=build-a11 ./scripts/test-shell-audit.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-a11}"
CUE="$ROOT/$BUILD_DIR/cue"
EVAL="$ROOT/$BUILD_DIR/cue-eval"
MOCK="$ROOT/$BUILD_DIR/cue-mock-svc"
LIMIT_MS=5000

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

now_ms() { date +%s%3N; }

free_port() {
  python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}

wait_for_line() { # <file> <pattern> <timeout_s>
  local file="$1" pattern="$2" deadline
  deadline=$(( $(now_ms) + $3 * 1000 ))
  while [ "$(now_ms)" -lt "$deadline" ]; do
    if grep -q -- "$pattern" "$file" 2>/dev/null; then return 0; fi
    sleep 0.05
  done
  return 1
}

wait_for_port() { # <port> <timeout_s>
  local port="$1" deadline="$(( $(now_ms) + $2 * 1000 ))"
  while [ "$(now_ms)" -lt "$deadline" ]; do
    if python3 -c "import socket,sys
s = socket.socket(); s.settimeout(0.2)
try:
    s.connect(('127.0.0.1', $port))
except OSError:
    raise SystemExit(1)
finally:
    s.close()" >/dev/null 2>&1; then return 0; fi
    sleep 0.05
  done
  return 1
}

PIDS=()
TMPDIRS=()

cleanup() {
  trap - EXIT INT TERM
  local p
  for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -TERM "$p" 2>/dev/null || true; done
  sleep 0.1
  for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -KILL "$p" 2>/dev/null || true; done
  local d
  for d in ${TMPDIRS[@]+"${TMPDIRS[@]}"}; do rm -rf "$d"; done
}
trap cleanup EXIT INT TERM

start_fake_llm() { # <mode> <log> -> 全局 LLM_PORT
  LLM_PORT="$(free_port)"
  : > "$2"
  python3 "$ROOT/scripts/fake-llm.py" --port "$LLM_PORT" --mode "$1" > "$2" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  wait_for_line "$2" "mode=$1" 10 || { cat "$2" >&2; fail "假端点没起来"; }
}

# 审计断言（python 读 JSONL）：三条 denied + 一条 ran(exit=0)，并打印样例行。
assert_audit() { # <audit-file> <label>
  local audit="$1" label="$2"
  [ -f "$audit" ] || fail "[$label] 审计文件不存在：$audit"
  python3 - "$audit" "$label" <<'PY'
import json, sys

path, label = sys.argv[1], sys.argv[2]
records = []
with open(path, "rb") as f:
    for raw in f.read().splitlines():
        if not raw.strip():
            continue
        assert len(raw) <= 4096, f"[{label}] 审计单条超过 4 KiB：{len(raw)}"
        rec = json.loads(raw)
        for key in ("ts_us", "tool", "cmd", "args", "decision", "reason", "exit", "signal",
                    "duration_ms", "stdout_bytes", "stderr_bytes", "cancelled", "degraded"):
            assert key in rec, f"[{label}] 审计缺字段 {key}: {rec}"
        records.append(rec)

def find(cmd, decision):
    return [r for r in records if r["cmd"] == cmd and r["decision"] == decision]

for bad in ("rm", "/bin/sh", "bash"):
    hit = find(bad, "denied")
    assert hit, f"[{label}] 审计里没有 {bad} 的 denied 记录：{records}"
    assert hit[0]["exit"] in (-1, 0), f"[{label}] 被拒的调用不应该有真实退出码：{hit[0]}"

ls = find("ls", "ran")
assert ls, f"[{label}] 审计里没有 ls 的 ran 记录：{records}"
assert any(r["exit"] == 0 for r in ls), f"[{label}] 白名单内 ls 应以 exit=0 结束：{ls}"

denied = find("rm", "denied")[0]
ran = [r for r in ls if r["exit"] == 0][0]
print(f"[{label}] audit: {path}")
print("  denied: " + json.dumps(denied, ensure_ascii=False))
print("  ran   : " + json.dumps(ran, ensure_ascii=False))
PY
}

# ---------------------------------------------------------------- 场景 1：cue-eval --endpoint
run_eval_scenario() {
  local tmp suite mock_log llm_log mock_port
  tmp="$(mktemp -d)"
  TMPDIRS+=("$tmp")
  mock_log="$tmp/mock.log"
  llm_log="$tmp/fake-llm.log"
  suite="$tmp/ac11-suite.json"

  cat > "$suite" <<'JSON'
{
  "suite": "ac11-shell-audit",
  "version": 1,
  "note": "AC-11 夹具：只验证 run_shell 的白名单拒绝与审计，不代表模型能力",
  "cases": [
    {
      "id": "ac11-shell",
      "seed": 42,
      "fault": { "kind": "none" },
      "pipeline": { "bitrate_kbps": 1500, "gop": 60 },
      "prompt": "用 run_shell 查看系统状态",
      "expect": {
        "root_cause": "network_loss",
        "evidence_candidates": ["fps"],
        "allowed_actions": ["run_shell", "query_metrics"],
        "sla": { "fps_min": 1, "dropped_max": 1000000 }
      },
      "budget": { "max_turns": 8, "max_yuan": 1.0, "max_wall_s": 120 }
    }
  ]
}
JSON

  mock_port="$(free_port)"
  "$MOCK" --port "$mock_port" > "$mock_log" 2>&1 &
  PIDS+=("$!")
  wait_for_port "$mock_port" 10 || { cat "$mock_log" >&2; fail "[eval-endpoint] 假域没起来"; }

  start_fake_llm shell "$llm_log"

  local rc=0
  ( cd "$tmp" && env CUE_API_KEY=test-key CUE_BASE_URL="http://127.0.0.1:$LLM_PORT" \
      "$EVAL" --suite "$suite" --mode real --live --endpoint "http://127.0.0.1:$mock_port" \
      --warmup 0 --repeat 1 --out "$tmp/runs" ) > "$tmp/eval.log" 2>&1 || rc=$?
  # 夹具的 agent 明说 give_up：本用例不追求判分通过（退出码 1），只要求审计成立
  [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ] \
    || { cat "$tmp/eval.log" >&2; fail "[eval-endpoint] cue-eval 退出码 $rc"; }

  local audit
  audit="$(ls "$tmp"/runs/run-*/audit.jsonl 2>/dev/null | head -1)"
  [ -n "$audit" ] || { cat "$tmp/eval.log" >&2; fail "[eval-endpoint] 没有产出 audit.jsonl"; }
  assert_audit "$audit" "eval-endpoint"
}

# ---------------------------------------------------------------- 场景 2：cue run
run_cue_scenario() {
  local tmp llm_log
  tmp="$(mktemp -d)"
  TMPDIRS+=("$tmp")
  llm_log="$tmp/fake-llm.log"
  mkdir -p "$tmp/build"
  # cue run 的 cued 路径写死为 ./build/cued：假 cued 只完成握手就退出，
  # 本场景不需要媒体流（run_shell 不依赖管线指标）。
  printf '#!/bin/sh\nexit 0\n' > "$tmp/build/cued"
  chmod +x "$tmp/build/cued"

  start_fake_llm shell "$llm_log"

  local rc=0
  ( cd "$tmp" && env CUE_API_KEY=test-key CUE_BASE_URL="http://127.0.0.1:$LLM_PORT" \
      "$CUE" run --goal "验证 run_shell 审计" --warmup 0 --max-turns 8 --max-wall 120 \
      --allow-dangerous run_shell ) > "$tmp/cue.log" 2>&1 || rc=$?
  [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ] \
    || { cat "$tmp/cue.log" >&2; fail "[cue-run] cue run 退出码 $rc"; }

  assert_audit "$tmp/runs/audit.jsonl" "cue-run"
}

# ---------------------------------------------------------------- 场景 3：工具执行中被取消
run_cancel_scenario() {
  local tmp llm_log cue_log pid t0 t1 elapsed rc=0
  tmp="$(mktemp -d)"
  TMPDIRS+=("$tmp")
  llm_log="$tmp/fake-llm.log"
  cue_log="$tmp/cue.log"
  mkdir -p "$tmp/build"
  printf '#!/bin/sh\nexit 0\n' > "$tmp/build/cued"
  chmod +x "$tmp/build/cued"

  start_fake_llm shell-long "$llm_log"

  ( cd "$tmp" && exec env CUE_API_KEY=test-key CUE_BASE_URL="http://127.0.0.1:$LLM_PORT" \
      "$CUE" run --goal "验证 run_shell 执行中可取消" --warmup 0 --max-turns 4 --max-wall 120 \
      --allow-dangerous run_shell ) > "$cue_log" 2>&1 &
  pid=$!
  PIDS+=("$pid")

  # 同步点不能看 cue 的日志：stdout 重定向到文件是块缓冲，[event] 行要等进程退出才落盘
  # （实测：等日志等于等进程结束）。直接看 /proc 里那条命令本身——它出现就说明
  # 工具已经 fork/exec 并在执行中，这才是 AC-8「工具执行中被取消」的前提。
  local waited=0
  while ! pgrep -f -- "ls -R -l /usr" >/dev/null 2>&1; do
    if [ "$waited" -ge 200 ]; then
      cat "$cue_log" >&2
      fail "[cancel] 20 s 内没有看到 run_shell 子进程"
    fi
    waited=$(( waited + 1 ))
    sleep 0.02
  done
  kill -0 "$pid" 2>/dev/null || { cat "$cue_log" >&2; fail "[cancel] cue 在信号前就退出了"; }

  t0="$(now_ms)"
  kill -TERM "$pid"
  while kill -0 "$pid" 2>/dev/null; do
    if [ $(( $(now_ms) - t0 )) -gt 15000 ]; then fail "[cancel] SIGTERM 后 15 s 仍未退出"; fi
    sleep 0.02
  done
  t1="$(now_ms)"
  elapsed=$(( t1 - t0 ))
  wait "$pid" 2>/dev/null || rc=$?

  [ "$rc" -eq 130 ] || { cat "$cue_log" >&2; fail "[cancel] 退出码 $rc（期望 130）"; }
  [ "$elapsed" -lt "$LIMIT_MS" ] || { cat "$cue_log" >&2; fail "[cancel] ${elapsed} ms ≥ ${LIMIT_MS} ms"; }

  # 残留：sandbox 的子进程组必须整体消失（否则 audit 里 cancelled=true 只是自说自话）
  local residual
  residual="$(pgrep -af -- "ls -R -l /usr" || true)"
  [ -z "$residual" ] || fail "[cancel] 有残留的 run_shell 子进程：
$residual"

  python3 - "$tmp/runs/audit.jsonl" <<'PY'
import json, sys
recs = [json.loads(x) for x in open(sys.argv[1], "rb").read().splitlines() if x.strip()]
hits = [r for r in recs if r["cmd"] == "ls" and r["cancelled"] is True]
assert hits, f"审计里没有 cancelled=true 的 ls 记录：{recs}"
print("  cancelled: " + json.dumps(hits[0], ensure_ascii=False))
PY

  echo "[cancel] run_shell 执行中 SIGTERM → 进程消失：${elapsed} ms（退出码 ${rc}，无残留）"
}

[ -x "$CUE" ] || fail "找不到 $CUE（先构建 $BUILD_DIR）"
[ -x "$EVAL" ] || fail "找不到 $EVAL（先构建 $BUILD_DIR）"
[ -x "$MOCK" ] || fail "找不到 $MOCK（先构建 $BUILD_DIR）"
command -v python3 >/dev/null 2>&1 || fail "需要 python3"

echo "[test-shell-audit] BUILD_DIR=$BUILD_DIR"
run_eval_scenario
run_cue_scenario
run_cancel_scenario
echo "[test-shell-audit] PASS：白名单外命令被拒且审计有记录（AC-11），工具执行中取消 < ${LIMIT_MS} ms"
