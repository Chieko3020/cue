#!/usr/bin/env python3
"""Exercise the actual HTTP backend -> query_metrics counter-presence boundary."""
import argparse, http.server, json, pathlib, subprocess, threading

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--driver',required=True);ap.add_argument('--out',required=True);args=ap.parse_args()
    out=pathlib.Path(args.out);out.mkdir(parents=True,exist_ok=True)
    keys=['queue_dropped','delayed_pkts','delayed_max_us','ingress_late','control_ops']
    current={}
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            payload=json.dumps({'ok':True,'pipeline':{'running':True},'metrics':{'samples':1,'fps':30,**current}}).encode()
            self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
        def log_message(self,*args): pass
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever);thread.start()
    records=[]
    try:
        values=[None,-1,1.5,'0',True,{},[],0,137,2**64-1,'absent']
        for n,value in enumerate(values):
            current.clear()
            if value!='absent':current.update({key:value for key in keys})
            current['control_channel']='uds'
            argv=[str(pathlib.Path(args.driver).resolve()),'--http',f'http://127.0.0.1:{server.server_port}']
            p=subprocess.run(argv,capture_output=True,text=True,timeout=10)
            (out/f'{n}.log').write_text(p.stdout+p.stderr)
            assert p.returncode==0,p.stderr
            result=json.loads(p.stdout)
            expected=value if type(value) is int and value>=0 else None
            assert all(result[key]==expected for key in keys),(value,result)
            assert result['control_channel']=='uds'
            records.append({'argv':argv,'exit_code':p.returncode,'input':value,'expected':expected})
    finally:
        server.shutdown();server.server_close();thread.join()
    (out/'summary.json').write_text(json.dumps({'passed':True,'cases':records},indent=2)+'\n')
    print('HTTP metrics boundary: 11 actual query_metrics cases passed')
if __name__=='__main__':main()
