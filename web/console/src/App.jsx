import { useCallback, useEffect, useRef, useState } from 'react';
import { getJson, postJson } from './api.js';
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
  const seenRef = useRef(new Set());

  // 事件去重：SSE 推送与 /api/state 快照（首屏补齐、刷新后不空白）可能有同一 id
  const pushEvent = useCallback((msg) => {
    if (!msg || typeof msg.id !== 'number' || !msg.ev) return;
    if (seenRef.current.has(msg.id)) return;
    seenRef.current.add(msg.id);
    setEvents((prev) => {
      const next = prev.concat(msg);
      return next.length > 800 ? next.slice(next.length - 800) : next;
    });
  }, []);

  // SSE：浏览器 EventSource 断线会自动重连，并按 Last-Event-ID 让服务端补齐（SPEC §6 M5）
  useEffect(() => {
    const es = new EventSource('/events');
    es.onopen = () => setSseStatus('live');
    es.onerror = () => setSseStatus('reconnecting');
    es.onmessage = (e) => {
      let msg = null;
      try {
        msg = JSON.parse(e.data);
      } catch {
        return;
      }
      if (!msg || !msg.ev) return;
      setSseStatus('live');
      if (msg.ev === 'console.ready') return; // 握手提示，不是业务事件
      pushEvent(msg);
    };
    return () => es.close();
  }, [pushEvent]);

  // 状态快照轮询：指标/管线参数/待审批（SSE 只负责增量）
  useEffect(() => {
    let alive = true;
    const load = async () => {
      const j = await getJson('/api/state');
      if (!alive) return;
      if (j && j.ok) {
        setState(j);
        if (Array.isArray(j.events)) j.events.forEach(pushEvent);
      } else {
        setState((prev) => prev || { ok: false, error: (j && j.error) || '无法连接 cue-serve' });
      }
    };
    load();
    const t = setInterval(load, 1500);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, [pushEvent]);

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
  const connectionOk = state ? state.ok !== false : false;

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
            后端 <b>{connectionOk ? '在线' : '离线'}</b>
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
        {state && state.ok === false ? (
          <div className="card">
            <h2>后端不可用</h2>
            <div className="muted">{state.error}</div>
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
