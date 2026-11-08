#!/usr/bin/env python3
"""Real loopback WS/UDP processor paths; raw evidence and owned-child cleanup.

No device, model, public network or performance claim. Decoder helper uses the
same H264Decoder as production. Only owned instrumented children get preload.
"""
import sys
if sys.flags.optimize:
    raise SystemExit('assertion-based test requires Python without optimization')
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
from test_cued_reassembly import WebSocket, free_tcp_port, IDR, packet


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cued', type=Path, required=True)
    ap.add_argument('--decoder', type=Path, required=True)
    ap.add_argument('--artifacts', type=Path)
    ap.add_argument('--sanitizer', choices=['none', 'address,undefined', 'thread'], default='none')
    args = ap.parse_args()
    out = args.artifacts or Path(tempfile.mkdtemp(prefix='cue-processors-'))
    out.mkdir(parents=True, exist_ok=True)
    assert not (out / 'summary.json').exists(), 'preserve previous evidence'
    record = {'result': 'pending', 'cases': [], 'sanitizer': args.sanitizer,
              'cued_sha256': hashlib.sha256(args.cued.read_bytes()).hexdigest(),
              'decoder_sha256': hashlib.sha256(args.decoder.read_bytes()).hexdigest()}
    env = os.environ.copy()
    if args.sanitizer != 'none':
        library = 'libtsan.so' if args.sanitizer == 'thread' else 'libasan.so'
        env['LD_PRELOAD'] = subprocess.check_output(['g++-14', '-print-file-name=' + library], text=True).strip()
        env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1:print_stacktrace=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',
                   TSAN_OPTIONS='halt_on_error=1:second_deadlock_stack=1')
    try:
        for name, initial, dimensions in [('passthrough', False, (64,64)),
                                          ('transcode', True, (64,64)),
                                          ('scale', True, (32,48))]:
            case = {'name': name, 'phases': [], 'controls': []}
            record['cases'].append(case)
            p = ws = None
            try:
                with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp, (out / (name+'.log')).open('wb') as log:
                    udp.bind(('127.0.0.1',0)); udp.settimeout(3)
                    port = free_tcp_port()
                    argv = [str(args.cued.resolve()), '--bind','127.0.0.1','--ws-port',str(port),
                            '--probe-port',str(udp.getsockname()[1]),'--max-sessions','1',
                            '--fec','off','--jitter','off','--max-ingress-lag-ms','0']
                    if initial:
                        argv += ['--transcode','on','--width',str(dimensions[0]),'--height',str(dimensions[1]),
                                 '--preset','ultrafast','--threads','1','--gop','5','--bitrate','500']
                    case['argv'] = argv
                    p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env)
                    case['pid'] = p.pid
                    def control(op, values=None):
                        with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as sock:
                            sock.settimeout(3); sock.connect(f'/tmp/cue/{p.pid}/control.sock')
                            req={'id':1,'op':op,'args':values or {}}
                            sock.sendall(json.dumps(req).encode()); response=json.loads(sock.recv(200000))
                            case['controls'].append({'request':req,'response':response})
                            assert response['ok'], response
                            return response['result']
                    deadline=time.monotonic()+10
                    while True:
                        assert p.poll() is None, 'cued exited before listen'
                        try:
                            ws=WebSocket(port); control('session.list'); break
                        except (ConnectionRefusedError,FileNotFoundError):
                            assert time.monotonic()<deadline,'listen timeout';time.sleep(.05)
                    phases=[('initial',initial,dimensions,{})]
                    if name=='scale':
                        phases += [('resized',True,(48,32),{'width':48,'height':32,'bitrate_kbps':650}),
                                   ('disabled',False,(64,64),{'transcode':'off'}),
                                   ('enabled',True,(48,32),{'transcode':'on'})]
                    fid=0
                    for phase,on,dims,updates in phases:
                        for key,value in updates.items():
                            control('pipeline.set_param',{'key':key,'value':value})
                        data_path=out / (name+'-'+phase+'.frames')
                        info={'name':phase,'transcode':on,'width':dims[0],'height':dims[1],
                              'data':str(data_path),'received':0,'headers':[]}
                        case['phases'].append(info)
                        with data_path.open('wb') as stream:
                            for _ in range(30):
                                pts=1000000+fid*40000
                                ws.send(packet(fid,0,1,pts,IDR))
                                parts={}; count=None
                                while count is None or len(parts)<count:
                                    raw=udp.recv(65535)
                                    magic,typ,flags,codec,sid,got,index,count,got_pts=struct.unpack('<BBBBIIHHQ',raw[:24])
                                    assert magic==0xD1 and typ==1 and got==fid and got_pts==pts and codec==1
                                    assert index<count
                                    parts[index]=raw[24:]
                                data=b''.join(parts[i] for i in range(count))
                                if not on: assert data==IDR and flags&1, 'passthrough bytes/keyframe changed'
                                stream.write(struct.pack('<I',len(data))+data)
                                info['received']+=1
                                info['headers'].append({'frame_id':got,'pts':got_pts,'codec':codec,'flags':flags})
                                fid+=1;time.sleep(.04)
                        if on:
                            assert info['headers'][0]['flags']&1, 'new encoder output must start with keyframe'
                            assert any(not h['flags']&1 for h in info['headers']), 'output keyframe flags blindly follow IDR input'
                        # Publish a real snapshot after processing, even if its ordinary 1 Hz tick just passed.
                        time.sleep(1.05);ws.send(b'{}',opcode=1)
                        deadline=time.monotonic()+3
                        while True:
                            rows=control('session.list')['sessions']
                            row=next((r for r in rows if r['state']=='live' and r.get('stats',{}).get('frames_out')==fid),None)
                            if row:break
                            assert time.monotonic()<deadline,'snapshot timeout';time.sleep(.02)
                        info['snapshot']=row
                        stats=row['stats']
                        assert stats['frames_in']==fid and stats['decoded']==fid and stats['decode_errors']==0
                        assert stats['encode_errors']==0 and stats['encode_delays']==0
                        for stage in ['recv','buffer','decode','op','send']: assert stats[stage] is not None,(phase,stage)
                        if on: assert stats['encode'] is not None,'transcode encode samples absent'
                        if name=='passthrough': assert stats['encode'] is None,'passthrough invented encode sample'
                    ws.close();ws=None
                    deadline=time.monotonic()+3
                    while True:
                        closed=[r for r in control('session.list')['sessions'] if r['state']=='closed']
                        if closed:break
                        assert time.monotonic()<deadline,'final snapshot timeout';time.sleep(.02)
                    case['closed']=closed[0]
                    assert closed[0]['stats']['frames_out']==fid and closed[0]['stats']['frames_in']==fid
                    events=[json.loads(line[len('[cue-event] '):]) for line in (out/(name+'.log')).read_text().splitlines()
                            if line.startswith('[cue-event] ')]
                    case['events']=events
                    configured=[e for e in events if e['ev']=='pipeline.encoder' and e['data'].get('phase')=='configured']
                    assert len(configured)==(3 if name=='scale' else int(initial)),len(configured)
                    if name=='scale':
                        assert any(e['ev']=='pipeline.encoder_rebuild' and e['data'].get('reason')=='param_change' for e in events)
                        assert configured[-1]['data']['width']==48 and configured[-1]['data']['height']==32
                        assert configured[-1]['data']['bitrate_kbps']==650
                    control('pipeline.stop')
                    p.wait(timeout=8)
            finally:
                if ws:ws.close()
                if p:
                    if p.poll() is None:p.terminate()
                    try:p.wait(timeout=8)
                    except subprocess.TimeoutExpired:p.kill();p.wait(timeout=5)
                    case.update(exit_code=p.returncode,owned_pid_reaped=True)
            # cued's sigwait thread deliberately exits 128+SIGTERM for control stop.
            assert case['exit_code']==143,('cued exit',case['exit_code'])
            assert any(c['request']['op']=='pipeline.stop' and c['response']['ok']
                       for c in case['controls']), 'successful stop control absent'
            for phase in case['phases']:
                # Run serially after cued exit. The helper contains actual production H264Decoder.
                command=[str(args.decoder.resolve()),'--decode',phase['data'],str(phase['width']),str(phase['height'])]
                result=subprocess.run(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
                phase.update(decode_argv=command,decode_exit=result.returncode,decode_output=result.stdout)
                assert result.returncode==0,phase
        record['result']='pass'
    except Exception as error:
        record.update(result='fail',error=repr(error))
    (out/'summary.json').write_text(json.dumps(record,indent=2))
    print(json.dumps({'result':record['result'],'error':record.get('error'),'artifacts':str(out)}))
    return 0 if record['result']=='pass' else 1

if __name__=='__main__':raise SystemExit(main())
