#!/usr/bin/env python3
"""Run one resource-bounded Linux lane; preserve configure/build/CTest and identity.

Use --compare with four result directories to check registration parity. An absent,
skipped, disabled or uninstrumented C++ test fails the gate. No installs/retries.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
INVENTORY = json.loads((ROOT / 'scripts/ci-test-inventory.json').read_text())
NON_CPP = {'layer_isolation', 'no_external_cdn', 'console_client', 'avc_level_consistency'}
PROCESS = {'probe_audio_recovery', 'probe_audio_drift_recovery', 'recorded_metrics_http',
           'llm_deadline_http', 'eval_manual', 'eval_producer', 'cued_reassembly', 'cued_processors', 'cued_final_snapshot', 'c13_cli_bench', 'integration'}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n')


def source_identity():
    paths = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=ROOT).split(b'\0')
    files = {p.decode(): hashlib.sha256((ROOT / p.decode()).read_bytes()).hexdigest()
             for p in paths if p and (ROOT / p.decode()).is_file()}
    return {'files': files, 'sha256': hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()}


def registration(data, sdl, san):
    tests = data['tests']
    by_name = {t['name']: t for t in tests}
    # Frozen independently of add_test: deleting a registration cannot delete its expectation.
    expected = set(INVENTORY['portable_cpp']) | (set(INVENTORY['sdl_cpp']) if sdl else set())
    actual = {t['name'] for t in tests if pathlib.Path(t.get('command', [''])[0]).name.startswith('test_')}
    require(actual == expected, f'C++ registrations differ: missing={expected-actual}, extra={actual-expected}')
    expected_all = expected | NON_CPP
    if san == 'none':
        expected_all |= PROCESS if sdl else PROCESS - {'probe_audio_recovery', 'probe_audio_drift_recovery'}
    require(set(by_name) == expected_all, f'registration difference: missing={expected_all-set(by_name)}, extra={set(by_name)-expected_all}')
    for name in actual:
        props = {p['name']: p['value'] for p in by_name[name].get('properties', [])}
        require(not props.get('DISABLED'), f'{name}: disabled')
        require('SKIP_RETURN_CODE' not in props and 'SKIP_REGULAR_EXPRESSION' not in props, f'{name}: skip policy')
        if san != 'none':
            env = dict(e.split('=', 1) for e in props.get('ENVIRONMENT', []))
            require('detect_leaks=1' in env.get('ASAN_OPTIONS', ''), f'{name}: missing leak check')
            require('halt_on_error=1' in env.get('UBSAN_OPTIONS', ''), f'{name}: missing UB gate')
            require('halt_on_error=1' in env.get('TSAN_OPTIONS', ''), f'{name}: missing race gate')
    return sorted(actual)


def check_instrumentation(commands, instrument):
    if instrument:
        for item in commands:
            require(all('-fsanitize=' + s in item['command'] for s in instrument),
                    f'uninstrumented source: {item["file"]}')


def check_sdl(commands, sdl):
    probe = [x['command'] for x in commands if x['file'].endswith('/apps/cue-probe.cpp')]
    require(len(probe) == 1 and ('-DCUE_HAVE_SDL2=1' in probe[0]) == (sdl == 'ON'), 'probe SDL macro mismatch')


def lane(args):
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    build = pathlib.Path(args.build_dir).resolve()
    initial = source_identity()
    save(out / 'toolchain.json', {
        'compiler': subprocess.check_output(['g++-14', '--version'], text=True).splitlines()[0],
        'cmake': subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0],
        'flags': '-O0 -DNDEBUG -g0', 'san_debug_flags': '-g0',
        'sdl': args.sdl, 'sanitize': args.sanitize, 'build_jobs': 1})
    save(out / 'source.json', initial)
    commands = []

    def run(name, cmd):
        with (out / (name + '.log')).open('w') as log:
            result = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        commands.append({'name': name, 'argv': cmd, 'exit': result.returncode})
        save(out / 'commands.json', commands)
        print(f'{name}: exit={result.returncode}, log={out / (name + ".log")}', flush=True)
        return result.returncode

    cfg = ['cmake', '-S', str(ROOT), '-B', str(build), '-DCMAKE_CXX_COMPILER=g++-14',
           '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O0 -DNDEBUG -g0',
           '-DCUE_SAN_DEBUG_FLAGS=-g0', '-DCUE_ENABLE_SDL2=' + args.sdl,
           '-DCUE_SANITIZE=' + args.sanitize]
    if run('configure', cfg):
        return 1
    listing = subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], cwd=ROOT)
    (out / 'ctest-configured.json').write_bytes(listing)
    if run('build', ['cmake', '--build', str(build), '-j1']):
        return 1
    listing = subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1'], cwd=ROOT)
    (out / 'ctest.json').write_bytes(listing)
    data = json.loads(listing)
    cpp = registration(data, args.sdl == 'ON', args.sanitize)
    if args.sanitize == 'none':
        require(not re.search(r'warning:', (out / 'build.log').read_text()), 'regular build emitted a warning')
    compile_commands = json.loads((build / 'compile_commands.json').read_text())
    (out / 'compile_commands.json').write_text(json.dumps(compile_commands, indent=2) + '\n')
    built_test_targets = {m.group(1) for x in compile_commands
                          if (m := re.search(r'CMakeFiles/(test_\w+)\.dir/', x.get('output', x['command'])))}
    registered_targets = {pathlib.Path(t['command'][0]).stem for t in data['tests'] if t['name'] in cpp}
    require(built_test_targets == registered_targets, f'C++ target/registration mismatch: unregistered={built_test_targets-registered_targets}, unbuilt={registered_targets-built_test_targets}')
    check_sdl(compile_commands, args.sdl)
    instrument = None if args.sanitize == 'none' else args.sanitize.split(',')
    check_instrumentation(compile_commands, instrument)
    binaries = {}
    for file in build.iterdir():
        if file.is_file() and file.open('rb').read(4) == b'\x7fELF':
            binaries[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
    save(out / 'binaries.json', binaries)
    rc = run('ctest', ['ctest', '--test-dir', str(build), '-j1', '--output-on-failure',
                       '--output-junit', str(out / 'ctest.xml')])
    xml = ET.parse(out / 'ctest.xml').getroot()
    results = []
    for test in xml.iter('testcase'):
        status = 'failed' if test.find('failure') is not None or test.find('error') is not None else 'skipped' if test.find('skipped') is not None or test.attrib.get('status', 'run') != 'run' else 'passed'
        results.append({'name': test.attrib['name'], 'status': status, 'time': test.attrib.get('time')})
    require(set(t['name'] for t in results) == set(t['name'] for t in data['tests']), 'CTest result inventory mismatch')
    require(source_identity() == initial, 'source changed during lane; binary identity is stale')
    result = {'sdl': args.sdl, 'sanitize': args.sanitize, 'cpp': cpp, 'results': results,
              'source_sha256': initial['sha256'], 'ctest_exit': rc,
              'passed': rc == 0 and all(t['status'] == 'passed' for t in results)}
    save(out / 'result.json', result)
    return 0 if result['passed'] else 1


def audit_native(build_dir, sdl):
    build = pathlib.Path(build_dir)
    data = json.loads((build / 'windows-client-tests.json').read_text())
    check_sdl(json.loads((build / 'compile_commands.json').read_text()), sdl)
    selected = set(INVENTORY['native_client_cpp']) | (set(INVENTORY['sdl_cpp']) if sdl == 'ON' else set())
    by_name = {t['name']: t for t in data['tests']}
    require(selected <= set(by_name), f'native registrations missing: {selected-set(by_name)}')
    for name in selected:
        t = by_name[name]
        require('command' in t, f'{name}: missing built command')
        props = {p['name']: p['value'] for p in t.get('properties', [])}
        require(not props.get('DISABLED'), f'{name}: disabled')
        require('SKIP_RETURN_CODE' not in props and 'SKIP_REGULAR_EXPRESSION' not in props, f'{name}: skip policy')
    xml = ET.parse(build / 'windows-client-results.xml').getroot()
    results = list(xml.iter('testcase'))
    require(len(results) == len(selected) and {t.attrib['name'] for t in results} == selected, 'native result inventory mismatch')
    require(all(t.find('skipped') is None and t.find('failure') is None and t.find('error') is None
               and t.attrib.get('status', 'run') == 'run' for t in results), 'native failed/skipped result')
    print(f'native SDL={sdl}: exact selected inventory {len(selected)}/{len(selected)} passed; zero skipped')
    return 0


def compare(dirs):
    lanes = [json.loads((pathlib.Path(d) / 'result.json').read_text()) for d in dirs]
    require(len(lanes) == 4 and len({(x['sdl'], x['sanitize']) for x in lanes}) == 4, 'regression invariant failed')
    require({(x['sdl'], x['sanitize']) for x in lanes} == {('ON', 'none'), ('OFF', 'none'), ('ON', 'address,undefined'), ('ON', 'thread')}, 'regression invariant failed')
    require(len({x['source_sha256'] for x in lanes}) == 1, 'source identities differ')
    portable = [set(x['cpp']) - {'probe_render_sdl'} for x in lanes]
    require(all(p == portable[0] for p in portable), 'portable C++ inventory differs')
    for x in lanes:
        print(f"SDL={x['sdl']} sanitizer={x['sanitize']}: cpp={len(x['cpp'])} total={len(x['results'])} passed={x['passed']}")
    return 0 if all(x['passed'] for x in lanes) else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir')
    parser.add_argument('--out')
    parser.add_argument('--sdl', choices=['ON', 'OFF'])
    parser.add_argument('--sanitize', choices=['none', 'address,undefined', 'thread'], default='none')
    parser.add_argument('--audit-native', metavar='BUILD_DIR')
    parser.add_argument('--compare', nargs=4, metavar='RESULT_DIR')
    args = parser.parse_args()
    if args.audit_native:
        if args.sdl is None:
            parser.error('--audit-native requires --sdl')
        sys.exit(audit_native(args.audit_native, args.sdl))
    if not args.compare and not all([args.build_dir, args.out, args.sdl]):
        parser.error('lane requires --build-dir, --out and --sdl')
    sys.exit(compare(args.compare) if args.compare else lane(args))
