#!/usr/bin/env python3
"""Exercise the real offline CLI with synthetic records and an unused local API trap."""
import argparse,copy,json,os,socket,subprocess,tempfile,threading,time
from pathlib import Path

def main():
    p=argparse.ArgumentParser();p.add_argument('--eval',required=True);p.add_argument('--artifacts');a=p.parse_args()
    exe=str(Path(a.eval).resolve());out=Path(a.artifacts or tempfile.mkdtemp(prefix='cue-eval-manual-')).resolve();out.mkdir(parents=True,exist_ok=True)
    suite=out/'suite.json';input_file=out/'input.json'
    c={'id':'manual-fixture','seed':1,'prompt':'Synthetic manual grading interface fixture','manual_only':True,'fault':{'kind':'none'},'expect':{'root_cause':'fec_ghost_frame','evidence_candidates':['e2e_frag_ghost_discarded>0'],'sla':{'fps_min':25}}}
    suite.write_text(json.dumps({'suite':'synthetic-manual-interface','cases':[c]}))
    item={'id':c['id'],'source':'Synthetic CLI fixture, not observed browser/FEC evidence','diagnosis':{'root_cause':'fec_ghost_frame','evidence':['e2e_frag_ghost_discarded=2']},'observations':[{'tool':'query_metrics','text':'e2e_frag_ghost_discarded=2','data':{'e2e_frag_ghost_discarded':2}}],'metrics':{'fps':30,'samples':1}}
    good={'version':1,'cases':[item]};input_file.write_text(json.dumps(good))
    trap=socket.socket();trap.bind(('127.0.0.1',0));trap.listen();trap.settimeout(.1);hits=[];stop=threading.Event()
    def accept():
        while not stop.is_set():
            try:s,_=trap.accept();hits.append(True);s.close()
            except socket.timeout:pass
    worker=threading.Thread(target=accept);worker.start()
    env=os.environ.copy();env.update(CUE_BASE_URL='http://127.0.0.1:'+str(trap.getsockname()[1]),CUE_API_KEY='synthetic-unused',CUE_RECORD='0')
    checks=[];observed={}
    def run(name,args,expected,report=True):
        base=out/name;base.mkdir();argv=[exe,'--suite',str(suite),'--out',str(base/'runs'),'--no-baseline',*args]
        watch_stop=threading.Event()
        with (base/'console.log').open('wb') as log:
            proc=subprocess.Popen(argv,env=env,stdout=log,stderr=subprocess.STDOUT)
            def watch():
                while not watch_stop.is_set():
                    rows={}
                    for path in Path('/proc').iterdir():
                        if not path.name.isdigit():continue
                        try:st=(path/'stat').read_text();tail=st[st.rfind(')')+2:].split();rows[int(path.name)]=int(tail[1])
                        except (OSError,ValueError,IndexError):pass
                    parents={proc.pid};again=True
                    while again:
                        again=False
                        for pid,ppid in rows.items():
                            if pid not in parents and ppid in parents:parents.add(pid);again=True
                    for pid in parents:
                        try:
                            cmd=Path('/proc',str(pid),'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
                            if cmd:observed[pid]=cmd
                        except OSError:pass
                    watch_stop.wait(.005)
            watcher=threading.Thread(target=watch);watcher.start()
            try:code=proc.wait(timeout=15)
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait()
                watch_stop.set();watcher.join()
        found=list((base/'runs').glob('run-*/report.json'));r=json.loads(found[0].read_text()) if found else None
        ok=code==expected and ((r is not None) if report else not found)
        checks.append({'name':name,'pass':ok,'exit':code,'expected':expected,'pid':proc.pid,'argv':argv})
        return r,base
    common=['--mode','manual','--manual-input',str(input_file),'--cued','/missing-cued','--capture','/missing-capture','--probe','/missing-probe']
    try:
        r,base=run('positive',common,0);assert r
        checks.append({'name':'manual-report-provenance','pass':r['evaluation_source']=='manual_import' and r['cases'][0]['manual_source']==item['source'] and r['cases'][0]['scored'] and r['cases'][0]['score']['evidence']})
        rid=r['run_id'];resume=[exe,'--out',str(base/'runs'),'--resume',rid,'--no-baseline']
        with (base/'resume.log').open('wb') as log:q=subprocess.run(resume,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=15)
        resumed=json.loads((base/'runs'/rid/'report.json').read_text());checks.append({'name':'resume-restores-manual-input','pass':q.returncode==0 and resumed['agent_mode']=='manual' and resumed['manual_input']==str(input_file) and resumed['fingerprint']==r['fingerprint']})
        for key,value in [('llm_mode','replay'),('record',True),('record',False)]:
            config=out/('resume-config-'+key+'-'+str(value)+'.json');config.write_text(json.dumps({'eval':{key:value}}))
            with (base/('rejected-resume-'+key+'-'+str(value)+'.log')).open('wb') as log:
                q=subprocess.run(resume+['--config',str(config)],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=15)
            checks.append({'name':'resume-reject-explicit-'+key+'-'+str(value),'pass':q.returncode==2})
        changed=copy.deepcopy(good);changed['cases'][0]['diagnosis']['evidence']=['e2e_frag_ghost_discarded=0'];changed['cases'][0]['observations'][0]={'tool':'query_metrics','text':'e2e_frag_ghost_discarded=0','data':{'e2e_frag_ghost_discarded':0}};input_file.write_text(json.dumps(changed))
        with (base/'changed-resume.log').open('wb') as log:q=subprocess.run(resume,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=15)
        changed_report=json.loads((base/'runs'/rid/'report.json').read_text());checks.append({'name':'changed-input-invalidates-cache','pass':q.returncode==1 and changed_report['fingerprint']!=r['fingerprint'] and not changed_report['cases'][0]['score']['evidence']})
        input_file.unlink()
        with (base/'missing-resume.log').open('wb') as log:q=subprocess.run(resume,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=15)
        checks.append({'name':'missing-input-refuses-resume-without-overwriting','pass':q.returncode==2 and json.loads((base/'runs'/rid/'report.json').read_text())==changed_report})
        input_file.write_text(json.dumps(good))
        low=copy.deepcopy(good);low['cases'][0]['metrics']['fps']=1;input_file.write_text(json.dumps(low));run('low-sla',common,1)
        input_file.write_text(json.dumps({'version':1,'cases':[]}));missing,_=run('missing-record',common,1);checks.append({'name':'missing-record-runerror','pass':missing and missing['cases'][0]['error_kind']=='manual_missing_result'})
        input_file.write_text(json.dumps(good))
        for name,extra in [('producer',['--producer','lavfi']),('endpoint',['--endpoint','http://127.0.0.1:1']),('record-tools',['--record-tools',str(out/'unstarted')]),('replay-tools',['--replay-tools',str(out/'unstarted')]),('live',['--live']),('replay',['--replay']),('record',['--record']),('repeat',['--repeat','2'])]:run('reject-'+name,common+extra,2,False)
        run('reject-other-mode',['--mode','scripted','--manual-input',str(input_file)],2,False)
        run('reject-no-input',['--mode','manual'],2,False)
        cfg=out/'config.json';cfg.write_text(json.dumps({'eval':{'agent_mode':'manual','manual_input':str(out/'overridden-missing')}}));run('config-cli-override',['--config',str(cfg),'--manual-input',str(input_file)],0)
        cfg.write_text(json.dumps({'eval':{'agent_mode':'manual','manual_input':str(input_file)}}));run('config-manual',['--config',str(cfg)],0)
        run('real-default-skips-manual',['--mode','real','--cued','/missing-cued'],1)
        for name,mutate in [('unknown-top',lambda d:d.update(score=True)),('version',lambda d:d.update(version=2)),('duplicate',lambda d:d['cases'].append(item)),('unknown-id',lambda d:d['cases'][0].update(id='unknown')),('bad-type',lambda d:d['cases'][0]['metrics'].update(samples='1'))]:
            data=copy.deepcopy(good);mutate(data);input_file.write_text(json.dumps(data));run('reject-schema-'+name,common,2,False)
    finally:
        stop.set();worker.join();trap.close()
    media=[v for v in observed.values() if v.split() and Path(v.split()[0]).name in {'cued','cue-probe','cue-capture','ffmpeg'}]
    checks.append({'name':'no-media-backend-descendants','pass':not media,'media':media})
    checks.append({'name':'no-llm-http-request','pass':not hits})
    result={'result':'pass' if all(c['pass'] for c in checks) else 'fail','checks':checks,'observed_processes':observed,'source':'synthetic file/CLI interface validation only'}
    (out/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps({'result':result['result'],'checks':len(checks),'failed':[c['name'] for c in checks if not c['pass']],'artifacts':str(out)},ensure_ascii=False));return 0 if result['result']=='pass' else 1
if __name__=='__main__':raise SystemExit(main())
