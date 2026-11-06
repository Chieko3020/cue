import { useCallback, useEffect, useRef, useState } from 'react';
import { getJson, postJson } from './api.js';
import { createEventStore } from './event-store.js';
import Device from './pages/Device.jsx';
import Pipeline from './pages/Pipeline.jsx';
import Session from './pages/Session.jsx';
import Approval from './pages/Approval.jsx';
import Runs from './pages/Runs.jsx';

const PAGES = [
  { id: 'device', label: '设备' },
  { id: 'pipeline', label: '管线' },
  { id: 'session', label: '会话' },
  { id: 'approval', label: '审批' },
  { id: 'runs', label: '评测' },
];

export default function App() {
  const [page, setPage] = useState('device');
  const [state, setState] = useState(null);
  const [events, setEvents] = useState([]);
  const [sseStatus, setSseStatus] = useState('connecting');
  const [lastAction, setLastAction] = useState(null);
  const [busy, setBusy] = useState(0);
  const [tick, setTick] = useState(0);
  const storeRef = useRef(createEventStore());
  const snapshotEpoch = useRef(0);
  const [snapshotStatus, setSnapshotStatus] = useState('connecting');
  const [snapshotError, setSnapshotError] = useState('');
  const selectGeneration = useCallback((generation) => {
    const store = storeRef.current;
    const previous = store.generation;
    if (!store.select(generation)) return false;
    if (previous !== generation) {
      snapshotEpoch.current += 1;
      setEvents([]);
      setSnapshotStatus('stale');
    }
    return true;
  }, []);

  const pushEvent = useCallback((msg) => {
    if (storeRef.current.push(msg)) setEvents(storeRef.current.events);
  }, []);

  useEffect(() => {
    const es = new EventSource('/events');
    es.onopen = () => setSseStatus('live');
    es.onerror = () => {
      snapshotEpoch.current += 1;
      setSseStatus('reconnecting');
      setSnapshotStatus('stale');
    };
    es.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (!msg?.ev || !selectGeneration(msg.generation)) return;
      setSseStatus('live');
      pushEvent(msg);
    };
    return () => es.close();
  }, [pushEvent, selectGeneration]);

  // Schedule after completion, rather than setInterval: at most one snapshot
  // request is live. SSE errors and generation switches invalidate older responses.
  useEffect(() => {
    let alive = true;
    let timer;
    const controller = new AbortController();
    const load = async () => {
      const epoch = snapshotEpoch.current;
      const j = await getJson('/api/state', { signal: controller.signal });
      if (!alive) return;
      if (epoch === snapshotEpoch.current) {
        if (j?.ok && selectGeneration(j.server?.generation)) {
          setState(j);
          setSnapshotStatus('online');
          setSnapshotError('');
          if (Array.isArray(j.events)) j.events.forEach(pushEvent);
        } else {
          setSnapshotStatus('offline');
          setSnapshotError(j?.error || '无法连接 cue-serve');
        }
      }
      timer = setTimeout(load, 1500);
    };
    load();
    return () => {
      alive = false;
      controller.abort();
      clearTimeout(timer);
    };
  }, [pushEvent, selectGeneration]);

  // 审批倒计时每秒重绘
  useEffect(() => {
    const t = setInterval(() => setTick((n) => n + 1), 1000);
    return () => clearInterval(t);
  }, []);

  const runAction = useCallback(async (body) => {
    setBusy((n) => n + 1);
    const r = await postJson('/api/action', body);
    setBusy((n) => Math.max(0, n - 1));
    setLastAction({ ...r, req: body, at: Date.now() });
    return r;
  }, []);

  const pipeline = state?.pipeline || {};
  const metrics = state?.metrics || {};
  const pending = state?.approvals?.pending || [];
  const connectionOk = snapshotStatus === 'online';

  return (
    <>
      <header className="app">
        <h1>Cue 控制台</h1>
        <div className="meta">
          <span>
            SSE <b data-testid="sse-status">{sseStatus}</b>
          </span>
          <span>
            事件 <b data-testid="event-count">{events.length}</b>
          </span>
          <span>
            管线{' '}
            <b data-testid="header-pipeline">
              {pipeline.running ? `运行中 pid=${metrics.pid ?? '—'}` : '已停止'}
            </b>
          </span>
          <span>
            审批模式 <b>{state?.approvals?.mode ?? '—'}</b>
          </span>
          <span>
            后端 <b data-testid="backend-status">{connectionOk ? '在线' : snapshotStatus === 'connecting' ? '连接中' : '离线 / 快照过期'}</b>
          </span>
          {busy > 0 ? <span className="badge warn">动作进行中 {busy}</span> : null}
        </div>
      </header>

      <nav className="tabs">
        {PAGES.map((p) => (
          <button
            key={p.id}
            data-testid={`tab-${p.id}`}
            className={page === p.id ? 'active' : ''}
            onClick={() => setPage(p.id)}
          >
            {p.label}
            {p.id === 'approval' && pending.length > 0 ? ` (${pending.length})` : ''}
          </button>
        ))}
        <span className="pill">
          {state ? `${state.server?.bind ?? '?'}:${state.server?.port ?? '?'} · 静态 ${state.server?.web_dir ?? '?'}` : '连接中…'}
        </span>
      </nav>

      <main>
        {!connectionOk && snapshotStatus !== 'connecting' ? (
          <div className="card" data-testid="snapshot-stale">
            <h2>后端状态不可用</h2>
            <div className="muted">{snapshotError || '等待当前服务的状态快照'}{state ? '；下方保留的是旧快照，指标与管线状态可能已过期。' : ''}</div>
          </div>
        ) : null}
        <div data-testid={`page-${page}`} className="page">
          {page === 'device' ? (
            <Device state={state} runAction={runAction} busy={busy} lastAction={lastAction} />
          ) : null}
          {page === 'pipeline' ? (
            <Pipeline state={state} runAction={runAction} busy={busy} lastAction={lastAction} />
          ) : null}
          {page === 'session' ? <Session events={events} sseStatus={sseStatus} /> : null}
          {page === 'approval' ? <Approval state={state} tick={tick} /> : null}
          {page === 'runs' ? <Runs /> : null}
        </div>
      </main>
    </>
  );
}
