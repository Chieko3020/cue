#!/usr/bin/env python3
"""CLI semantics and report unknown-value regression, using actual executables."""
import sys

if sys.flags.optimize:
    raise SystemExit('assertion-based test requires Python without -O/PYTHONOPTIMIZE')

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--eval', required=True)
args = ap.parse_args()
root = Path(__file__).resolve().parents[1]
help_run = subprocess.run([args.eval, '--help'], capture_output=True, text=True)
assert help_run.returncode == 0
for phrase in ['--mode scripted：', '不起真实管线', 'scripted/oracle', '不证明真实 LLM']:
    assert phrase in help_run.stdout, phrase
for options in [['--mode', 'invalid'], ['--mode', 'scripted', '--live'],
                ['--mode', 'scripted', '--producer', 'lavfi']]:
    bad = subprocess.run([args.eval] + options, capture_output=True, text=True)
    assert bad.returncode == 2, (options, bad.returncode, bad.stderr)
with tempfile.TemporaryDirectory(prefix='cue-c13-fixture-') as temporary:
    work = Path(temporary)
    (work / 'raw').mkdir()
    meta = {'stamp': 'fixture', 'iso': 'fixture', 'host': 'fixture', 'nproc': 'unknown',
            'mem_gib': 'unknown', 'build_dir': 'unknown', 'build_type': 'unknown',
            'compiler': 'unknown', 'seconds_per_run': 2, 'fps': 30, 'settle_secs': 3,
            'drop_pct_for_lossy': 7, 'sample': {'name': 'unknown', 'width': 'unknown',
            'height': 'unknown', 'fps': 'unknown', 'frames': 1, 'avg_frame_bytes': 1,
            'avg_fragments': 1}, 'scenarios': [{'label': 'missing', 'drop_pct': 7,
            'transcode': 'off', 'fec': False, 'nack': False}, {'label': 'clean',
            'drop_pct': 0, 'transcode': 'off', 'fec': True, 'nack': True}]}
    (work / 'run.json').write_text(json.dumps(meta))
    (work / 'raw/clean.cued.log').write_text('METRICS ' + json.dumps({'recv': {'p50': 2, 'p99': 4}, 'buffer': {'p50': 7, 'p99': 9}}) + '\n')
    (work / 'raw/clean.probe.log').write_text('[probe] session=2 fps=30.0 frames=11 lost=2 pend=0 pkts=50 nack=3 sup=0 pm=0ms mode=normal pause=4 fec_rec=5 fec_unrec=0 ghost=0/0 rej=6 rpend=0/0% loss=7.0%\n')
    report = subprocess.run(['node', str(root / 'scripts/bench-report.mjs'), str(work)],
                            capture_output=True, text=True, check=True).stdout
    assert '同口径（M2a' not in report
    assert '抖动缓冲中等待放行' in report and '| 2.000 | 7.000 |' in report
    assert '| 50 | 11 |' in report
    assert '7% 丢包' in report and 'unknownxunknown，ffprobe r_frame_rate=unknown' in report
    assert '未完成测量' in report and '| missing | off | off | n/a | n/a | n/a |' in report
    # The existing m4 suite is a short scripted selftest with constructed observations.
    run = subprocess.run([args.eval, '--mode', 'scripted', '--suite', str(root / 'suites/m4-diagnosis.json'),
                          '--repeat', '1', '--out', str(work / 'runs'), '--no-baseline'],
                         cwd=root, capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, (run.returncode, run.stderr, run.stdout[-3000:])
    reports = list((work / 'runs').glob('*/report.md'))
    assert reports and '自检' in reports[0].read_text()
print('PASS: actual help, usage errors, scripted selftest and bench unknown/source fixture')
