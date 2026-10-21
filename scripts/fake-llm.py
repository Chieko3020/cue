#!/usr/bin/env python3
"""假 OpenAI 兼容端点：给 M6 cassette 录制备料（不联网、零成本）。

两种剧本：
  demo  按序返回 `cue demo` 的 7 条脚本响应（用于验证录制/回放链路）
  eval  按对话状态返回一次「诊断」轨迹（用于 cue-eval --live --record）
  shell / shell-long  AC-11 夹具：按序尝试 run_shell（越权拒绝 → 白名单命令 → 审计），
                      shell-long 用一条长命令验证「工具执行中也能被取消」（AC-8）

用法：
  python3 scripts/fake-llm.py --port 18765 --mode demo &
  ./build-cassette/cue demo --http http://127.0.0.1:18765 --record cassettes
  ./build-cassette/cue demo --replay cassettes

这个端点是测试夹具，不要把它当成 LLM 实现：它不理解自然语言，只按剧本/工具历史决策。
"""

import argparse
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# 与 apps/cue.cpp 的 demo_script() 逐条对应
DEMO_SCRIPT = [
    ("query_metrics", {"window_ms": 5000}),
    ("stage_complete", {"next": "diagnose"}),
    ("stage_complete", {
        "next": "repair",
        "root_cause": "network_loss",
        "evidence": ["nack_retransmit=42"],
        "confidence": 0.8,
    }),
    ("pipeline_set_param", {"key": "bitrate_kbps", "value": "800"}),
    ("stage_complete", {"next": "verify"}),
    ("query_metrics", {"window_ms": 5000}),
    ("stage_complete", {"next": "done"}),
]


def tool_call(call_id, name, args, usage=(120, 30)):
    return {
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "",
                "tool_calls": [{
                    "id": call_id,
                    "type": "function",
                    "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)},
                }],
            },
            "finish_reason": "tool_calls",
        }],
        "usage": {"prompt_tokens": usage[0], "completion_tokens": usage[1]},
    }


def text_reply(content, usage=(200, 40)):
    return {
        "choices": [{"message": {"role": "assistant", "content": content}, "finish_reason": "stop"}],
        "usage": {"prompt_tokens": usage[0], "completion_tokens": usage[1]},
    }


class DemoBrain:
    def __init__(self):
        self.lock = threading.Lock()
        self.turn = 0

    def respond(self, _request):
        with self.lock:
            idx = min(self.turn, len(DEMO_SCRIPT) - 1)
            self.turn += 1
        name, args = DEMO_SCRIPT[idx]
        # 与 demo 的 c1..c7 对齐，便于肉眼对照
        return tool_call(f"c{idx + 1}", name, args)


def _parse_call(tc):
    fn = tc.get("function") or {}
    try:
        args = json.loads(fn.get("arguments") or "{}")
    except json.JSONDecodeError:
        args = {}
    return fn.get("name") or "", args if isinstance(args, dict) else {}


# eval 剧本：按「工具历史 + 阶段守卫的实际结果」决策，能容忍 guard 拒绝与 SLA 重测。
# 证据从最近一次成功的 query_metrics 返回里逐字摘抄（判分器只认真实观测）。
class EvalBrain:
    def __init__(self):
        self.lock = threading.Lock()
        self.n = 0

    def respond(self, request):
        messages = request.get("messages", [])
        results = {}
        stage = "collect"
        latest_metrics = None
        mutating_ok = False
        observed_ok = False
        for msg in messages:
            if msg.get("role") == "assistant":
                for tc in msg.get("tool_calls") or []:
                    name, args = _parse_call(tc)
                    results.setdefault(tc.get("id"), {"name": name, "args": args, "text": None})
            elif msg.get("role") == "tool":
                item = results.setdefault(msg.get("tool_call_id"), {})
                text = msg.get("content") or ""
                item["text"] = text
                name = item.get("name")
                if text.startswith("ok, now in stage "):
                    stage = text[len("ok, now in stage "):].strip()
                    # 进入 verify 后必须重新取一次「重启之后」的采样；
                    # 用 restart 前的 fps 提交 done 会被 harness 重测拒绝（连续两次 = give_up）
                    if stage == "verify":
                        latest_metrics = None
                elif text.startswith("拒绝："):
                    if stage == "verify":
                        latest_metrics = None
                elif not text.startswith("错误："):
                    if name == "query_metrics" and "metrics=" in text:
                        observed_ok = True
                        latest_metrics = text
                    if name in ("inject_fault", "pipeline_set_param"):
                        mutating_ok = True

        with self.lock:
            self.n += 1
            turn = self.n
        usage = (300 + turn * 7, 30 + turn)

        if stage == "collect":
            if not observed_ok:
                return tool_call(f"e{turn}", "query_metrics", {"window_ms": 5000}, usage)
            return tool_call(f"e{turn}", "stage_complete", {"next": "diagnose"}, usage)

        if stage == "diagnose":
            evidence = []
            if latest_metrics:
                for key in ("injected", "fps", "dropped"):
                    m = re.search(r'"%s"\s*:\s*(-?[0-9.]+)' % key, latest_metrics)
                    if m:
                        evidence.append(f"{key}={m.group(1)}")
            if not evidence:
                return tool_call(f"e{turn}", "query_metrics", {"window_ms": 5000}, usage)
            return tool_call(f"e{turn}", "stage_complete", {
                "next": "repair",
                "root_cause": "network_loss",
                "evidence": evidence[:4],
                "confidence": 0.85,
            }, usage)

        if stage == "repair":
            if not mutating_ok:
                # 取消 5% 注入 = 一次真实的状态变更（SPEC §7.5 认可 dangerous 工具）
                return tool_call(f"e{turn}", "inject_fault", {"kind": "drop", "value": 0}, usage)
            return tool_call(f"e{turn}", "stage_complete", {"next": "verify"}, usage)

        if stage == "verify":
            # harness 的 verify→done 会重测 SLA：先等「重启后」的新采样，再提交 done。
            fps = None
            age = None
            if latest_metrics:
                m = re.search(r'"fps"\s*:\s*([0-9.]+)', latest_metrics)
                if m:
                    fps = float(m.group(1))
                a = re.search(r'"age_ms"\s*:\s*(-?[0-9.]+)', latest_metrics)
                if a:
                    age = float(a.group(1))
            if fps is None or fps < 25.0 or age is None or age > 3000:
                time.sleep(1.2)
                return tool_call(f"e{turn}", "query_metrics", {"window_ms": 5000}, usage)
            return tool_call(f"e{turn}", "stage_complete", {"next": "done"}, usage)

        return text_reply("已进入 done，无需继续调用工具", usage)


