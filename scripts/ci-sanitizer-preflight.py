#!/usr/bin/env python3
"""Exercise the selected compiler runtime once, without changing host settings."""
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('sanitizer', choices=['address,undefined', 'thread'])
parser.add_argument('--compiler', default='g++-14')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='cue-sanitizer-preflight-') as directory:
    root = pathlib.Path(directory)
    source = root / 'probe.cpp'
    source.write_text('#include <thread>\nint main(){ int value=0; std::thread t([&]{value=1;}); t.join(); return value!=1; }\n')
    binary = root / 'probe'
    subprocess.run([args.compiler, '-std=c++23', '-O0', '-g0', '-pthread',
                    '-fsanitize=' + args.sanitizer, str(source), '-o', str(binary)], check=True)
    # Match CMake's runtime ordering workaround locally, without touching preload configuration.
    import os
    env = os.environ.copy()
    runtime = 'libtsan.so' if args.sanitizer == 'thread' else 'libasan.so'
    env['LD_PRELOAD'] = subprocess.check_output([args.compiler, '-print-file-name=' + runtime], text=True).strip()
    env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1', TSAN_OPTIONS='halt_on_error=1')
    result = subprocess.run([str(binary)], env=env)
    if result.returncode:
        print('::error::Sanitizer runtime preflight failed; no host settings were changed. '
              'This is an environment failure, not a passing test suite.', flush=True)
        raise SystemExit(1)
    print('Minimal runtime preflight passed once; full CTest is still required.')
