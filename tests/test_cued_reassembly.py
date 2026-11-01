#!/usr/bin/env python3
"""Bounded real WS -> cued -> UDP pressure test, without devices or public peers.

The fixed 64x64 black H264 IDR below was generated with system ffmpeg/libx264:
-f lavfi -i color=c=black:s=64x64:r=1 -frames:v 1 -c:v libx264 -threads 1
-preset ultrafast -tune zerolatency -x264-params keyint=1:repeat-headers=1:aud=1 -f h264
No encoder or third-party Python package is needed to run the test.
Budget gauges cover pending only. Linux RSS samples are diagnostic, not an RSS cap.
"""
from __future__ import annotations
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

IDR = base64.b64decode("AAAAAQkQAAAAAWdCwArcQmwEQAAAAwBAAAADAKPEieAAAAABaM4PyAAAAQYF//9N3EXpvebZSLeWLNgg2SPu73gyNjQgLSBjb3JlIDE2NCByMzEwOCAzMWUxOWY5IC0gSC4yNjQvTVBFRy00IEFWQyBjb2RlYyAtIENvcHlsZWZ0IDIwMDMtMjAyMyAtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTEgZGVibG9jaz0wOjA6MCBhbmFseXNlPTA6MCBtZT1kaWEgc3VibWU9MCBwc3k9MSBwc3lfcmQ9MS4wMDowLjAwIG1peGVkX3JlZj0wIG1lX3JhbmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MCA4eDhkY3Q9MCBjcW09MCBkZWFkem9uZT0yMSwxMSBmYXN0X3Bza2lwPTEgY2hyb21hX3FwX29mZnNldD0wIHRocmVhZHM9MSBsb29rYWhlYWRfdGhyZWFkcz0xIHNsaWNlZF90aHJlYWRzPTAgbnI9MCBkZWNpbWF0ZT0xIGludGVybGFjZWQ9MCBibHVyYXlfY29tcGF0PTAgY29uc3RyYWluZWRfaW50cmE9MCBiZnJhbWVzPTAgd2VpZ2h0cD0wIGtleWludD0xIGtleWludF9taW49MSBzY2VuZWN1dD0wIGludHJhX3JlZnJlc2g9MCByYz1jcmYgbWJ0cmVlPTAgY3JmPTIzLjAgcWNvbXA9MC42MCBxcG1pbj0wIHFwbWF4PTY5IHFwc3RlcD00IGlwX3JhdGlvPTEuNDAgYXE9MACAAAABZYiEOiYoAAkCycnJ1111111111114A==")
RESERVED = {9000, 9001, 8080}


def free_tcp_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class WebSocket:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        response = bytearray()
        while not response.endswith(b"\r\n\r\n") and len(response) < 8192:
            part = self.socket.recv(1)
            if not part:
                raise RuntimeError("WS handshake closed")
            response.extend(part)
        assert response.startswith(b"HTTP/1.1 101 "), response.decode(errors="replace")
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert accept.lower() in bytes(response).lower(), "WS accept mismatch"

    def send(self, data, opcode=2):
        mask = os.urandom(4)
        size = len(data)
        header = bytes([0x80 | opcode, 0x80 | size]) if size < 126 else bytes([0x80 | opcode, 0xFE]) + struct.pack("!H", size)
        body = bytes(value ^ mask[i % 4] for i, value in enumerate(data))
        self.socket.sendall(header + mask + body)

    def close(self):
        self.socket.close()  # EOF exercises session cleanup, not WS graceful close


def packet(frame_id, index, count, pts_us, data):
    return struct.pack("<BBBBIIIIQI", 0xC1, 1, 1, 1, 1, frame_id, index, count,
                       pts_us, len(data)) + data


def metrics(path):
    return [json.loads(line[8:]) for line in path.read_text(errors="replace").splitlines()
            if line.startswith("METRICS ")]


