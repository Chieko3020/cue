// 会话页：SSE 事件流时间线（实时追加）+ 按类型过滤。
// 事件类型不写死：cued 新增事件（fault.* / codec.*）会自动出现在过滤条里。
import { useMemo, useState } from 'react';
import { Card } from '../components/ui.jsx';
import { clock, eventSummary, num } from '../api.js';

export default function Session({ events, sseStatus }) {
  const [only, setOnly] = useState(() => new Set());

  const types = useMemo(() => {
    const set = new Set(events.map((e) => e.ev));
    return Array.from(set).sort();
  }, [events]);

  const counts = useMemo(() => {
    const m = new Map();
    for (const e of events) m.set(e.ev, (m.get(e.ev) || 0) + 1);
    return m;
  }, [events]);

  const shown = only.size === 0 ? events : events.filter((e) => only.has(e.ev));
  // 新的在上：不用滚动就能看到最新事件
  const ordered = shown.slice(-400).reverse();

  function toggle(t) {
    const next = new Set(only);
    if (next.has(t)) next.delete(t);
    else next.add(t);
    setOnly(next);
  }

  return (
    <Card
      title="事件流"
      sub={`共 ${events.length} 条 · SSE ${sseStatus} · 显示 ${shown.length} 条（最新在上）`}
      right={
        only.size > 0 ? (
          <button className="chip" data-testid="filter-all" onClick={() => setOnly(new Set())}>
            全部
          </button>
        ) : (
          <span className="badge muted">全部</span>
        )
      }
    >
      <div className="row" style={{ marginBottom: 10 }}>
        {types.map((t) => (
          <button
            key={t}
            className={'chip' + (only.has(t) ? ' active' : '')}
            data-testid={`filter-${t}`}
            onClick={() => toggle(t)}
          >
            {t} <span className="muted">{counts.get(t)}</span>
          </button>
        ))}
        {types.length === 0 ? <span className="muted">等待事件…（起 cued 或点一次动作就会有条目）</span> : null}
      </div>

      {ordered.length === 0 ? (
        <div className="empty" data-testid="session-empty">
          当前过滤条件下没有事件
        </div>
      ) : (
        <ol className="events" data-testid="event-list">
          {ordered.map((e) => (
            <li key={`${e.id}-${e.ev}`} data-testid="event-row" data-ev={e.ev}>
              <span className="t">{clock(e.t)}</span>
              <span className="ev">{e.ev}</span>
              <span className="d">{eventSummary(e) || <span className="muted">（无摘要）</span>}</span>
              <span className="raw">{JSON.stringify(e.data)}</span>
            </li>
          ))}
        </ol>
      )}
      <p className="small muted" style={{ marginBottom: 0 }}>
        断线重连时浏览器自动带 Last-Event-ID，服务端从环形缓冲补齐后发 console.ready
        （其中 replayed_from 是重连点）。事件最多保留 {num(800, 0)} 条在前端。
      </p>
    </Card>
  );
}