# AC-11 夹具：按序尝试「白名单外 → 白名单内」命令。白名单外的 rm/sh/bash 只用于
# 证明「拒绝 + 审计」，脚本里的 rm 永远不会被执行（沙箱在 fork 之前就拒了）。
SHELL_SCRIPT = [
    {"cmd": "rm", "args": ["-rf", "/tmp/cue-ac11-should-not-exist"]},
    {"cmd": "/bin/sh", "args": ["-c", "echo ac11-pwned"]},
    {"cmd": "bash", "args": ["-c", "echo ac11-pwned"]},
    {"cmd": "ls", "args": ["/usr"]},
]

# AC-8 的「工具执行中被 SIGTERM」场景：ls -R -l /usr 在本机约 4.5 s（20 万+ 条目逐个 stat），
# timeout_ms 设到工具允许的上限，保证信号到达时它一定还在执行。
SHELL_LONG_SCRIPT = [{"cmd": "ls", "args": ["-R", "-l", "/usr"], "timeout_ms": 30000}]


class ShellBrain:
    """按剧本逐条返回 run_shell 调用，用完之后 give_up（避免无工具空转）。"""

    def __init__(self, script):
        self.script = script
        self.lock = threading.Lock()
        self.n = 0

    def respond(self, request):
        used = 0
        for msg in request.get("messages", []):
            if msg.get("role") != "assistant":
                continue
            for tc in msg.get("tool_calls") or []:
                name, _ = _parse_call(tc)
                if name == "run_shell":
                    used += 1
        with self.lock:
            self.n += 1
            turn = self.n
        usage = (300 + turn * 7, 30 + turn)
        if used < len(self.script):
            step = self.script[used]
            args = {"cmd": step["cmd"], "args": step["args"]}
            if "timeout_ms" in step:
                args["timeout_ms"] = step["timeout_ms"]
            return tool_call(f"s{turn}", "run_shell", args, usage)
        # 剧本走完：明确放弃，别让 Runner 用文本回复空转到轮次预算
        return tool_call(f"s{turn}", "stage_complete", {"next": "give_up"}, usage)


BRAINS = {
    "demo": DemoBrain,
    "eval": EvalBrain,
    "shell": lambda: ShellBrain(SHELL_SCRIPT),
    "shell-long": lambda: ShellBrain(SHELL_LONG_SCRIPT),
}


def make_handler(brain, stall_s=0.0):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # 静音；需要排查时改成 print
            _ = (fmt, args)

        def do_POST(self):  # noqa: N802（BaseHTTPRequestHandler 的命名约定）
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length)
            if stall_s > 0:
                # AC-8：卡在 LLM 调用里的场景。请求已经收全但故意不回，客户端阻塞在
                # curl_easy_perform 里。这一行是给 scripts/test-cancel.sh 的同步点。
                print(f"[fake-llm] request received, stalling {stall_s:g}s", flush=True)
                time.sleep(stall_s)
            try:
                request = json.loads(raw or b"{}")
            except json.JSONDecodeError:
                request = {}
            body = json.dumps(brain.respond(request), ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                # 客户端被取消后连接已断：写失败是预期结果，不要把 traceback 打到 stderr
                pass
            self.close_connection = True

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=18765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--mode", choices=sorted(BRAINS), default="demo")
    ap.add_argument("--stall-s", type=float, default=0.0,
                    help="收完请求后先睡 N 秒再回（模拟卡在 LLM 调用里；默认 0）")
    args = ap.parse_args()

    brain = BRAINS[args.mode]()
    server = ThreadingHTTPServer((args.host, args.port), make_handler(brain, args.stall_s))
    server.daemon_threads = True
    print(f"[fake-llm] mode={args.mode} http://{args.host}:{args.port} stall_s={args.stall_s:g}",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
