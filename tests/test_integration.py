#!/usr/bin/env python3
"""T-18a 集成测试：起真进程，验证技术设计 §13.2 的三条端到端判据。

为什么单测不够：ctest 里的 20 个测试全是进程内单测（纯函数/纯组件），证明不了
「cued + cue-probe + 推流端连起来真的能跑」。这个脚本起真进程、走真 UDP 回环，
把 §13.2 的三条要求变成可复跑的测量：

  场景 1 链路健康：cue-probe + cued + cue-capture(lavfi 合成源) 跑 10 s，断言 probe
                  的 decoded / decode_errors / fps / 端到端 p50·p99。
  场景 2 丢包可观测：cued --inject-drop 5 跑 5 s（640x360 多分片帧），断言 injected > 0、
                  probe 侧 nack_sent > 0、FEC 真的补回分片，且发送侧不产生帧级丢弃。
  场景 3 AC-2 崩溃恢复：cue-serve 作为 harness 托管 cued，kill -9 cued 子进程，断言
                  supervisor 记下 pipeline.stopped/restart（旧 pid → 新 pid）且新进程
                  重新吃帧。这条是 AC-2 唯一的自动化验证。

判据的宽紧（防止"为了过而放宽"与"紧到 flaky"都出现）：
  * 硬判据（设计上必须成立，不受负载影响）：decoded>0、decode_errors==0、injected>0、
    nack_sent>0、fec_rec>0、发送侧 dropped/budget_drops==0、restart 事件里的 pid 变化、
    重启后 frames_in 从 0 重新增长。
  * 宽松上界（只防"明显坏了"，不追求精确）：fps 中位数 ∈ [25,35]（标称 30）、
    端到端 p50 中位数 < 100 ms、p99 中位数 < 300 ms、接收端整帧缺失率 ≤5%。
    本机实测 p50≈6 ms、p99≈8 ms、缺失率 0~0.7%：上界留了一个数量级以上的余量——
    2 vCPU 上 x264 编码 + 软解同跑，卡一下就紧界就是 flaky。缺失率 5% 这条不是凑数：
    完全不修复时 5% 包损的整帧缺失率约 23%，两者差一个量级。
  * 只记录不断言：fps 的 min/max、p99 的最大值、cued 的 send 段分位。它们随负载波动，
    写进输出方便定位，但不作为门禁。

端口：随机挑空闲端口（避开写死 9000/9001 的 eval 路径），跑完杀进程组。
总耗时：约 25~30 s。BUILD_DIR 环境变量指定构建目录（ctest 传绝对路径）。
"""
from __future__ import annotations

import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parent.parent
_build = Path(os.environ.get("BUILD_DIR") or "build")
BUILD = _build if _build.is_absolute() else ROOT / _build

EVENT_TAG = "[cue-event] "
RESERVED_PORTS = {9000, 9001}  # eval 后端写死的端口：绝不复用

PROCS: list["Proc"] = []          # 需要兜底清理的进程（异常路径也要收干净）
CHECKS: list[tuple[str, bool, str]] = []


