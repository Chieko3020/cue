#!/usr/bin/env python3
"""Real WS -> cued -> captured UDP -> production probe, changing source A/B/A.

Owned children only; raw packets, frame data, commands, events and final snapshots
are preserved. SDL dummy proves engineering adaptation, never device acceptance.
"""
import sys
if sys.flags.optimize:
    raise SystemExit('assertion-based test requires Python without optimization')
import argparse
import hashlib
import json
import os
import re
import signal
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import traceback
from test_cued_reassembly import WebSocket, free_tcp_port, packet


def upstream(fid, pts, data):
    # The shared reassembly fixture defaults to IDR; this fixture has real P frames.
    nals = re.split(b'\x00\x00(?:\x00)?\x01', data)
    key = any(nal and nal[0]&31 == 5 for nal in nals)
    raw = bytearray(packet(fid,0,1,pts,data)); raw[3] = int(key)
    assert struct.unpack_from('<BBBB',raw)==(0xC1,1,1,int(key)), 'wire magic/type/codec/flags'
    return bytes(raw)


def frames(path):
    raw = path.read_bytes(); out = []; pos = 0
    while pos < len(raw):
        size, = struct.unpack_from('<I', raw, pos); pos += 4
        out.append(raw[pos:pos+size]); pos += size
    assert pos == len(raw) and len(out) == 20
    return out


def events(path):
    return [json.loads(line[len('[cue-event] '):]) for line in path.read_text(errors='replace').splitlines()
            if line.startswith('[cue-event] ')]