def rss_kib(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except FileNotFoundError:
        pass  # /proc is unavailable on Windows; not a zero-RSS measurement
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cued", required=True, type=Path)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="cue-upstream-reassembly-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    assert not (artifacts / "summary.json").exists(), "preserve prior evidence"
    log = artifacts / "cued.log"
    record = {"result": "pending", "checks": {}, "rss_kib": [], "phases": [],
              "topology": "local WS and UDP loopback only; no devices or public UDP",
              "fixture_sha256": hashlib.sha256(IDR).hexdigest()}
    sockets = []
    process = None
    checks = record["checks"]
    try:
        # Reserve both session UDP slots before launch, avoiding fixed service ports.
        while True:
            first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            first.bind(("127.0.0.1", 0))
            base = first.getsockname()[1]
            second = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                assert base < 65535 and not ({base, base + 1} & RESERVED)
                second.bind(("127.0.0.1", base + 1))
                break
            except (AssertionError, OSError):
                first.close()
                second.close()
        sockets.extend([first, second])
        first.setblocking(False)
        port = free_tcp_port()
        while port in RESERVED:
            port = free_tcp_port()
        argv = [str(args.cued.resolve()), "--bind", "127.0.0.1", "--ws-port", str(port),
                "--probe-host", "127.0.0.1", "--probe-port", str(base),
                "--max-sessions", "2", "--transcode", "off", "--fec", "off"]
        record.update(argv=argv, cued_sha256=hashlib.sha256(args.cued.read_bytes()).hexdigest())
        with log.open("wb") as stream:
            process = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT)
            record["pid"] = process.pid
            deadline = time.monotonic() + 8
            normal = None
            while time.monotonic() < deadline:
                assert process.poll() is None, "cued exited before listen"
                try:
                    normal = WebSocket(port)
                    break
                except ConnectionRefusedError:
                    time.sleep(.05)
            assert normal is not None, "cued listen timeout"
            sockets.append(normal)
            flood = WebSocket(port)
            sockets.append(flood)
            normal_id = 0
            flood_id = 100000
            start = time.monotonic()
            received = {"warmup": 0, "pressure": 0, "drain": 0, "reconnect": 0, "disconnect": 0}
            # Synchronous finite loops avoid orphan sender threads on failure.
            for phase, duration in [("warmup", 1.5), ("pressure", 6.0),
                                    ("drain", 4.0), ("reconnect", 1.5), ("disconnect", 2.5)]:
                if phase == "reconnect":
                    flood.close()
                    sockets.remove(flood)
                    # Give the session thread time to unregister its slot.
                    time.sleep(.3)
                    flood = WebSocket(port)
                    sockets.append(flood)
                    flood.send(packet(999999, 0, 1024, 1, b"X"))
                if phase == "disconnect":
                    flood.close()
                    sockets.remove(flood)
                phase_start = time.monotonic()
                record["phases"].append({"name": phase, "start_s": phase_start - start,
                                          "first_metric_index": len(metrics(log))})
                next_tick = phase_start
                while time.monotonic() - phase_start < duration:
                    assert process.poll() is None, "cued exited during test"
                    pts = int((time.monotonic() - start) * 1e6)
                    cut = len(IDR) // 2
                    normal.send(packet(normal_id, 0, 2, pts, IDR[:cut]))
                    normal.send(packet(normal_id, 1, 2, pts, IDR[cut:]))
                    normal_id += 1
                    if phase == "pressure":
                        # 30 new incomplete 1024-part slots per 50ms, finite 3600 packets.
                        for _ in range(30):
                            flood.send(packet(flood_id, 0, 1024, pts, b"X" * 32))
                            flood_id += 1
                    if phase in ("drain", "reconnect"):
                        # cued's existing housekeeping is receive-driven, not an
                        # idle timer. No-op text messages drive TTL and snapshots
                        # without adding a new pending frame or server response.
                        flood.send(b"{}", opcode=1)
                    while True:
                        try:
                            data = first.recv(65535)
                        except BlockingIOError:
                            break
                        if len(data) >= 24 and data[:2] == b"\xd1\x01":
                            received[phase] += 1
                    sample = rss_kib(process.pid)
                    if sample is not None:
                        record["rss_kib"].append({"s": time.monotonic() - start,
                                                  "phase": phase, "value": sample})
                    next_tick += .05
                    time.sleep(max(0, next_tick - time.monotonic()))
                record["phases"][-1]["last_metric_index"] = len(metrics(log))
            if os.name == "posix" and hasattr(socket, "SOCK_SEQPACKET"):
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as control:
                    control.settimeout(2)
                    control.connect(f"/tmp/cue/{process.pid}/control.sock")
                    control.sendall(json.dumps({"id": 1, "op": "metrics.query",
                                                "args": {"window_ms": 5000}}).encode())
                    record["control_response"] = json.loads(control.recv(200000))
            all_metrics = metrics(log)
            (artifacts / "metrics.json").write_text(json.dumps(all_metrics, indent=2))
            record.update(normal_frames_sent=normal_id, flood_packets_sent=flood_id - 100000,
                          normal_udp_video_packets=received)
            assert all_metrics, "no real metrics"
            pressure = next(p for p in record["phases"] if p["name"] == "pressure")
            pressured = all_metrics[pressure["first_metric_index"]:pressure["last_metric_index"]]
            drained = next(p for p in record["phases"] if p["name"] == "drain")
            draining = all_metrics[drained["first_metric_index"]:drained["last_metric_index"]]
            checks["observed_pressure_limit"] = any(
                any(s.get("upstream_pending_frames") == 256 for s in m["per_session"])
                for m in pressured)
            checks["refusal_visible"] = any(m["upstream_process_budget_rejected"] > 0 for m in pressured)
            checks["all_gauges_bounded"] = all(
                m["upstream_process_pending_frames"] <= m["upstream_process_max_frames"] and
                m["upstream_process_pending_bytes"] <= m["upstream_process_max_bytes"] and
                all(s.get("upstream_pending_frames", 0) <= s.get("upstream_max_pending_frames", 256) and
                    s.get("upstream_pending_bytes", 0) <= s.get("upstream_max_pending_bytes", 64 * 1024**2)
                    for s in m["per_session"]) for m in all_metrics)
            checks["metadata_charged"] = any(m["upstream_process_pending_bytes"] >
                                              m["upstream_process_pending_frames"] * 32 for m in pressured)
            checks["normal_video_continues"] = all(received[p] > 10 for p in received)
            checks["normal_video_decodes"] = any(
                any(s.get("decoded", 0) > 10 and s.get("decode_errors", 0) == 0
                    for s in m["per_session"]) for m in pressured)
            checks["drains_after_ttl"] = bool(draining) and draining[-1]["upstream_process_pending_frames"] == 0
            checks["reconnect_reuses_credit"] = any(
                any(s.get("session_id", 0) > 2 and s.get("upstream_pending_frames") == 1
                    for s in m["per_session"]) for m in all_metrics)
            disconnected = next(p for p in record["phases"] if p["name"] == "disconnect")
            after_disconnect = all_metrics[disconnected["first_metric_index"]:disconnected["last_metric_index"]]
            checks["disconnect_refunds_pending"] = bool(after_disconnect) and all(
                m["upstream_process_pending_frames"] == 0 for m in after_disconnect)
            # All flood inputs are incomplete. The final tick-driven drain must
            # settle only accepted slots, never count each refused packet as a frame.
            flood_final = next(s for s in draining[-1]["per_session"] if s["session_id"] == 2)
            checks["refusal_not_counted_as_frame"] = (
                flood_final["frames_in"] == flood_final["dropped"] and
                flood_final["frames_in"] + flood_final["upstream_pending_frames"] +
                flood_final["upstream_budget_rejected"] == record["flood_packets_sent"])
            if "control_response" in record:
                observed = record["control_response"]["result"]
                keys = ["upstream_pending_frames", "upstream_pending_bytes", "upstream_budget_rejected",
                        "upstream_process_pending_frames", "upstream_process_pending_bytes",
                        "upstream_process_budget_rejected", "upstream_process_max_frames",
                        "upstream_process_max_bytes"]
                checks["control_matches_stdout_snapshot"] = any(
                    all(observed[key] == m[key] for key in keys) for m in all_metrics)
            checks["aggregate_is_sum"] = all(
                m["upstream_pending_frames"] == sum(s.get("upstream_pending_frames", 0) for s in m["per_session"])
                and m["upstream_budget_rejected"] == sum(s.get("upstream_budget_rejected", 0) for s in m["per_session"])
                for m in all_metrics)
            assert all(checks.values()), checks
            record["result"] = "pass"
    except Exception as error:
        record["result"] = "fail"
        record["error"] = repr(error)
    finally:
        for sock in reversed(sockets):
            sock.close()
        if process is not None:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            record.update(cleanup_exit_code=process.returncode, owned_pid_reaped=True)
        (artifacts / "summary.json").write_text(json.dumps(record, indent=2))
    print(json.dumps({"result": record["result"], "checks": checks, "artifacts": str(artifacts)}))
    return 0 if record["result"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
