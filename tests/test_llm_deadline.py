#!/usr/bin/env python3
"""Real libcurl/Runner timing against bounded loopback HTTP servers; no API credentials."""
import argparse,json,os,socket,subprocess,tempfile,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path

SUCCESS=json.dumps({'choices':[{'message':{'role':'assistant','content':'local ok','tool_calls':[{'id':'local1','type':'function','function':{'name':'query_metrics','arguments':'{}'}}]},'finish_reason':'tool_calls'}],'usage':{'prompt_tokens':7,'completion_tokens':3}}).encode()

class Server(ThreadingHTTPServer):
    daemon_threads=False
    block_on_close=True
    def __init__(self,kind):
        super().__init__(('127.0.0.1',0),Handler);self.kind=kind;self.rows=[];self.lock=threading.Lock();self.stop=threading.Event()

class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*_):pass
    def setup(self):
        super().setup();self.connection.settimeout(2)
    def do_POST(self):
        size=int(self.headers.get('Content-Length','0'))
        if size>1024*1024:return
        payload=json.loads(self.rfile.read(size))
        with self.server.lock:
            index=len(self.server.rows);self.server.rows.append({'at':time.monotonic(),'path':self.path,'payload':payload})
        kind=self.server.kind
        if kind=='hang':
            if self.server.stop.wait(1.3):return
            status,body=503,b'{}'
        elif kind=='disconnect':
            self.close_connection=True
            self.connection.shutdown(socket.SHUT_RDWR);self.connection.close();return
        elif kind=='success429':status,body=(429,b'{}') if index==0 else (200,SUCCESS)
        elif kind=='success503':status,body=(503,b'{}') if index==0 else (200,SUCCESS)
        elif kind=='success':status,body=200,SUCCESS
        elif kind=='overflow':status,body=200,b'x'*1024
        elif kind=='badjson':status,body=200,b'not json'
        elif kind=='400':status,body=400,b'bad request'
        else:status,body=int(kind),b'{}'
        try:
            self.send_response(status);self.send_header('Content-Length',str(len(body)));self.send_header('Connection','close');self.end_headers();self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):pass
        self.close_connection=True

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--driver',required=True);ap.add_argument('--artifacts');ap.add_argument('--baseline-only',action='store_true');ap.add_argument('--only',help='Comma-separated scenario names');a=ap.parse_args()
    out=Path(a.artifacts) if a.artifacts else Path(tempfile.mkdtemp(prefix='cue-llm-deadline-'));out.mkdir(parents=True,exist_ok=True);print('artifacts:',out,flush=True);assert not (out/'summary.json').exists(),'Preserve previous evidence'
    cases=[('shared-timeout','hang','shared-timeout'),('rate429','429','shared-timeout'),('status503','503','shared-timeout'),('disconnect','disconnect','shared-timeout'),('config-cap','hang','config-cap'),('absolute','hang','absolute'),('expired','503','expired'),('minimum','503','minimum'),('subms','503','subms'),('preset-cancel','503','preset-cancel'),('expired-cancel','503','expired-cancel'),('retry-limit','503','retry-limit'),('cancel-200','503','cancel-200'),('cancel-800','503','cancel-800'),('cancel-transfer','hang','cancel-transfer'),('runner','hang','runner'),('runner-cancel','hang','runner-cancel'),('success429','success429','success'),('success503','success503','success'),('record-expiry','success','record-expiry'),('record-cancel','success','record-cancel'),('max0','503','max0'),('status400','400','status400'),('overflow','overflow','overflow'),('badjson','badjson','badjson')]
    if a.baseline_only:cases=cases[:1]
    if a.only:
        names=set(a.only.split(','));assert names <= {x[0] for x in cases};cases=[c for c in cases if c[0] in names]
    results=[];env=os.environ.copy();env.update(NO_PROXY='127.0.0.1,localhost',no_proxy='127.0.0.1,localhost')
    for name,kind,scenario in cases:
        directory=out/name;directory.mkdir();server=Server(kind);thread=threading.Thread(target=lambda:server.serve_forever(poll_interval=.02));thread.start()
        argv=[str(Path(a.driver).resolve()),f'http://127.0.0.1:{server.server_port}',scenario,str(directory/'cassette')]
        if a.baseline_only:argv=argv[:2]
        reader=None;reader_stop=threading.Event();recorded=[];reader_errors=[]
        if name in ('record-expiry','record-cancel'):
            key=subprocess.check_output([str(Path(a.driver).resolve()),'--cassette-key'],text=True,env=env,timeout=3).strip()
            cassette=directory/'cassette';cassette.mkdir();fifo=cassette/(key+'.jsonl');os.mkfifo(fifo)
            def read_recording():
                # Opening a read end releases the real ofstream open only after
                # the request's cap/cancel has elapsed; no product hook or sleep.
                if reader_stop.wait(.6):return
                fd=os.open(fifo,os.O_RDONLY|os.O_NONBLOCK);chunks=[];end=time.monotonic()+2
                try:
                    while not reader_stop.is_set() and time.monotonic()<end:
                        try:chunk=os.read(fd,65536)
                        except BlockingIOError:
                            reader_stop.wait(.005)
                            continue
                        if chunk:chunks.append(chunk)
                        elif chunks:break
                        reader_stop.wait(.005)
                except OSError as exc:reader_errors.append(repr(exc))
                finally:os.close(fd)
                recorded.append(b''.join(chunks).decode())
            reader=threading.Thread(target=read_recording);reader.start()
        p=None;failure=None;data={};began=time.monotonic()
        try:
            with (directory/'client.log').open('w') as log:
                p=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,env=env)
                try:p.wait(timeout=12)
                finally:
                    if p.poll() is None:
                        p.terminate()
                        try:p.wait(timeout=2)
                        except subprocess.TimeoutExpired:p.kill();p.wait()
            data=json.loads((directory/'client.log').read_text())
        except Exception as exc:failure=repr(exc)
        finally:
            server.stop.set();server.shutdown();server.server_close();thread.join(timeout=3)
            if reader is not None:
                reader.join(timeout=2.5);reader_stop.set();reader.join(timeout=.1)
                (directory/'completed-recording.jsonl').write_text(''.join(recorded))
        rows=server.rows;count=len(rows);checks={'driver_exit':p is not None and p.returncode==0,'valid_result':bool(data) and failure is None,'server_stopped':not thread.is_alive()}
        if data:
            elapsed=data['elapsed_ms'];error=data['error'];success=data['success']
            checks['bounded_requests']=count<=3
            checks['execution_controls_not_http_payload']=all('deadline' not in row['payload'] and 'cancel' not in row['payload'] and 'timeout_s' not in row['payload'] for row in rows)
            # This is the original baseline regression: its threshold is unchanged.
            if name=='shared-timeout':checks.update(client_error=not success,shared_total_budget=elapsed<=1500)
            if name in ('rate429','status503','disconnect','config-cap'):
                checks.update(error=not success,total_budget=elapsed<=1500,deadline_error='deadline' in error)
            if name in ('rate429','status503','disconnect'):
                checks['retry_observed']=count>=2
                checks['requests_before_deadline']=all(row['at']-rows[0]['at']<1.05 for row in rows)
            if name=='absolute':checks.update(total_budget=elapsed<=600,deadline_error='deadline' in error,one_attempt=count==1)
            if name in ('expired','minimum','subms','preset-cancel','expired-cancel'):
                checks.update(no_http=count==0,fast_return=elapsed<250,error=not success)
                checks['error_reason']=error=='cancelled' if name in ('preset-cancel','expired-cancel') else 'deadline' in error
            if name in ('cancel-200','cancel-800'):
                checks.update(cancelled=error=='cancelled',responsive=0<=data['cancel_latency_ms']<=500,no_post_cancel_retry=count==(1 if name=='cancel-200' else 2))
            if name in ('cancel-transfer','runner-cancel'):
                checks.update(cancelled=error=='cancelled',responsive=0<=data['cancel_latency_ms']<=1500,one_attempt=count==1)
            if name=='runner':checks.update(wall_budget='wall clock budget exhausted' in error,total_budget=elapsed<=1500,one_attempt=count==1)
            if name=='retry-limit':checks.update(three_attempts=count==3,original_http_error='http 503' in error,total_budget=elapsed<=1500)
            if name.startswith('success'):
                files=list((directory/'cassette').glob('*.jsonl'))
                lines=[line for f in files for line in f.read_text().splitlines() if line]
                checks.update(response=success and data['content']=='local ok' and data['prompt_tokens']==7 and data['tool_calls']==1,two_attempts=count==2,one_recording=len(lines)==1)
            if name in ('record-expiry','record-cancel'):
                checks.update(late_io_not_success=not success,recording_really_written=bool(recorded and recorded[0]),reader_stopped=reader is not None and not reader.is_alive(),reader_no_io_error=not reader_errors,io_tail_observed=elapsed>=500)
                checks['final_reason']=error=='cancelled' if name=='record-cancel' else 'deadline' in error
            if name in ('max0','status400','overflow','badjson'):

                checks.update(error=not success,one_attempt=count==1)
                expected={'max0':'http 503','status400':'http 400','overflow':'response too large','badjson':'invalid response'}[name]
                checks['original_error']=expected in error
        result={'name':name,'argv':argv,'exit_code':None if p is None else p.returncode,'client':data,'server':rows,'checks':checks,'exception':failure,'reader_errors':reader_errors,'owned_driver_reaped':p is not None and p.poll() is not None,'wall_seconds':time.monotonic()-began}
        results.append(result);(directory/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(name,all(checks.values()),data.get('elapsed_ms'),count,flush=True)
    summary={'passed':all(all(r['checks'].values()) for r in results),'scenarios':len(results),'failed':[r['name'] for r in results if not all(r['checks'].values())],'results':results}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'passed':summary['passed'],'scenarios':summary['scenarios'],'failed':summary['failed']}));return 0 if summary['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
