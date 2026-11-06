#!/usr/bin/env python3
"""Fixed-input real cued trace test; no model/device/network changes.
Usage: python3 scripts/test-injection-trace.py --cued build/cued --out /tmp/cue-trace
"""
import argparse, hashlib, json, pathlib, re, socket, subprocess, threading, time

ROOT = pathlib.Path(__file__).resolve().parents[1]

def available_port(kind):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cued', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    binary = pathlib.Path(args.cued).resolve()
    # Use the same known-decodable IDR as the existing media smoke test. Its
    # source and actual payload hash are retained; timestamps alone are live.
    text = (ROOT / 'scripts/test-client.mjs').read_text()
    payload = re.search(r"const IDR_ANNEXB = Buffer.from\(\s*'([^']+)'", text).group(1)
    import base64
    body = base64.b64decode(payload)
    sender = '''
import {packMediaPackets, TYPE_VIDEO, CODEC_H264} from '%s';
const socket = new WebSocket(process.argv[1]);
const timeout = setTimeout(() => { console.error('sender timeout'); process.exit(2); }, 12000);
await new Promise((ok, bad) => { socket.onopen = ok; socket.onerror = bad; });
const t = Math.round(performance.now()*1000);
const pong = new Promise((resolve) => socket.onmessage = e => { const j=JSON.parse(e.data); if(j.op==='pong') resolve(j); });
socket.send(JSON.stringify({op:'ping',t}));
const reply = await pong;
const offset=reply.server_us-Math.round(performance.now()*1000);
const bytes=Buffer.from('%s','base64');
let packets=0;
for(let frame=0;frame<96;frame++) {
 const data=packMediaPackets({type:TYPE_VIDEO,codec:CODEC_H264,streamId:1,frameId:frame,keyframe:true,ptsUs:Math.max(0,Math.round(performance.now()*1000+offset))},bytes);
 for(const packet of data) { socket.send(packet); packets++; }
 await new Promise(r=>setTimeout(r,15));
}
await new Promise(r=>setTimeout(r,600)); socket.close(); clearTimeout(timeout);
console.log(JSON.stringify({frames:96,upstream_packets:packets,payload_bytes:bytes.length}));
''' % ((ROOT / 'scripts/lib/upstream-wire.mjs').as_uri(), payload)
    all_runs = []
    for name, seed in [('same-a',42),('same-b',42),('different',43)]:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
            receiver.bind(('127.0.0.1',0)); udp=receiver.getsockname()[1]
            receiver.settimeout(.1)
            received=[]; stop=threading.Event()
            def collect():
                while not stop.is_set():
                    try: received.append(receiver.recvfrom(65536)[0])
                    except socket.timeout: pass
            thread=threading.Thread(target=collect); thread.start()
            port=available_port(socket.SOCK_STREAM)
            trace=out/(name+'.trace')
            argv=[str(binary),'--bind','127.0.0.1','--ws-port',str(port),'--probe-host','127.0.0.1','--probe-port',str(udp),
                  '--transcode','off','--fec','off','--jitter','off','--send-mbps','100','--inject-drop','35','--inject-seed',str(seed),'--inject-trace',str(trace)]
            rec={'name':name,'seed':seed,'argv':argv}
            try:
                with (out/(name+'.log')).open('w') as log:
                    proc=subprocess.Popen(argv,cwd=ROOT,stdout=log,stderr=log)
                    rec['pid']=proc.pid
                    try:
                        for _ in range(100):
                            if proc.poll() is not None: raise RuntimeError('cued exited early')
                            try:
                                with socket.create_connection(('127.0.0.1',port),timeout=.1): break
                            except OSError: time.sleep(.03)
                        sent=subprocess.run(['node','--input-type=module','-e',sender,f'ws://127.0.0.1:{port}/'],cwd=ROOT,capture_output=True,text=True,timeout=16)
                        (out/(name+'-sender.log')).write_text(sent.stdout+sent.stderr)
                        assert sent.returncode==0,sent.stderr
                        rec['input']=json.loads(sent.stdout.strip().splitlines()[-1]); time.sleep(.2)
                    finally:
                        proc.terminate()
                        try: rec['exit_code']=proc.wait(timeout=4)
                        except subprocess.TimeoutExpired: proc.kill();rec['exit_code']=proc.wait(timeout=4)
                        rec['reaped']=True
            finally:
                stop.set();thread.join(timeout=2)
            decisions=[tuple(map(int,line.split())) for line in trace.read_text().splitlines()]
            assert decisions and all(len(d)==2 and 0<=d[0]<96 and d[1]==0 for d in decisions), decisions
            assert rec['input']['upstream_packets']==96
            # Exactly one downstream packet/frame and FEC disabled: received
            # frame ids + dropped ids must partition the complete fixed domain.
            video=[packet for packet in received if len(packet)>=24 and packet[0]==0xD1 and packet[1]==1]
            kept=[int.from_bytes(packet[8:12],'little') for packet in video]
            assert len(kept)==len(set(kept))
            assert set(kept).isdisjoint(d[0] for d in decisions)
            assert set(kept)|{d[0] for d in decisions}==set(range(96)),(len(kept),decisions)
            rec.update(dropped=decisions,received_frames=sorted(kept),trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest())
            all_runs.append(rec)
    assert all_runs[0]['dropped']==all_runs[1]['dropped']
    assert all_runs[0]['dropped']!=all_runs[2]['dropped']
    result={'passed':True,'decision_domain':'96 frames, one data fragment/frame, no FEC/retransmission, outbound packet seq 0..95',
            'input_payload_sha256':hashlib.sha256(body).hexdigest(),'cued_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
            'runs':all_runs,'scope':'Linux real cued/WS/UDP; fixed embedded synthetic IDR, no provider/device/public network'}
    (out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print('trace: same seed identical nonempty decisions, different seed different; complete 96-packet domains verified')
if __name__=='__main__': main()
