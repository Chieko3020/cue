#!/usr/bin/env python3
"""Actual Linux eval/producer lifecycle. Fake failure fixtures are explicit;
normal and cancelled lavfi use the real capture/ffmpeg. No paid model/device.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def children(pid):
    try:
        return [int(p) for p in Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
    except (FileNotFoundError, ProcessLookupError):
        return []


def alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(")", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eval", dest="eval_bin", required=True)
    ap.add_argument("--cued", required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--capture", required=True)
    ap.add_argument("--artifacts")
    a = ap.parse_args()
    for field in ("eval_bin", "cued", "probe", "capture"):
        setattr(a, field, str(Path(getattr(a, field)).resolve()))
    # Engine's existing Process endpoint uses these defaults. Refuse to run
    # against a busy endpoint; never connect a test producer to another service.
    for port, kind in ((9000, socket.SOCK_STREAM), (9001, socket.SOCK_DGRAM)):
        with socket.socket(socket.AF_INET, kind) as reserved:
            reserved.bind(("127.0.0.1", port))
    out = Path(a.artifacts or tempfile.mkdtemp(prefix="cue-producer-test-")).resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Adopt intentionally orphaned fake grandchildren so we can reap them after
    # evaluating whether the product killed the owned process group.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise RuntimeError("PR_SET_CHILD_SUBREAPER failed")
    suite = out / "suite.json"
    suite.write_text(json.dumps({"suite": "producer-lifecycle", "cases": [{
        "id": "producer", "prompt": "observe", "seed": 7,
        "fault": {"kind": "none"}, "pipeline": {"transcode": False},
        "expect": {"root_cause": "network_loss", "evidence_candidates": ["fps"],
                   "sla": {"fps_min": 1}},
        "budget": {"max_wall_s": 60, "max_turns": 16, "max_yuan": 1}}]}))
    fake = out / "fake-capture"
    fake.write_text("#!" + sys.executable + "\n" + '''
import os, signal, subprocess, sys, time
from pathlib import Path
mode=os.environ['CUE_PRODUCER_TEST_MODE']
p=Path(os.environ['CUE_PRODUCER_TEST_PIDS'])
def note(pid):
    with p.open('a') as f: f.write(str(pid)+'\\n')
note(os.getpid())
if mode in ('early', 'no-stream'):
    child=subprocess.Popen([sys.executable,'-c',"import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(120)"])
    note(child.pid)
    time.sleep(.2)
    if mode=='early': sys.exit(7)
    time.sleep(120)
elif mode=='midway':
    child=subprocess.Popen([os.environ['CUE_PRODUCER_TEST_REAL_CAPTURE'],*sys.argv[1:]])
    note(child.pid)
    time.sleep(3)
    sys.exit(8)
else: raise RuntimeError(mode)
''')
    fake.chmod(0o700)
    noexec = out / "no-exec-fixture"
    noexec.write_text("not executable\n")
    noexec.chmod(0o600)
    results = []

    def run(label, capture, producer="lavfi", record=True, mode="", cancel_after=None,
            extra=(), expected_error=None, expected_exit=None, wait_timeout=55, expected_timeout=False):
        work = out / label
        work.mkdir()
        pidlog = work / "fake-pids.txt"
        env = dict(os.environ, CUE_PRODUCER_TEST_MODE=mode,
                   CUE_PRODUCER_TEST_PIDS=str(pidlog),
                   CUE_PRODUCER_TEST_REAL_CAPTURE=a.capture)
        argv = [a.eval_bin, "--suite", str(suite), "--mode", "scripted", "--out", str(work / "runs"),
                "--cued", a.cued, "--probe", a.probe, "--producer", producer,
                "--capture", str(capture), "--producer-size", "160x90", "--producer-bitrate", "200",
                "--warmup", "0", "--sla-settle-ms", "1000"]
        if record:
            argv += ["--record-tools", str(work / "tools")]
        argv += list(extra)
        seen = {}
        failure = None
        timed_out = False
        stop = threading.Event()
        with (work / "eval.log").open("wb") as log:
            proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env)
            def observe():
                while not stop.is_set():
                    todo = children(proc.pid)
                    for pid in todo:
                        todo.extend(p for p in children(pid) if p not in todo)
                        try:
                            cmd = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b'\0', b' ').decode(errors='replace')
                            if cmd: seen[pid] = cmd
                        except FileNotFoundError:
                            pass
                    stop.wait(.02)
            watcher = threading.Thread(target=observe)
            watcher.start()
            try:
                if cancel_after is not None:
                    time.sleep(cancel_after)
                    proc.send_signal(signal.SIGTERM)
                code = proc.wait(timeout=wait_timeout)
            except BaseException as exc:
                failure = repr(exc)
                timed_out = isinstance(exc, subprocess.TimeoutExpired)
            finally:
                # The observer remains live during graceful shutdown. Even when
                # wait/cancellation raises, cleanup owns the engine AND every
                # discovered descendant, including orphaned fake grandchildren.
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=3)
                code = proc.returncode
                stop.set()
                watcher.join()
                fake_pids = [int(x) for x in pidlog.read_text().split()] if pidlog.exists() else []
                observed = set(seen) | set(fake_pids)
                end = time.monotonic() + 3
                while any(alive(pid) for pid in observed) and time.monotonic() < end:
                    time.sleep(.05)
                leaked = [pid for pid in observed if alive(pid)]
                # Preserve the product-leak observation, then ensure the fixture
                # itself never leaves a live process on exceptional paths.
                for pid in leaked:
                    try: os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                end = time.monotonic() + 3
                while any(alive(pid) for pid in observed) and time.monotonic() < end:
                    time.sleep(.02)
                remaining = [pid for pid in observed if alive(pid)]
                for pid in observed:
                    try: os.waitpid(pid, os.WNOHANG)
                    except ChildProcessError: pass
        reports = list((work / "runs").glob("*/report.json"))
        report = json.loads(reports[0].read_text()) if reports else None
        error = report["cases"][0]["error_kind"] if report and report.get("cases") else ""
        checks = {"owned_processes_reaped": not leaked, "fixture_cleanup_complete": not remaining,
                  "unexpected_exception": timed_out if expected_timeout else failure is None}
        if expected_exit is not None:
            checks["exit"] = code == expected_exit
        if expected_error:
            checks["error"] = error.startswith(expected_error)
            checks["no_fake_score"] = bool(report) and report["raw_scored"] == 0 and report["run_errors"] == 1
        if label == "missing":
            # C17 validates entry identity before creating run state or children.
            # Missing paths therefore fail as CLI fatal errors, not case run_errors.
            diagnostics = []
            for line in (work / "eval.log").read_text().splitlines():
                try:
                    diagnostics.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
            fatals = [row for row in diagnostics if isinstance(row, dict) and row.get("event") == "fatal"]
            checks["entry_identity_diagnostic"] = len(fatals) == 1 and fatals[0].get("err") == "process entry identity unreadable: capture"
            checks["no_processes_spawned"] = not seen and not fake_pids and not pidlog.exists()
            checks["no_fake_score_or_run_state"] = not reports and not (work / "runs").exists()
        if label == "forced-timeout":
            checks["forced_timeout_with_descendant"] = timed_out and len(fake_pids) >= 2
        if label == "none":
            checks["capture_not_started"] = not pidlog.exists() and not any(str(fake) in cmd for cmd in seen.values())
            checks["selfcheck_judged"] = bool(report) and report["scored"] == 1
        if label == "real-lavfi":
            checks["actual_capture_seen"] = any(a.capture in cmd for cmd in seen.values())
            checks["actual_ffmpeg_seen"] = any("ffmpeg" in cmd for cmd in seen.values())
            checks["real_stream"] = bool(report) and report["scored"] == 1 and report["cases"][0]["final_samples"] > 0
        if label == "cancel-real-lavfi":
            checks["actual_ffmpeg_seen"] = any("ffmpeg" in cmd for cmd in seen.values())
        if label.startswith("reject-"):
            checks["no_processes_spawned"] = not seen and not reports and not pidlog.exists()
        row = {"label": label, "argv": argv, "exit": code, "error": error, "checks": checks,
               "observed_processes": seen, "fake_pids": fake_pids, "leaked_before_cleanup": leaked,
               "remaining_after_cleanup": remaining, "exception": failure, "expected_timeout": expected_timeout}
        (work / "summary.json").write_text(json.dumps(row, indent=2))
        results.append(row)
        print(label, code, error, checks, flush=True)

    run("none", fake, producer="none", record=False, expected_exit=0)
    run("missing", out / "missing", expected_exit=2)
    run("no-exec", noexec, expected_error="producer_start_failed", expected_exit=1)
    run("early", fake, mode="early", expected_error="producer_start_failed", expected_exit=1)
    run("no-stream", fake, mode="no-stream", expected_error="no_media_stream", expected_exit=1)
    run("midway", fake, mode="midway", expected_error="producer_exited", expected_exit=1)
    run("forced-timeout", fake, mode="no-stream", wait_timeout=2.5, expected_timeout=True)
    run("real-lavfi", a.capture, expected_exit=0)
    run("cancel-real-lavfi", a.capture, cancel_after=3, expected_exit=130)
    run("reject-http", a.capture, extra=("--endpoint", "http://127.0.0.1:9"), expected_exit=2)
    run("reject-replay", a.capture, extra=("--replay-tools", str(out / "absent-tools")), expected_exit=2)
    run("reject-scripted", a.capture, record=False, expected_exit=2)
    passed = all(all(r["checks"].values()) for r in results)
    (out / "summary.json").write_text(json.dumps({"pass": passed, "results": results}, indent=2))
    print("artifacts:", out)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