def main():
    ap = argparse.ArgumentParser()
    for name in ['cued', 'probe', 'fixture', 'decoder']:
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--artifacts', type=Path)
    ap.add_argument('--sanitizer', choices=['none', 'address,undefined', 'thread'], default='none')
    ap.add_argument('--baseline', action='store_true')
    ap.add_argument('--no-render', action='store_true')
    args = ap.parse_args()
    out = args.artifacts or Path(tempfile.mkdtemp(prefix='cue-dynamic-'))
    out.mkdir(parents=True, exist_ok=True)
    assert not (out/'summary.json').exists(), 'preserve prior evidence'
    record = {'result':'pending', 'cases':[], 'baseline':args.baseline, 'sanitizer':args.sanitizer,
              'sha256':{k:hashlib.sha256(getattr(args,k).read_bytes()).hexdigest() for k in ['cued','probe','fixture','decoder']}}
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy')
    if args.sanitizer != 'none':
        lib = 'libtsan.so' if args.sanitizer=='thread' else 'libasan.so'
        env['LD_PRELOAD'] = subprocess.check_output(['g++-14','-print-file-name='+lib],text=True).strip()
        env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:print_stacktrace=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', TSAN_OPTIONS='halt_on_error=1:second_deadlock_stack=1')
    try:
        gen = [str(args.fixture.resolve()), '--generate', str(out/'fixtures')]
        result = subprocess.run(gen, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        record['generate'] = {'argv':gen,'exit':result.returncode,'raw':result.stdout}
        assert result.returncode == 0, record['generate']
        source = {64:frames(out/'fixtures/64.frames'),96:frames(out/'fixtures/96.frames')}
        for name in (['auto'] if args.baseline else ['passthrough','fixed','auto']):
            case = {'name':name,'phases':[],'controls':[]};record['cases'].append(case)
            cued = probe = ws = None
            cued_log = out/(name+'-cued.log'); probe_log = out/(name+'-probe.log')
            try:
                with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as relay, cued_log.open('wb') as clog, probe_log.open('wb') as plog:
                    udp.bind(('127.0.0.1',0));udp.settimeout(4)
                    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reservation:
                        reservation.bind(('127.0.0.1',0)); probe_addr=reservation.getsockname()
                    pcommand=[str(args.probe.resolve()),'--bind','127.0.0.1','--port',str(probe_addr[1]),'--jitter','off','--av-delay-ms','0']
                    if not args.no_render:pcommand += ['--render']
                    probe=subprocess.Popen(pcommand,stdout=plog,stderr=subprocess.STDOUT,env=env)
                    case.update(probe_argv=pcommand,probe_pid=probe.pid)
                    port=free_tcp_port()
                    command=[str(args.cued.resolve()),'--bind','127.0.0.1','--ws-port',str(port),'--probe-port',str(udp.getsockname()[1]),'--max-sessions','1','--fec','off','--jitter','off','--max-ingress-lag-ms','0']
                    if name!='passthrough':command += ['--transcode','on','--width',str(32 if name=='fixed' else 0),'--height',str(48 if name=='fixed' else 0),'--preset','ultrafast','--threads','1','--gop','5','--bitrate','500']
                    cued=subprocess.Popen(command,stdout=clog,stderr=subprocess.STDOUT,env=env)
                    case.update(cued_argv=command,cued_pid=cued.pid)
                    def control(op):
                        with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as sock:
                            sock.settimeout(3);sock.connect(f'/tmp/cue/{cued.pid}/control.sock')
                            req={'id':1,'op':op,'args':{}};sock.sendall(json.dumps(req).encode())
                            response=json.loads(sock.recv(200000));case['controls'].append({'request':req,'response':response})
                            assert response['ok'],response
                            return response['result']
                    deadline=time.monotonic()+12
                    while 'listening udp' not in probe_log.read_text(errors='replace'):
                        assert probe.poll() is None and time.monotonic()<deadline,'probe listen timeout'
                        time.sleep(.05)
                    while True:
                        assert cued.poll() is None and probe.poll() is None,'child exited'
                        try:ws=WebSocket(port);control('session.list');break
                        except (ConnectionRefusedError,FileNotFoundError):
                            if ws:ws.close();ws=None
                            assert time.monotonic()<deadline,'listen timeout';time.sleep(.05)
                    fid=0
                    prelude=0
                    if name=='passthrough':
                        # Actual standalone parameter packet; no output is expected until IDR.
                        split=re.split(b'\x00\x00(?:\x00)?\x01',source[64][0])
                        config=b''.join(b'\x00\x00\x00\x01'+nal for nal in split if nal and nal[0]&31 in [7,8])
                        assert config
                        ws.send(upstream(0,960000,config))
                        raw=udp.recv(65535);relay.sendto(raw,probe_addr)
                        (out/'passthrough-config.datagram').write_bytes(raw)
                        assert raw[24:]==config
                        time.sleep(1.1)
                        initial=[e['data'] for e in events(probe_log) if 'decode_width' in e.get('data',{})]
                        assert initial and initial[-1]['decode_width'] is None and initial[-1]['decoded']==0
                        assert initial[-1]['decode_errors']==0 and initial[-1]['parameter_packets']==1
                        case['parameter_only_probe_metric']=initial[-1]
                        fid=1;prelude=1
                    for phase,width in enumerate([64,96,64]):
                        expected=(32,48) if name=='fixed' else ((64,64) if args.baseline else (width,80 if width==96 else 64))
                        path=out/f'{name}-{phase}.frames'
                        info={'phase':phase,'source_width':width,'expected':expected,'headers':[],'data':str(path)};case['phases'].append(info)
                        with path.open('wb') as stream, (out/f'{name}-{phase}.datagrams').open('wb') as packets:
                            for data in source[width]:
                                pts=1000000+fid*40000
                                ws.send(upstream(fid,pts,data))
                                parts={};count=None
                                while count is None or len(parts)<count:
                                    raw=udp.recv(65535)
                                    magic,typ,flags,codec,sid,got,index,count,got_pts=struct.unpack('<BBBBIIHHQ',raw[:24])
                                    assert magic==0xD1 and typ==1 and got==fid and got_pts==pts and codec==1
                                    packets.write(struct.pack('<I',len(raw))+raw)
                                    relay.sendto(raw,probe_addr);parts[index]=raw[24:]
                                encoded=b''.join(parts[i] for i in range(count))
                                if name=='passthrough':assert encoded==data and flags&1==upstream(fid,pts,data)[3], 'passthrough bytes/keyframe'
                                stream.write(struct.pack('<I',len(encoded))+encoded)
                                info['headers'].append({'frame_id':got,'pts':got_pts,'codec':codec,'flags':flags})
                                fid+=1;time.sleep(.04)
                        time.sleep(1.1);ws.send(b'{}',opcode=1)
                        deadline=time.monotonic()+4
                        while True:
                            rows=control('session.list')['sessions']
                            row=next((r for r in rows if r['state']=='live' and r.get('stats',{}).get('frames_out')==fid),None)
                            if row:break
                            assert time.monotonic()<deadline,'snapshot timeout';time.sleep(.05)
                        info['snapshot']=row
                        stats=row['stats'];assert stats['decoded']==fid-prelude and stats['decode_errors']==0 and stats['encode_errors']==0,stats
                        if not args.baseline:
                            assert stats['decode_width']==width and stats['decode_output_changes']==phase and stats['sps_changes']==phase and stats['decode_resets']==0,stats
                        metrics=[e['data'] for e in events(probe_log) if 'decode_width' in e.get('data',{}) and e['data'].get('decoded')==fid-prelude]
                        if not args.baseline:
                            assert metrics and metrics[-1]['decode_width']==expected[0] and metrics[-1]['decode_height']==expected[1],metrics
                            assert metrics[-1]['decode_errors']==0 and metrics[-1]['decode_resets']==0,metrics[-1]
                        info['probe_metrics']=metrics[-1] if metrics else None
                    ws.close();ws=None
                    deadline=time.monotonic()+4
                    while True:
                        rows=control('session.list')['sessions'];closed=[r for r in rows if r['state']=='closed']
                        if closed:break
                        assert time.monotonic()<deadline,'closed timeout';time.sleep(.05)
                    case['closed']=closed[0]
                    assert closed[0]['stats']['decoded']==60 and closed[0]['stats']['frames_out']==60+prelude
                    if not args.baseline:
                        assert closed[0]['stats']['decode_output_changes']==2 and closed[0]['stats']['decode_width']==64
                        assert closed[0]['stats']['parameter_packets']==prelude
                    case['events']=events(cued_log);case['probe_events']=events(probe_log)
                    configured=[e for e in case['events'] if e['ev']=='pipeline.encoder' and e['data'].get('phase')=='configured']
                    assert len(configured)==(1 if args.baseline or name=='fixed' else 3 if name=='auto' else 0),configured
                    if name=='auto' and not args.baseline:
                        assert len([e for e in case['events'] if e['ev']=='pipeline.encoder_rebuild' and e['data'].get('reason')=='source_change'])==2
                    if not args.no_render:
                        assert '降级为只算指标' not in probe_log.read_text(), 'SDL renderer failed'
                    control('pipeline.stop');cued.wait(timeout=8)
            finally:
                if ws:ws.close()
                for key,child in [('cued',cued),('probe',probe)]:
                    if child:
                        if child.poll() is None:child.terminate()
                        try:child.wait(timeout=8)
                        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
                        case[key+'_exit']=child.returncode;case[key+'_owned_pid_reaped']=True
            assert case['cued_exit']==143,case
            # SDL handles SIGTERM as a quit event; headless has default signal exit.
            assert case['probe_exit']==(-signal.SIGTERM if args.no_render else 0),case
            for info in case['phases']:
                cmd=[str(args.decoder.resolve()),'--decode',info['data'],*[str(n) for n in info['expected']]]
                result=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
                info.update(decode_argv=cmd,decode_exit=result.returncode,decode_raw=result.stdout)
                assert result.returncode==0,info
                if args.baseline and info['source_width']==96:
                    desired=[str(args.decoder.resolve()),'--decode',info['data'],'96','80']
                    red=subprocess.run(desired,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
                    info.update(desired_decode_argv=desired,desired_decode_exit=red.returncode,desired_decode_raw=red.stdout)
                    assert red.returncode!=0,'unfixed automatic target unexpectedly follows source'
        record['result']='pass'
    except Exception as error:record.update(result='fail',error=repr(error),traceback=traceback.format_exc())
    (out/'summary.json').write_text(json.dumps(record,indent=2))
    print(json.dumps({'result':record['result'],'error':record.get('error'),'artifacts':str(out)}))
    return 0 if record['result']=='pass' else 1

if __name__=='__main__':raise SystemExit(main())
