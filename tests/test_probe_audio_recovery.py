#!/usr/bin/env python3
"""Actual UDP/Opus -> cue-probe -> SDL dummy recovery regression (stdlib only).

No sound device or encoder is used. Every queue assertion uses a metric generated
at least 400 ms after the last send, well beyond consumption of a 20 ms frame.
Logs and checkpoints are retained, including the owned process's termination code.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

# One 20 ms Opus silence frame. Runtime accounting below verifies that the real
# decoder accepts it and emits exactly 960 PCM samples per received packet.
SILENCE = bytes.fromhex("f8fffe")
TAG = "[cue-event] "


class Probe:
    def __init__(self, binary, target, artifacts):
        self.target = target
        self.sent = self.gaps = self.resyncs = 0
        self.session = 404
        self.sequence = 0
        self.last_send = 0
        self.log_path = artifacts / (str(target) + ".log")
        self.log = self.log_path.open("wb")
        self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.address = reservation.getsockname()
        env = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy")
        self.argv = [str(binary), "--bind", "127.0.0.1", "--port",
                     str(self.address[1]), "--render", "--audio-prebuffer-ms", str(target)]
        self.p = subprocess.Popen(self.argv, stdout=self.log, stderr=subprocess.STDOUT, env=env)
        self.checkpoints = []

    def send(self, count):
        for _ in range(count):
            packet = struct.pack("<BBBBIIHHQ", 0xD1, 2, 0, 3, self.session,
                                 self.sequence, 0, 1, self.sequence * 20000) + SILENCE
            self.sender.sendto(packet, self.address)
            self.sequence += 1
            self.sent += 1
            time.sleep(0.002)
        self.last_send = time.time()

    def skip(self, count):
        self.sequence += count
        self.gaps += count
        self.resyncs += 1

    def observe(self, name, queue_ms, timeout=8):
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            if self.p.poll() is not None:
                raise AssertionError("probe exited unexpectedly: %s; log=%s" %
                                     (self.p.returncode, self.log_path))
            for line in self.log_path.read_text(errors="replace").splitlines():
                if not line.startswith(TAG):
                    continue
                event = json.loads(line[len(TAG):])
                data = event.get("data", {})
                if "audio_queue_ms" not in data:
                    continue
                if event["t"] / 1e6 < self.last_send + 0.4:
                    continue
                latest = data
                if data["audio_decoded_frames"] != self.sent:
                    continue
                # Startup uses None to wait for the real render/metric loop.
                if queue_ms is not None and data["audio_queue_ms"] != queue_ms:
                    continue
                expected = {
                    "audio_decode_errors": 0, "audio_failed_frames": 0,
                    "audio_queue_push_errors": 0, "audio_queue_trimmed": 0,
                    "audio_dropped_drift": 0, "audio_fec_recovered": 0,
                    "audio_plc_frames": 0, "audio_seq_gaps": self.gaps,
                    "audio_unconcealed_frames": self.gaps,
                    "audio_committed_frames": self.sent + self.gaps,
                    "audio_pcm_frames": self.sent * 960,
                    "audio_queue_segments": self.sent,
                    "audio_resyncs": self.resyncs,
                }
                for key, value in expected.items():
                    assert data[key] == value, "%s: %s=%r expected %r" % (name, key, data[key], value)
                assert data["audio_queue_ms"] is not None, "SDL dummy audio device did not open"
                self.checkpoints.append({"name": name, "event": event})
                print("PASS %sms %s queue=%s sent=%s gaps=%s" %
                      (self.target, name, data["audio_queue_ms"], self.sent, self.gaps), flush=True)
                return
            time.sleep(0.025)
        raise AssertionError("%s: expected queue=%r after >=400ms; latest=%r; log=%s" %
                             (name, queue_ms, latest, self.log_path))

    def stop(self, artifacts):
        before = self.p.poll()
        if before is None:
            self.p.terminate()
        try:
            self.p.wait(timeout=4)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait(timeout=4)
        self.log.close()
        self.sender.close()
        (artifacts / (str(self.target) + ".json")).write_text(json.dumps({
            "argv": self.argv, "pid": self.p.pid, "exit_before_cleanup": before,
            "cleanup_exit_code": self.p.returncode, "owned_pid_reaped": self.p.poll() is not None,
            "checkpoints": self.checkpoints,
        }, indent=2), encoding="utf-8")


def scenario(p):
    p.observe("ready", None)
    if p.target == 2000:
        p.send(99)
        p.observe("nonempty-before-resync", 1980)
        p.skip(6)
        p.send(1)
        p.observe("nonempty-resync-preserves-and-drains", 0)
        return
    if p.target == 250:
        p.send(12)
        p.observe("startup-below-target-paused", 240)
        p.send(1)
        p.observe("startup-cross-target-drains", 0)
    else:
        p.send(1)
        p.observe("zero-target-startup-drains", 0)
    p.skip(150)
    p.send(1)
    p.observe("same-session-recovery", 20 if p.target else 0)
    if p.target == 250:
        p.send(11)
        p.observe("same-resync-does-not-clear-subsequent-pcm", 240)
        p.send(1)
        p.observe("recovery-cross-target-drains", 0)
        p.session += 1
        p.sequence = 0
        p.send(1)
        p.observe("new-session-below-target-paused", 20)
        p.send(12)
        p.observe("new-session-cross-target-drains", 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    binary = args.probe.resolve()
    artifacts = args.artifacts or Path(tempfile.mkdtemp(prefix="cue-audio-recovery-"))
    artifacts.mkdir(parents=True, exist_ok=True)
    print("artifacts=%s probe_sha256=%s" %
          (artifacts.resolve(), hashlib.sha256(binary.read_bytes()).hexdigest()), flush=True)
    failures = []
    for target in (250, 0, 2000):
        p = Probe(binary, target, artifacts)
        try:
            scenario(p)
        except (AssertionError, OSError, ValueError) as error:
            failures.append("%sms: %s" % (target, error))
            print("FAIL " + failures[-1], flush=True)
        finally:
            p.stop(artifacts)
    print("%s: %s/3 scenarios passed; artifacts=%s" %
          ("FAIL" if failures else "PASS", 3 - len(failures), artifacts.resolve()), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
