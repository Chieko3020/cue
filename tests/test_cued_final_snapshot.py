#!/usr/bin/env python3
"""Real WS/Opus close accounting via cued's control socket; stdlib only.

UDP receipt synchronizes input processing, not a general delivery guarantee.
Artifacts retain exact binary identity, commands, closed records and owned PID exit.
"""
import sys

if sys.flags.optimize:
    raise SystemExit('assertion-based test requires Python without -O/PYTHONOPTIMIZE')

import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from test_cued_reassembly import WebSocket, free_tcp_port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cued', type=Path, required=True)
    ap.add_argument('--artifacts', type=Path)
    args = ap.parse_args()
    out = args.artifacts or Path(tempfile.mkdtemp(prefix='cue-final-snapshot-'))
    out.mkdir(parents=True, exist_ok=True)
    assert not (out / 'summary.json').exists(), 'preserve prior evidence'
    record = {'result': 'pending', 'cases': [], 'cued_sha256': hashlib.sha256(args.cued.read_bytes()).hexdigest()}
    p = None
    ws = None
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, (out / 'cued.log').open('wb') as log:
            udp.bind(('127.0.0.1', 0))
            udp.settimeout(3)
            port = free_tcp_port()
            argv = [str(args.cued.resolve()), '--bind', '127.0.0.1', '--ws-port', str(port),
                    '--probe-port', str(udp.getsockname()[1]), '--max-sessions', '1',
                    '--transcode', 'off', '--fec', 'off']
            record['argv'] = argv
            p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
            record['pid'] = p.pid

            def control(op, values=None):
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
                    sock.settimeout(3)
                    sock.connect(f'/tmp/cue/{p.pid}/control.sock')
                    sock.sendall(json.dumps({'id': 1, 'op': op, 'args': values or {}}).encode())
                    response = json.loads(sock.recv(200000))
                    assert response['ok'], response
                    return response['result']

            deadline = time.monotonic() + 8
            while True:
                assert p.poll() is None, 'cued exited before listen'
                try:
                    ws = WebSocket(port)
                    control('session.list')
                    break
                except (ConnectionRefusedError, FileNotFoundError):
                    assert time.monotonic() < deadline, 'listen timeout'
                    time.sleep(.05)
            for name, warm, count, close in [('short', False, 7, 'eof'),
                                              ('tail_after_tick', True, 5, 'graceful'),
                                              ('empty', False, 0, 'eof'),
                                              ('control_kill', False, 3, 'kill')]:
                if ws is None:
                    ws = WebSocket(port)
                deadline = time.monotonic() + 3
                while True:
                    live = [s for s in control('session.list')['sessions'] if s['state'] == 'live']
                    if live:
                        break
                    assert time.monotonic() < deadline, 'live record timeout'
                    time.sleep(.01)
                sid = live[0]['session_id']
                started = time.monotonic()
                if warm:
                    time.sleep(1.05)
                    ws.send(b'{}', opcode=1)  # drive the receive-dependent 1 Hz snapshot
                    deadline = time.monotonic() + 3
                    while not next(s for s in control('session.list')['sessions'] if s['session_id'] == sid)['stats']:
                        assert time.monotonic() < deadline, 'tick not published'
                        time.sleep(.01)
                for i in range(count):
                    body = bytes.fromhex('f8fffe')  # valid 20 ms Opus silence: 960 samples
                    ws.send(struct.pack('<BBBBIIIIQI', 0xC1, 2, 3, 0, 1, i, 0, 1, i * 20000, len(body)) + body)
                    data = udp.recv(65535)
                    assert data[:2] == b'\xd1\x02' and data[24:] == body, data.hex()
                if close == 'kill':
                    control('session.kill', {'session_id': sid})
                elif close == 'graceful':
                    ws.send(struct.pack('!H', 1000), opcode=8)
                ws.close()
                ws = None
                deadline = time.monotonic() + 3
                while True:
                    rows = control('session.list')['sessions']
                    closed = [s for s in rows if s['session_id'] == sid and s['state'] == 'closed']
                    if closed:
                        break
                    assert time.monotonic() < deadline, 'closed record timeout'
                    time.sleep(.01)
                row = closed[0]
                record['cases'].append({'name': name, 'sent': count, 'elapsed_s': time.monotonic() - started, 'closed': row})
                expected = {'audio_frames_in': count, 'audio_frames_out': count,
                            'audio_decoded_frames': count, 'audio_pcm_frames': count * 960,
                            'audio_bytes_in': count * 3, 'audio_decode_errors': 0}
                for key, value in expected.items():
                    assert row['stats'].get(key) == value, (name, key, row['stats'].get(key), value)
                if name == 'short':
                    assert record['cases'][-1]['elapsed_s'] < 1, 'short session exceeded one tick'
                if close == 'kill':
                    assert row['reason'] == 'session.kill', row
                # Existing reporting pauses when no sessions remain. A media-free
                # successor drives a real live-only aggregate without changing that policy.
                ws = WebSocket(port)
                deadline = time.monotonic() + 3
                while True:
                    aggregate = control('metrics.query', {'window_ms': 1000})
                    if (aggregate.get('samples', 0) > 0 and aggregate['t_us'] > row['last_seen_us']
                            and any(s['session_id'] > sid for s in aggregate.get('per_session', []))
                            and all(s['session_id'] != sid for s in aggregate['per_session'])):
                        break
                    assert time.monotonic() < deadline, 'post-close aggregate timeout'
                    time.sleep(.02)
                record['cases'][-1]['post_close_aggregate'] = aggregate
                assert aggregate['audio_frames_in'] == 0, 'closed counters leaked into live aggregate'
                # Check the actual emitted event too: an earlier cached zero or
                # an event from the closed session cannot establish this property.
                deadline = time.monotonic() + 3
                while True:
                    samples = [json.loads(line[len('[cue-event] '):])
                               for line in (out / 'cued.log').read_text().split('\n')[:-1]
                               if line.startswith('[cue-event] ')]
                    samples = [event for event in samples
                               if event['ev'] == 'metric.sample'
                               and event['t'] > row['last_seen_us']
                               and len(event['data'].get('per_session', [])) == 1
                               and event['data']['per_session'][0]['session_id'] > sid]
                    if samples:
                        break
                    assert time.monotonic() < deadline, 'successor metric.sample timeout'
                    time.sleep(.02)
                sample = samples[-1]
                assert sample['data']['audio_frames_in'] == 0, sample
                record['cases'][-1]['post_close_metric_sample'] = sample
            record['result'] = 'pass'
    except Exception as error:
        record.update(result='fail', error=repr(error))
    finally:
        if ws is not None:
            ws.close()
        if p is not None:
            if p.poll() is None:
                p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=5)
            record.update(cleanup_exit_code=p.returncode, owned_pid_reaped=True)
        (out / 'summary.json').write_text(json.dumps(record, indent=2))
    print(json.dumps({'result': record['result'], 'error': record.get('error'), 'artifacts': str(out)}))
    return 0 if record['result'] == 'pass' else 1


if __name__ == '__main__':
    raise SystemExit(main())
