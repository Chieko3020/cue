#!/usr/bin/env python3
"""T-05 验收：改参数 / 注入故障时 cued 不断流（真 probe + 真 cued + 真推流）。

为什么需要这个脚本：单测只能证明"控制通道的报文语义"，证明不了「改参数不断流」——
后者要么靠 pid 不变 + 帧水位连续，要么没有证据。这里把三条判据都做成可复跑的测量：

  A（本版）：UDS set_param / fault.set 前后
      - cued 的 pid 必须不变（最直接的"没重启"证据）；
      - frames_out_total 的相邻增量间隔 = 「多少 ms 没有新帧发出」，与动作前的基线比；
      - probe 的 decoded / frames 单调增长、decode_errors 不涨（解码器没被重置）；
      - cued 自报的 pipeline.encoder_rebuild / pipeline.encoder 事件给出
        「关编码器 + 重新 configure」的真实耗时（µs）。
  B（对照）：进程重启路径（旧实现 set_param 走的 restart_cued = kill + spawn）
      用 cued 自己发的 pipeline.stopped / pipeline.started 时间戳量出停摆时长。
  C（对照，端到端）：按 B 的方式重启 + 与 scripts/test-real-faults.sh 相同的推流端
      重连循环，用同一套帧水位量出「用户真的看到的断流」。

用法：
  python3 scripts/test-control-channel.py a          # 本版：改参数不断流
  python3 scripts/test-control-channel.py b          # HEAD 版：进程停摆（需 OLD_BUILD）
  python3 scripts/test-control-channel.py c          # 旧路径端到端断流
  python3 scripts/test-control-channel.py all 3      # 全部，repeats=3

环境变量：BUILD_DIR（默认 build）、OLD_BUILD（B/C 用的旧构建目录，默认 BUILD_DIR）、
          WS_PORT / PROBE_PORT（默认 19000 / 19001；默认值不与 eval 的 9000/9001 冲突）。

前置：已构建 cued / cue-probe（cmake --build $BUILD_DIR -j1）、node（合成推流）。
"""
import glob
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, os.environ.get("BUILD_DIR", "build"))
WS = int(os.environ.get("WS_PORT", "19000"))
PP = int(os.environ.get("PROBE_PORT", "19001"))
TAG = "[cue-event] "


def wait_socket(pid, timeout=5.0):
    path = f"/tmp/cue/{pid}/control.sock"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return path
        if not os.path.isdir(f"/proc/{pid}"):
            raise SystemExit(f"cued 退出了，等不到 {path}")
        time.sleep(0.01)
    raise SystemExit(f"control.sock 未出现：{path}（cued 是不是降级到 stdout 了？）")


class Lines(threading.Thread):
    """把子进程 stdout 按到达时刻收集起来（事件里的 unix_us 才是精确时刻，这里只用
    到达顺序做配对）。"""

    def __init__(self, proc):
        super().__init__(daemon=True)
        self.proc = proc
        self.items = []
        self.lock = threading.Lock()
        self.start()

    def run(self):
        for raw in self.proc.stdout:
            with self.lock:
                self.items.append((time.monotonic(), raw.rstrip("\n")))

    def snapshot(self):
        with self.lock:
            return list(self.items)


class Ctl:
    """UDS 控制通道客户端（seqpacket：一条消息一个 JSON 对象）。"""

    def __init__(self, pid):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.sock.settimeout(3.0)
        self.sock.connect(f"/tmp/cue/{pid}/control.sock")
        self.n = 0

    def call(self, op, args=None):
        self.n += 1
        req = json.dumps({"id": self.n, "op": op, "args": args or {}}, ensure_ascii=False)
        self.sock.sendall(req.encode())
        buf = b""
        while True:
            chunk = self.sock.recv(200000)
            if not chunk:
                raise RuntimeError("control socket closed")
            buf += chunk
            return json.loads(buf.decode())

    def close(self):
        self.sock.close()


def events(lines, name):
    out = []
    for t, line in lines:
        if not line.startswith(TAG):
            continue
        try:
            body = json.loads(line[len(TAG):])
        except json.JSONDecodeError:
            continue
        if body.get("ev") == name:
            out.append((t, body))
    return out


def metrics_lines(lines):
    out = []
    for t, line in lines:
        if line.startswith("METRICS "):
            try:
                out.append((t, json.loads(line[len("METRICS "):])))
            except json.JSONDecodeError:
                pass
    return out