# ---------------------------------------------------------------- 基础设施
class Proc:
    """一个后台子进程 + 它的输出文件。start_new_session=True 是为了杀进程组：
    cue-capture 的 lavfi 后端会再起一个 ffmpeg 子进程，只杀父进程会留下孤儿。"""

    def __init__(self, name: str, argv: list, log_path: Path, cwd: Path = ROOT) -> None:
        self.name = name
        self.log_path = log_path
        self._log = open(log_path, "wb")
        self.p = subprocess.Popen(
            [str(a) for a in argv],
            cwd=str(cwd),
            stdout=self._log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        PROCS.append(self)

    def running(self) -> bool:
        return self.p.poll() is None

    def stop(self, sig: int = signal.SIGTERM) -> None:
        if self.p.poll() is None:
            try:
                os.killpg(self.p.pid, sig)
            except ProcessLookupError:
                pass
        try:
            self.p.wait(timeout=4)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                self.p.wait(timeout=4)
            except subprocess.TimeoutExpired:
                pass
        if not self._log.closed:
            self._log.close()

    def tail(self, n: int = 15) -> str:
        try:
            lines = self.log_path.read_text(errors="replace").splitlines()
        except OSError:
            return "(没有输出文件)"
        return "\n".join(f"    {line}" for line in lines[-n:])


def pick_port(socktype: int) -> int:
    """向内核要一个当前空闲的端口。释放后再交给子进程，理论上有 TOCTOU 窗口，
    但测试串行跑、本机没有抢占者，实测稳定；比"从 19000 开始试并重试"简单得多。"""
    for _ in range(50):
        s = socket.socket(socket.AF_INET, socktype)
        try:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        finally:
            s.close()
        if port not in RESERVED_PORTS:
            return port
    raise RuntimeError("拿不到空闲端口")


def wait_until(fn, timeout: float, what: str, interval: float = 0.05) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if fn():
                return True
        except (URLError, ConnectionError, TimeoutError, json.JSONDecodeError, KeyError):
            pass
        time.sleep(interval)
    print(f"[it] 等待超时（{timeout:.1f}s）：{what}")
    return False


def wait_tcp(port: int, timeout: float) -> bool:
    def probe() -> bool:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True

    return wait_until(probe, timeout, f"TCP :{port} 可连接", interval=0.02)


def check(scenario: str, ok: bool, text: str) -> None:
    CHECKS.append((scenario, bool(ok), text))
    print(f"[it] {'PASS' if ok else 'FAIL'} [{scenario}] {text}")


def parse_events(path: Path) -> list[dict]:
    events = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                if line.startswith(EVENT_TAG):
                    try:
                        events.append(json.loads(line[len(EVENT_TAG):]))
                    except json.JSONDecodeError:
                        continue
    except OSError:
        pass
    return events


def parse_metrics(path: Path) -> list[dict]:
    metrics = []
    try:
        with open(path, errors="replace") as f:
            for line in f:
                if line.startswith("METRICS "):
                    try:
                        metrics.append(json.loads(line[len("METRICS "):]))
                    except json.JSONDecodeError:
                        continue
    except OSError:
        pass
    return metrics


def run_blocking(name: str, argv: list, log_path: Path, timeout: float) -> int:
    """前台跑一个短命进程（推流端），输出落文件；超时按失败处理并杀掉进程组。"""
    with open(log_path, "wb") as f:
        try:
            return subprocess.run(
                [str(a) for a in argv],
                cwd=str(ROOT),
                stdout=f,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                start_new_session=True,
            ).returncode
        except subprocess.TimeoutExpired:
            print(f"[it] {name} 超时（{timeout:.0f}s）")
            return -1


def free_udp_port() -> int:
    return pick_port(socket.SOCK_DGRAM)


# ---------------------------------------------------------------- 场景 1
def scenario_chain(tmp: Path) -> None:
    """./docs 技术设计 §13.2 第 1 条：本机回环 10 s，健康链路的三组读数。"""
    ws_port = pick_port(socket.SOCK_STREAM)
    udp_port = free_udp_port()
    probe = Proc("probe", [BUILD / "cue-probe", "--port", udp_port, "--bind", "127.0.0.1"],
                 tmp / "probe1.log")
    cued = Proc("cued", [BUILD / "cued", "--ws-port", ws_port, "--probe-port", udp_port,
                         "--probe-host", "127.0.0.1", "--transcode", "off", "--fps", "30"],
                tmp / "cued1.log")
    if not wait_tcp(ws_port, 5.0) or not probe.running() or not cued.running():
        check("场景1", False, "probe/cued 没起来（看日志尾部）")
        return

    rc = run_blocking("capture1", [BUILD / "cue-capture", "--backend", "lavfi",
                                   "--size", "640x360", "--fps", "30", "--duration", "10",
                                   "--ws", f"ws://127.0.0.1:{ws_port}/"],
                      tmp / "capture1.log", 40)
    # probe 的指标行是 1 s 窗口：等一拍收尾，才能拿到最后一个完整窗口
    time.sleep(1.5)
    cued.stop()
    probe.stop()

    check("场景1", rc == 0, f"推流端退出码 {rc}（期望 0）")

    probe_events = [e for e in parse_events(tmp / "probe1.log") if e.get("ev") == "probe.metric"]
    fps, p50s, p99s = [], [], []
    prev_decoded = 0
    decoded = 0
    decode_errors = 0
    for e in probe_events:
        d = e.get("data") or {}
        cur = int(d.get("decoded") or 0)
        decoded = max(decoded, cur)
        decode_errors = max(decode_errors, int(d.get("decode_errors") or 0))
        # 只统计完整的观测窗口：推流停掉后的尾巴窗口 fps=0，起点的半秒窗口 fps 也严重偏低
        # （比如 0.1 s 里解了 2 帧 → 20 fps），两者都不是链路问题。门槛 5 帧能把它们排除，
        # 又不会把真实掉帧（<=5 帧/s）当成噪声吞掉——那种情况中位数照样会掉下去。
        if cur - prev_decoded >= 5:
            fps.append(float(d.get("fps") or 0.0))
            # 延迟分位只在 probe 与 cued 时钟对齐后才有值；未对齐写 null
            if d.get("lat_p50") is not None and d.get("lat_p99") is not None:
                p50s.append(float(d["lat_p50"]))
                p99s.append(float(d["lat_p99"]))
        prev_decoded = max(prev_decoded, cur)

    cued_metrics = parse_metrics(tmp / "cued1.log")
    frames_in = max((int(m.get("frames_in") or 0) for m in cued_metrics), default=0)
    frames_out = max((int(m.get("frames_out") or 0) for m in cued_metrics), default=0)
    send_p99 = max((float((m.get("send") or {}).get("p99") or 0.0) for m in cued_metrics),
                   default=0.0)

    have_windows = len(fps) >= 5 and len(p50s) >= 5
    if not have_windows:
        check("场景1", False,
              f"有效观测窗口不足（fps 窗口 {len(fps)} 个、延迟窗口 {len(p50s)} 个，期望 ≥5）")
        return

    med_fps = statistics.median(fps)
    med_p50 = statistics.median(p50s)
    med_p99 = statistics.median(p99s)

    check("场景1", decoded > 0, f"probe 解码帧数 decoded={decoded}（硬判据：>0）")
    check("场景1", decode_errors == 0, f"probe 解码错误 decode_errors={decode_errors}（硬判据：==0）")
    check("场景1", 25.0 <= med_fps <= 35.0,
          f"解码帧率中位数={med_fps:.2f}（标称 30，宽松上界 [25,35]；"
          f"窗口 min={min(fps):.2f} max={max(fps):.2f}，仅记录）")
    check("场景1", med_p50 < 100.0,
          f"端到端 p50 中位数={med_p50:.2f} ms（宽松上界 <100 ms，实测口径见 probe offset）")
    check("场景1", med_p99 < 300.0,
          f"端到端 p99 中位数={med_p99:.2f} ms（宽松上界 <300 ms；"
          f"窗口 max={max(p99s):.2f} ms，仅记录）")
    check("场景1", frames_in > 0 and frames_out > 0,
          f"cued frames_in={frames_in} frames_out={frames_out}（硬判据：都 >0）")
    print(f"[it] 场景1 附注：cued send 段 p99 峰值={send_p99:.2f} ms；"
          f"probe 观测窗口={len(fps)} 个，延迟窗口={len(p50s)} 个")


# ---------------------------------------------------------------- 场景 2
def scenario_loss(tmp: Path) -> None:
    """§13.2 第 2 条：5% 出向丢包。判的是"丢失被观测到并被 FEC/NACK 修复"，
    不是"帧丢了"——注入是**包级**的，帧级 dropped 保持 0 才是修复成功的证据。"""
    ws_port = pick_port(socket.SOCK_STREAM)
    udp_port = free_udp_port()
    probe = Proc("probe", [BUILD / "cue-probe", "--port", udp_port, "--bind", "127.0.0.1"],
                 tmp / "probe2.log")
    cued = Proc("cued", [BUILD / "cued", "--ws-port", ws_port, "--probe-port", udp_port,
                         "--probe-host", "127.0.0.1", "--transcode", "off", "--fps", "30",
                         "--inject-drop", "5"],
                tmp / "cued2.log")
    if not wait_tcp(ws_port, 5.0) or not probe.running() or not cued.running():
        check("场景2", False, "probe/cued 没起来（看日志尾部）")
        return

    # 640x360 必须用多分片帧：64x64 单分片时整帧丢/整帧到，NACK 没有"缺片"可探测
    # （技术设计 §13.2 记下的陷阱），所以这里固定 640x360。
    rc = run_blocking("capture2", [BUILD / "cue-capture", "--backend", "lavfi",
                                   "--size", "640x360", "--fps", "30", "--duration", "5",
                                   "--ws", f"ws://127.0.0.1:{ws_port}/"],
                      tmp / "capture2.log", 30)
    time.sleep(1.3)
    cued.stop()
    probe.stop()

    check("场景2", rc == 0, f"推流端退出码 {rc}（期望 0）")

    probe_events = [e for e in parse_events(tmp / "probe2.log") if e.get("ev") == "probe.metric"]
    # nack_sent 是**每窗口**计数（probe 每秒清零），要累加；fec_rec 是累计值，取最大
    nack_total = sum(int((e.get("data") or {}).get("nack_sent") or 0) for e in probe_events)
    fec_total = max((int((e.get("data") or {}).get("fec_rec") or 0) for e in probe_events),
                    default=0)
    decoded = max((int((e.get("data") or {}).get("decoded") or 0) for e in probe_events),
                  default=0)
    decode_errors = max((int((e.get("data") or {}).get("decode_errors") or 0) for e in probe_events),
                        default=0)
    lost = max((int((e.get("data") or {}).get("lost") or 0) for e in probe_events), default=0)
    frames = max((int((e.get("data") or {}).get("frames") or 0) for e in probe_events), default=0)

    cued_metrics = parse_metrics(tmp / "cued2.log")
    injected = max((int(m.get("injected") or 0) for m in cued_metrics), default=0)
    frames_in = max((int(m.get("frames_in") or 0) for m in cued_metrics), default=0)
    dropped = max((int(m.get("dropped") or 0) for m in cued_metrics), default=0)
    budget_drops = max((int(m.get("budget_drops") or 0) for m in cued_metrics), default=0)
    fec_groups = max((int(m.get("fec_groups") or 0) for m in cued_metrics), default=0)

    check("场景2", injected > 0,
          f"cued 注入丢包 injected={injected}（硬判据：>0；--inject-drop 5 是包级丢包）")
    check("场景2", frames_in > 0, f"cued frames_in={frames_in}（硬判据：>0）")
    check("场景2", nack_total > 0,
          f"probe 发出 NACK nack_sent 合计={nack_total}（硬判据：>0；每窗口计数已累加）")
    check("场景2", fec_total > 0,
          f"FEC 补回分片 fec_rec={fec_total}（cued 发出冗余组 fec_groups={fec_groups}）")
    check("场景2", decoded > 0 and decode_errors == 0,
          f"probe decoded={decoded} decode_errors={decode_errors}（硬判据：解得出且无错）")
    # "修复成功"的核心：包丢了但帧基本没丢。
    #   * cued 侧 dropped/budget_drops 是硬判据：它们涨 = 发送侧真的丢了帧（预算/积压），
    #     与"线路上丢包后被修复"是两种不同的失败。
    #   * probe 的 lost 是接收端结算的"整帧没等到"：XOR-FEC 一组只救 1 片、NACK 限速
    #     8 包/秒，5% 丢包下偶尔真丢 1 帧是设计内的损失，不能断言恒为 0（那是假紧界）。
    #     判据改成丢帧率 ≤5%：若不修复，5% 包损下整帧缺失率约 1-0.95^5≈23%，差一个量级；
    #     实测 0~0.7%。这条是宽松上界，不是"永远能过"。
    lost_rate = lost / max(lost + frames, 1)
    check("场景2", dropped == 0 and budget_drops == 0,
          f"发送侧无帧丢弃 dropped={dropped} budget_drops={budget_drops}"
          f"（硬判据：--inject-drop 是包级注入，不该变成帧级预算丢弃）")
    check("场景2", lost_rate <= 0.05,
          f"接收端整帧缺失率={lost_rate * 100:.2f}%（lost={lost} frames={frames}；"
          f"宽松上界 ≤5%，不修复时约 23%）")



# ---------------------------------------------------------------- 场景 3（AC-2）
def scenario_restart(tmp: Path) -> None:
    """§13.2 第 3 条 / AC-2：kill -9 cued，supervisor 必须记事件并重启，新进程重新吃帧。

    为什么用 cue-serve 当 harness：自动重启是 ProcessPipelineControl（supervisor）的
    行为，不是 cued 自己的；直接起 cued 再 kill 只会得到"进程没了"。cue-serve 是这套
    supervisor 的真实宿主，它的 /api/state 里同时有事件缓存与当前 pid。
    """
    work = tmp / "serve"
    work.mkdir()
    # cue-serve 的 probe 路径是写死的相对路径 ./build/cue-probe（没有 --probe-bin 选项），
    # 用符号链接让任意 BUILD_DIR（CI 的 build-ci）也能被找到，而不是假设构建目录叫 build。
    (work / "build").symlink_to(BUILD.resolve())
    (work / "runs").mkdir()

    http_port = pick_port(socket.SOCK_STREAM)
    ws_port = pick_port(socket.SOCK_STREAM)
    udp_port = free_udp_port()
    serve = Proc("cue-serve", [BUILD / "cue-serve", "--port", http_port,
                               "--ws-port", ws_port, "--probe-port", udp_port,
                               "--cued", BUILD / "cued", "--web", ROOT / "web",
                               "--runs", work / "runs", "--approval", "auto"],
                 tmp / "serve3.log", cwd=work)

    def state() -> dict:
        with urlopen(f"http://127.0.0.1:{http_port}/api/state", timeout=2.0) as resp:
            return json.loads(resp.read().decode())

    def running_pid() -> int:
        st = state()
        return int(st["metrics"].get("pid") or 0) if st["pipeline"]["running"] else 0

    if not wait_until(lambda: running_pid() != 0, 8.0, "cue-serve 托管的 cued 起来"):
        check("场景3", False, "cue-serve 没把 cued 拉起来（看 serve3.log）")
        serve.stop()
        return
    old_pid = running_pid()
    print(f"[it] 场景3 cued 初始 pid={old_pid}")

    cap1 = Proc("capture3a", [BUILD / "cue-capture", "--backend", "lavfi",
                              "--size", "640x360", "--fps", "30", "--duration", "6",
                              "--ws", f"ws://127.0.0.1:{ws_port}/"], tmp / "capture3a.log")
    if not wait_until(lambda: int(state()["metrics"].get("frames_in") or 0) > 0, 6.0,
                      "cued 开始吃帧"):
        check("场景3", False, "kill 之前 frames_in 没有增长，无法证明重启后恢复")
        cap1.stop()
        serve.stop()
        return
    frames_before = int(state()["metrics"].get("frames_in") or 0)
    print(f"[it] 场景3 kill 之前 old_pid 已吃帧 frames_in={frames_before}")

    # SIGKILL：cued 没有任何清理机会，事件只能由 supervisor 补（这才是 AC-2 的形状）
    os.kill(old_pid, signal.SIGKILL)
    got_events = wait_until(
        lambda: any(e.get("ev") == "pipeline.restart" for e in state()["events"]), 8.0,
        "supervisor 记录 pipeline.restart")

    events = state()["events"]
    stopped = [e for e in events if e.get("ev") == "pipeline.stopped"
               and int((e.get("data") or {}).get("pid") or 0) == old_pid]
    restart = [e for e in events if e.get("ev") == "pipeline.restart"
               and int((e.get("data") or {}).get("old_pid") or 0) == old_pid]
    got_events = got_events and bool(stopped) and bool(restart)
    if got_events:
        r = restart[-1]["data"]
        check("场景3", True,
              f"harness 记录了崩溃事件：pipeline.stopped(status={stopped[-1]['data'].get('status')!r})"
              f" + pipeline.restart(old_pid={r.get('old_pid')} → new_pid={r.get('new_pid')})")
    else:
        check("场景3", False, f"事件流里没有 pipeline.restart/pipeline.stopped（events={len(events)} 条）")

    new_pid = running_pid()
    check("场景3", new_pid not in (0, old_pid),
          f"cued 真的换了进程：old_pid={old_pid} → new_pid={new_pid}（硬判据：pid 变了且在跑）")

    # 重启后必须重新吃帧：只有 pid 变化 + frames_in 从"重启瞬间的水位"重新涨起来，
    # 才能排除"supervisor 记了事件但新进程是个空壳"。
    frames_at_restart = int(state()["metrics"].get("frames_in") or 0)
    cap2 = Proc("capture3b", [BUILD / "cue-capture", "--backend", "lavfi",
                              "--size", "640x360", "--fps", "30", "--duration", "3",
                              "--ws", f"ws://127.0.0.1:{ws_port}/"], tmp / "capture3b.log")
    recovered = wait_until(
        lambda: running_pid() == new_pid
        and int(state()["metrics"].get("frames_in") or 0) > max(frames_at_restart, 0),
        8.0, "重启后的 cued 重新吃帧")
    frames_after = int(state()["metrics"].get("frames_in") or 0)
    check("场景3", recovered,
          f"重启后链路恢复：new_pid={new_pid} 的 frames_in 从 {frames_at_restart}"
          f"（新进程计数清零）涨到 {frames_after}（硬判据：真的重新吃帧）")

    cap1.stop()
    cap2.stop()
    serve.stop()
    # 清理判据：harness 退出后不能留下 cued/probe 残留（否则下一次跑会抢端口/抢 /tmp/cue）
    leftovers = [pid for pid in (old_pid, new_pid) if Path(f"/proc/{pid}").exists()]
    check("场景3", not leftovers, f"harness 退出后无残留进程（残留：{leftovers or '无'}）")
    log_text = (tmp / "serve3.log").read_text(errors="replace")
    check("场景3", "[supervisor] cued pid=" in log_text and "已自动重启" in log_text,
          "harness 的 stderr 里有人读的 supervisor 行（与事件流是两份独立证据）")


# ---------------------------------------------------------------- main
def main() -> int:
    required = ["cued", "cue-probe", "cue-capture", "cue-serve"]
    missing = [str(BUILD / name) for name in required if not (BUILD / name).is_file()]
    if missing:
        print(f"[it] FAIL: 缺少可执行文件：{', '.join(missing)}（先 cmake --build {BUILD} -j1）")
        return 1

    t0 = time.monotonic()
    tmp = Path(tempfile.mkdtemp(prefix="cue-it-"))
    try:
        scenario_chain(tmp)
        scenario_loss(tmp)
        scenario_restart(tmp)
    finally:
        for proc in reversed(PROCS):
            try:
                proc.stop()
            except OSError:
                pass
        elapsed = time.monotonic() - t0

    failed = [c for c in CHECKS if not c[1]]
    print(f"\n[it] 汇总：{len(CHECKS) - len(failed)}/{len(CHECKS)} 条断言通过，"
          f"耗时 {elapsed:.1f}s（预算 40s）")
    if failed:
        print("[it] 失败断言：")
        for scenario, _, text in failed:
            print(f"[it]   - [{scenario}] {text}")
        for proc in PROCS:
            print(f"[it] ---- {proc.name} 输出尾部（{proc.log_path}）")
            print(proc.tail())
        print(f"[it] 完整日志留在 {tmp}")
        return 1

    print("[it] 结论：PASS —— 链路健康 / 丢包可观测 / AC-2 崩溃恢复 三个场景都成立")
    # 成功也保留日志目录路径：集成测试偶发失败时最需要的是现场
    print(f"[it] 日志目录：{tmp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