def probe_metrics(lines):
    out = []
    for t, line in lines:
        if not line.startswith(TAG):
            continue
        try:
            body = json.loads(line[len(TAG):])
        except json.JSONDecodeError:
            continue
        if body.get("ev") == "probe.metric":
            out.append((t, body["data"]))
    return out


def cued_args(binary, width, height, bitrate, preset):
    return [binary, "--ws-port", str(WS), "--probe-port", str(PP), "--transcode", "on",
            "--width", str(width), "--height", str(height), "--bitrate", str(bitrate),
            "--preset", preset, "--fps", "30"]


def start_probe(binary):
    proc = subprocess.Popen([binary, "--port", str(PP), "--bind", "127.0.0.1", "--fps", "30"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    time.sleep(0.2)
    return proc


def start_pusher():
    """与 scripts/test-real-faults.sh 相同的推流循环：断了就重连。"""
    return subprocess.Popen(
        ["bash", "-c",
         f'while :; do timeout 30 node {os.path.join(ROOT, "scripts/test-client.mjs")} '
         f'ws://127.0.0.1:{WS}/ 30 >/dev/null 2>&1 || true; sleep 0.1; done'],
        start_new_session=True)


def stop_all(*procs):
    for p in procs:
        if p is None:
            continue
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            try:
                p.send_signal(signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
    time.sleep(0.4)
    for p in procs:
        if p is not None and p.poll() is None:
            try:
                p.kill()
            except ProcessLookupError:
                pass


def gap_stats(samples, idx):
    """samples: [(t, in_total, out_total)]；返回 (推进点, [(间隔ms, 起点t), ...])。"""
    marks = []
    for s in samples:
        if not marks or s[idx] != marks[-1][1]:
            marks.append((s[0], s[idx]))
    gaps = [((b[0] - a[0]) * 1000.0, a[0]) for a, b in zip(marks, marks[1:])]
    return marks, gaps


def run_a():
    print("== A：本版 UDS 控制（改参数 / 注入不重启） ==")
    probe = start_probe(os.path.join(BUILD, "cue-probe"))
    cued = subprocess.Popen(cued_args(os.path.join(BUILD, "cued"), 640, 360, 1500, "veryfast"),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            bufsize=1)
    r_cued, r_probe = Lines(cued), Lines(probe)
    pusher = None
    try:
        wait_socket(cued.pid)
        ctl = Ctl(cued.pid)
        pusher = start_pusher()
        time.sleep(3.0)  # warmup：解码器/编码器/探针进稳态
        pid_before = ctl.call("metrics.query", {"window_ms": 2000})["result"]["pid"]
        plan = [
            (3.0, "pipeline.set_param", {"key": "bitrate_kbps", "value": "2000"}),
            (5.0, "fault.set", {"kind": "drop", "value": 10}),
            (7.0, "fault.set", {"kind": "drop", "value": 0}),
            (9.0, "pipeline.set_param", {"key": "preset", "value": "ultrafast"}),
            (11.0, "pipeline.set_param", {"key": "transcode", "value": "off"}),
            (13.0, "pipeline.set_param", {"key": "transcode", "value": "on"}),
        ]
        samples = []
        actions = []
        t0 = time.monotonic()
        while True:
            now = time.monotonic() - t0
            while plan and now >= plan[0][0]:
                _, op, args = plan.pop(0)
                ta = time.monotonic()
                actions.append((ta, op, args, ctl.call(op, args)))
            res = ctl.call("metrics.query", {"window_ms": 2000})["result"]
            samples.append((time.monotonic(), res.get("frames_in_total", 0),
                            res.get("frames_out_total", 0)))
            if now > 15.0:
                break
            time.sleep(0.005)  # 约 100~150 Hz
        pid_after = ctl.call("metrics.query", {"window_ms": 2000})["result"]["pid"]
        ctl.close()
        return {
            "pid": cued.pid, "pid_before": pid_before, "pid_after": pid_after,
            "samples": samples, "actions": actions,
            "cued": r_cued.snapshot(), "probe": r_probe.snapshot(),
        }
    finally:
        stop_all(pusher, cued, probe)


def report_a(data):
    samples = data["samples"]
    same = data["pid"] == data["pid_before"] == data["pid_after"]
    print(f"cued pid={data['pid']}；动作前 pid={data['pid_before']}；动作后 pid="
          f"{data['pid_after']} → pid 不变：{same}")
    print(f"轮询 {len(samples)} 次 / 窗口 {samples[-1][0] - samples[0][0]:.2f}s")
    first_action = data["actions"][0][0] if data["actions"] else samples[-1][0]
    for idx, label in ((2, "frames_out_total"), (1, "frames_in_total")):
        marks, gaps = gap_stats(samples, idx)
        if not gaps:
            print(f"{label}: 样本不足")
            continue
        vals = sorted(g[0] for g in gaps)
        base = [g[0] for g in gaps if samples[0][0] + 1.0 < g[1] < first_action - 0.2]
        print(f"{label}: 推进 {len(marks) - 1} 次；间隔 p50={vals[len(vals) // 2]:.1f}ms "
              f"p99={vals[max(0, int(len(vals) * 0.99) - 1)]:.1f}ms max={vals[-1]:.1f}ms"
              + (f"；基线（首个动作前）max={max(base):.1f}ms" if base else ""))
        top = sorted(gaps, reverse=True)[:5]
        print("  最大 5 个间隔：" +
              "  ".join(f"{g[0]:.1f}ms@t={g[1] - samples[0][0]:.2f}s" for g in top))
        for ta, op, args, resp in data["actions"]:
            near = [g[0] for g in gaps if abs(g[1] - ta) < 0.5]
            tag = "ok" if resp.get("ok") else f"失败：{resp.get('error')}"
            print(f"  t={ta - samples[0][0]:5.2f}s {op} {args} → {tag}；"
                  f"该动作 ±0.5s 内最大帧间隔 "
                  f"{max(near):.1f}ms" if near else f"  t={ta - samples[0][0]:5.2f}s {op} → {tag}")
    for name in ("pipeline.encoder_rebuild", "pipeline.encoder", "pipeline.param",
                 "fault.applied", "fault.cleared"):
        got = events(data["cued"], name)
        if got:
            print(f"事件 {name} × {len(got)}；µs 字段与参数：" +
                  " | ".join(json.dumps(b["data"], ensure_ascii=False) for _, b in got[:6]))
    ms = metrics_lines(data["cued"])
    if ms:
        print("cued 每秒 fps：" + " ".join(f"{m.get('fps', 0):.1f}" for _, m in ms))
        print("cued 每秒 injected（清除注入后应停涨）：" +
              " ".join(str(m.get("injected")) for _, m in ms))
    pm = probe_metrics(data["probe"])
    if pm:
        dec = [d.get("decoded") for _, d in pm]
        frm = [d.get("frames") for _, d in pm]
        err = [d.get("decode_errors") for _, d in pm]
        print(f"probe decoded：{dec}")
        print(f"probe frames ：{frm}")
        print(f"probe decode_errors：{err}")
        print(f"probe e2e lat_p99(ms)：{['%.1f' % d['lat_p99'] if isinstance(d.get('lat_p99'), (int, float)) else d.get('lat_p99') for _, d in pm]}")
        print(f"probe jb_level：{[d.get('jb_level') for _, d in pm]}")
        mono_dec = all(y >= x for x, y in zip(dec, dec[1:]))
        mono_frm = all(y >= x for x, y in zip(frm, frm[1:]))
        print(f"probe decoded 单调不减：{mono_dec}；frames 单调不减：{mono_frm}")
    else:
        print("probe 没有 metric 事件（推流没起来？）")


def run_b(repeat):
    old_build = os.path.join(ROOT, os.environ.get("OLD_BUILD", os.environ.get("BUILD_DIR", "build")))
    cued_bin = os.path.join(old_build, "cued")
    print(f"\n== B：进程重启路径（binary={cued_bin}）的停摆 ==")
    stopped_to_started, to_listen = [], []
    for i in range(repeat):
        probe = start_probe(os.path.join(old_build, "cue-probe"))
        cued = subprocess.Popen(cued_args(cued_bin, 640, 360, 1500, "veryfast"),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                bufsize=1)
        reader = Lines(cued)
        pusher = start_pusher()
        cued2 = None
        try:
            wait_socket(cued.pid)
            time.sleep(3.0)
            cued.send_signal(signal.SIGTERM)
            t_kill = time.monotonic()
            cued.wait(timeout=5)
            # 旧的 set_param 走的就是 restart_cued：kill 完立刻 spawn 新一代（同一命令）
            cued2 = subprocess.Popen(cued_args(cued_bin, 640, 360, 1500, "veryfast"),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     bufsize=1)
            t_listen = None
            while time.monotonic() - t_kill < 10:
                try:
                    socket.create_connection(("127.0.0.1", WS), timeout=0.2).close()
                    t_listen = time.monotonic()
                    break
                except OSError:
                    time.sleep(0.005)
            lines2 = Lines(cued2)
            time.sleep(1.5)
            old_stop = events(reader.snapshot(), "pipeline.stopped")
            new_start = events(lines2.snapshot(), "pipeline.started")
            if old_stop and new_start:
                # 两边都是 unix_us（同一台机器）：进程停摆可以直接相减
                stopped_to_started.append((new_start[0][1]["t"] - old_stop[-1][1]["t"]) / 1000.0)
            if t_listen is not None:
                to_listen.append((t_listen - t_kill) * 1000.0)
            print(f"  第 {i + 1} 次：pipeline.stopped→started = "
                  f"{stopped_to_started[-1] if stopped_to_started else float('nan'):.1f} ms；"
                  f"SIGTERM→WS 可连 = {to_listen[-1] if to_listen else float('nan'):.0f} ms")
        finally:
            stop_all(pusher, cued, cued2, probe)
    if stopped_to_started:
        print(f"  进程停摆：{[round(v, 1) for v in stopped_to_started]} ms，中位 "
              f"{sorted(stopped_to_started)[len(stopped_to_started) // 2]:.1f} ms")
    if to_listen:
        print(f"  SIGTERM→WS 可连：{[round(v) for v in to_listen]} ms，中位 "
              f"{sorted(to_listen)[len(to_listen) // 2]:.0f} ms")


def run_c(repeat):
    """旧路径的端到端断流：进程重启 + 推流端重连，用同一套帧水位量。"""
    print("\n== C：旧路径端到端断流（kill+spawn + 推流端重连，帧水位口径） ==")
    outs = []
    for i in range(repeat):
        probe = start_probe(os.path.join(BUILD, "cue-probe"))
        args = cued_args(os.path.join(BUILD, "cued"), 640, 360, 1500, "veryfast")
        cued = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        pusher = start_pusher()
        cued2 = None
        ctl = ctl2 = None
        try:
            wait_socket(cued.pid)
            ctl = Ctl(cued.pid)
            time.sleep(3.0)
            samples = []  # (t, frames_out_total, generation: 1=旧进程 2=新进程)
            t0 = time.monotonic()
            while time.monotonic() - t0 < 12.0:
                if ctl2 is None and time.monotonic() - t0 >= 3.0:
                    ctl.close()
                    cued.send_signal(signal.SIGTERM)
                    cued.wait(timeout=5)
                    cued2 = subprocess.Popen(args, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, bufsize=1)
                    wait_socket(cued2.pid)
                    ctl2 = Ctl(cued2.pid)
                try:
                    active, gen = (ctl2, 2) if ctl2 is not None else (ctl, 1)
                    res = active.call("metrics.query", {"window_ms": 2000})["result"]
                    samples.append((time.monotonic(), res.get("frames_out_total", 0), gen))
                except (RuntimeError, OSError, socket.timeout):
                    pass
                time.sleep(0.005)
            last_old = [s for s in samples if s[2] == 1]
            first_new = [s for s in samples if s[2] == 2 and s[1] > 0]
            if last_old and first_new:
                outs.append((first_new[0][0] - last_old[-1][0]) * 1000.0)
                print(f"  第 {i + 1} 次：旧进程最后一帧 → 新进程第一帧 = {outs[-1]:.0f} ms")
        finally:
            stop_all(pusher, cued, cued2, probe)
    if outs:
        print(f"  旧路径端到端断流：{[round(v) for v in outs]} ms，中位 "
              f"{sorted(outs)[len(outs) // 2]:.0f} ms（其中包含推流端重连的开销）")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "a"
    repeat = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    if mode in ("a", "all"):
        report_a(run_a())
    if mode in ("b", "all"):
        run_b(repeat)
    if mode in ("c", "all"):
        run_c(repeat)


if __name__ == "__main__":
    main()
