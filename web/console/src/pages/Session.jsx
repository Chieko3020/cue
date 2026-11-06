// 会话页：两个视图，同一份数据的不同切法。
//   · 轨迹（默认）：读 /api/session 的 jsonl 记录，按 collect→diagnose→repair→verify 分段，
//     段内按 seq（文件行号）还原顺序，展示「agent 怎么想的」：思考 → 工具调用 → 观测 → 证据。
//   · 事件流：原来的 SSE 实时流（cued/管线事件），适合看"现在发生了什么"。
//
// 为什么轨迹读 jsonl 而不是从 SSE 重建：SSE 是推送通道，环形缓冲有上限、断线只按
// Last-Event-ID 补最近一段，控制台还会丢慢消费者事件——用它拼"历史"必然缺段。
// 会话文件是 agent 自己 append-only 写的完整轨迹（工具参数、耗时、观测原文都在），
// 是唯一能当审计证据的来源。两者不互相替代：实时看 SSE，复盘看 jsonl。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Card } from '../components/ui.jsx';
import { clock, eventSummary, getJson, num } from '../api.js';

const STAGE_ORDER = ['collect', 'diagnose', 'repair', 'verify'];
const STAGE_LABEL = { collect: '收集', diagnose: '诊断', repair: '修复', verify: '验证' };
const GROUPS = ['messages', 'tools', 'observations', 'stages', 'budgets', 'ends', 'other'];

// 分组是为渲染方便（一类一个数组），顺序是事实：合并回按 seq（= 文件行号）排序的一条流。
function mergeRecords(detail) {
  const out = [];
  for (const g of GROUPS) {
    for (const r of detail?.[g] || []) out.push({ ...r, _group: g });
  }
  out.sort((a, b) => (a.seq ?? 0) - (b.seq ?? 0));
  return out;
}

// 工具记录的 summary 只有 512 字节（写端上限），完整结果在紧随其后的 message(role=tool)。
// 按 seq 顺序一一配对：这是"可展开看全文"的数据来源，不是把摘要当全文糊弄。
function pairToolResults(records) {
  const toolMsgs = records.filter((r) => r.t === 'message' && r.role === 'tool');
  const used = new Set();
  const map = new Map();
  for (const r of records) {
    if (r.t !== 'tool') continue;
    const hit = toolMsgs.find(
      (m) => !used.has(m.seq) && m.seq > r.seq && (!m.name || m.name === r.name),
    );
    if (hit) {
      used.add(hit.seq);
      map.set(r.seq, hit.content || '');
    }
  }
  return map;
}

// 按阶段切段：迁移记录（stage, allowed=true）属于它**离开**的那个阶段——
// diagnose→repair 这条记录带着 diagnosis.evidence，证据必须留在 diagnose 段里，
// 否则"诊断段能看到证据"这个页面存在的理由就没了。
function buildSegments(records) {
  const segs = [];
  let cur = null;
  const newSeg = (stage, start) => ({
    stage,
    items: [],
    start: start || { turns: 0, wall_us: 0 },
    end: null,
    diagnosis: null,
    guards: 0,
  });
  cur = newSeg('collect', { turns: 0, wall_us: 0 });
  segs.push(cur);
  let lastProgress = { turns: 0, wall_us: 0 };

  for (const r of records) {
    if (r.t === 'budget') {
      lastProgress = { turns: r.turns ?? lastProgress.turns, wall_us: r.wall_us ?? lastProgress.wall_us };
      continue;
    }
    if (r.t === 'end') {
      cur.end = { turns: r.turns, wall_us: r.wall_us, kind: r.kind, reason: r.reason, final_stage: r.final_stage };
      continue;
    }
    if (r.t === 'stage') {
      if (r.allowed !== true) {
        // 守卫拒绝：agent 想越权/跳阶段被拦下的唯一留痕，单独计一条
        cur.items.push({ kind: 'guard', rec: r });
        cur.guards += 1;
        continue;
      }
      if (r.diagnosis && typeof r.diagnosis === 'object') cur.diagnosis = r.diagnosis;
      cur.end = { turns: r.turns, wall_us: r.wall_us, to: r.to };
      if (STAGE_ORDER.includes(r.to)) {
        cur = newSeg(r.to, { turns: r.turns ?? 0, wall_us: r.wall_us ?? 0 });
        segs.push(cur);
      }
      continue;
    }
    if (r.t === 'message') {
      // 轨迹里只留 assistant 的正文（思考/结论）。tool/user/system 消息是对话回显：
      // 工具结果已由 tool/observation 记录承载，再列一遍只会让轨迹变成两倍长。
      if (r.role === 'assistant' && (r.content || '').trim() !== '') {
        cur.items.push({ kind: 'thinking', rec: r });
      }
      continue;
    }
    if (r.t === 'tool') {
      cur.items.push({ kind: 'tool', rec: r });
      continue;
    }
    if (r.t === 'observation') {
      cur.items.push({ kind: 'observation', rec: r });
      continue;
    }
    if (r.t === 'other') cur.items.push({ kind: 'other', rec: r });
  }

  // 段的耗时/turns 用累计量的差值（stage 记录带的是"迁移那一刻"的累计值）：
  // 没有预算增量数据，也就没有更细的口径可编。
  for (const s of segs) {
    const endP = s.end || lastProgress;
    s.turns = Math.max(0, (endP.turns ?? s.start.turns) - s.start.turns);
    s.wall_us = Math.max(0, (endP.wall_us ?? s.start.wall_us) - s.start.wall_us);
  }
  return segs;
}

// 耗时的显示单位跟着量级走：demo 用假时钟，整场只有几百微秒，写成 "0.000 s"
// 会被读成"没测到"；真实运行的段耗时是秒级，用 ms 会变成五位数。
function dur(us) {
  if (!us) return '0 ms';
  if (us < 1000 * 1000) return `${(us / 1000).toFixed(1)} ms`;
  return `${(us / 1e6).toFixed(3)} s`;
}

// 证据核对：判断一条证据能否在**本会话的 observation** 里落实。三种写法都认：
//   · 原文子串（"p99_ms=181.4" 直接出现在观测文本里）
//   · key=value（观测 data 是 JSON，"nack_retransmit=42" 对应 data.nack_retransmit === 42，
//     JSON 里是冒号不是等号，只做子串核对会把真证据误判成"未找到"——demo 就是这种）
//   · 裸 token（"fps"：出现在观测文本里，或作为 data 的键名）
// 这是本地一致性核对，**不是评测判分**——cue-eval 的口径是 case.expect.evidence_candidates。
// 界面上必须写清楚，否则会被读成"这次评测已经通过"。
function evidenceMarks(evidence, observations) {
  const obs = observations || [];
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const flat = (data, k) => {
    if (!data || typeof data !== 'object') return false;
    if (own(data, k)) return true;
    for (const v of Object.values(data)) {
      if (v && typeof v === 'object' && !Array.isArray(v) && own(v, k)) return true;
    }
    return false;
  };
  const lookup = (data, k) => {
    if (!data || typeof data !== 'object') return undefined;
    if (own(data, k)) return data[k];
    for (const v of Object.values(data)) {
      if (v && typeof v === 'object' && !Array.isArray(v) && own(v, k)) return v[k];
    }
    return undefined;
  };
  return (evidence || []).map((raw) => {
    const text = String(raw);
    const eq = text.indexOf('=');
    const key = eq > 0 ? text.slice(0, eq).trim() : null;
    const want = eq > 0 ? text.slice(eq + 1).trim() : null;
    let found = false;
    for (const o of obs) {
      const body = String(o.text || '');
      if (body.includes(text)) {
        found = true;
        break;
      }
      if (key && want) {
        const v = lookup(o.data, key);
        if (v !== undefined && String(v) === want) {
          found = true;
          break;
        }
      } else if (flat(o.data, text)) {
        found = true;
        break;
      }
    }
    return { text, found };
  });
}

function EvidencePanel({ diagnosis, observations }) {
  if (!diagnosis) {
    return (
      <div className="empty" data-testid="trace-evidence-missing">
        diagnose 段没有记录结构化诊断（diagnosis 为空）——agent 没有给出 root_cause / evidence。
      </div>
    );
  }
  const marks = evidenceMarks(diagnosis.evidence, observations);
  return (
    <div className="evidence" data-testid="trace-evidence">
      <div className="row">
        <span className="badge ok">证据</span>
        <span>
          root_cause <b data-testid="trace-root-cause">{diagnosis.root_cause || '—'}</b>
        </span>
        <span className="muted">confidence {num(diagnosis.confidence, 2)}</span>
      </div>
      {marks.length === 0 ? (
        <div className="small bad" data-testid="trace-evidence-empty">
          诊断里没有 evidence 数组（空证据 = 不可判分，不是"证据成立"）。
        </div>
      ) : (
        <ul className="evidence-list" data-testid="trace-evidence-list">
          {marks.map((m) => (
            <li key={m.text} data-testid="trace-evidence-item" data-found={m.found}>
              <span className={'badge ' + (m.found ? 'ok' : 'bad')}>{m.found ? '在观测中' : '未找到'}</span>
              <code>{m.text}</code>
            </li>
          ))}
        </ul>
      )}
      <p className="small muted" style={{ marginBottom: 0 }}>
        「在观测中/未找到」= 该证据能否在本次会话的 observation 里落实（原文子串，或 key=value
        与观测 data 对得上；裸 token 按"作为键名出现"算）。这是本地一致性核对，不是评测判分；
        cue-eval 的口径是 case.expect.evidence_candidates。
      </p>
    </div>
  );
}

function ToolItem({ rec, full }) {
  return (
    <li className="trace-item" data-testid="trace-tool" data-tool={rec.name}>
      <div className="row">
        <span className="badge mut">tool</span>
        <b>{rec.name}</b>
        <span className={'badge ' + (rec.ok ? 'ok' : 'bad')}>{rec.ok ? 'ok' : 'fail'}</span>
        <span className="badge muted">{rec.risk || 'unknown'}</span>
        <span className="small muted">
          {num((rec.duration_us || 0) / 1000, 3)} ms{rec.turn ? ` · turn ${rec.turn}` : ''}
        </span>
        {rec.approval_denied ? <span className="badge bad">审批拒绝</span> : null}
      </div>
      <div className="small muted">参数 {JSON.stringify(rec.args ?? {})}</div>
      <div className="small">结果摘要：{rec.summary || '（空）'}</div>
      {full ? (
        <details>
          <summary className="small">完整结果（{full.length} 字节）</summary>
          <pre className="trace-pre">{full}</pre>
        </details>
      ) : null}
    </li>
  );
}

function ObsItem({ rec }) {
  const text = rec.text || '';
  const brief = text.length > 240 ? text.slice(0, 240) + '…' : text;
  return (
    <li className="trace-item" data-testid="trace-observation" data-tool={rec.tool}>
      <div className="row">
        <span className="badge warn">observation</span>
        <b>{rec.tool || '?'}</b>
        {rec.truncated ? (
          <span className="badge bad" title={`原始 ${rec.original_bytes} 字节，写盘时被截断`}>
            已截断
          </span>
        ) : null}
        <span className="small muted">
          {rec.original_bytes || text.length} 字节 · turn {rec.turn ?? '—'}
        </span>
      </div>
      <div className="small wrap">{brief}</div>
      <details>
        <summary className="small">观测原文 / data</summary>
        <pre className="trace-pre">{text}</pre>
        <pre className="trace-pre">{JSON.stringify(rec.data ?? {}, null, 2)}</pre>
      </details>
    </li>
  );
}

function StageSection({ seg, toolResults, allObservations, index }) {
  const isDiagnose = seg.stage === 'diagnose';
  return (
    <section
      className={'trace-stage' + (isDiagnose ? ' diagnose' : '')}
      data-testid="trace-stage"
      data-stage={seg.stage}
    >
      <h3>
        <span className="badge muted">{index + 1}</span> {STAGE_LABEL[seg.stage] || seg.stage}
        <code className="small"> {seg.stage}</code>
        <span className="small muted">
          段耗时 {dur(seg.wall_us)} · turns {seg.turns} · {seg.items.length} 条
          {seg.guards > 0 ? ` · 守卫拒绝 ${seg.guards}` : ''}
        </span>
        {seg.end?.to ? <span className="badge muted">→ {seg.end.to}</span> : null}
      </h3>

      {isDiagnose ? (
        <EvidencePanel diagnosis={seg.diagnosis} observations={allObservations} />
      ) : null}

      {seg.items.length === 0 ? (
        <div className="empty">这一段没有记录（可能是刚进入就被中断）</div>
      ) : (
        <ol className="trace-items">
          {seg.items.map((it) => {
            if (it.kind === 'thinking') {
              return (
                <li className="trace-item" data-testid="trace-think" key={`m-${it.rec.seq}`}>
                  <div className="row">
                    <span className="badge muted">assistant</span>
                    <span className="small muted">#{it.rec.seq}</span>
                  </div>
                  <div className="wrap small">{it.rec.content}</div>
                </li>
              );
            }
            if (it.kind === 'tool') {
              return (
                <ToolItem key={`t-${it.rec.seq}`} rec={it.rec} full={toolResults.get(it.rec.seq)} />
              );
            }
            if (it.kind === 'observation') {
              return <ObsItem key={`o-${it.rec.seq}`} rec={it.rec} />;
            }
            if (it.kind === 'guard') {
              return (
                <li className="trace-item" data-testid="trace-guard" key={`g-${it.rec.seq}`}>
                  <div className="row">
                    <span className="badge bad">守卫拒绝</span>
                    <span className="small">
                      {it.rec.from} → {it.rec.to}：{it.rec.reason || '（无原因）'}
                    </span>
                  </div>
                </li>
              );
            }
            return (
              <li className="trace-item" data-testid="trace-other" key={`x-${it.rec.seq}`}>
                <div className="row">
                  <span className="badge muted">未知记录</span>
                  <code className="small">{it.rec.t}</code>
                </div>
                <pre className="trace-pre">{JSON.stringify(it.rec, null, 2)}</pre>
              </li>
            );
          })}
        </ol>
      )}
    </section>
  );
}

function TraceView() {
  const [sessions, setSessions] = useState(null); // {ok, sessions, error}
  const [selected, setSelected] = useState('');
  const [detail, setDetail] = useState(null);
  const [err, setErr] = useState('');
  const [follow, setFollow] = useState(true);
  const busy = useRef(false);

  const loadList = useCallback(async () => {
    const d = await getJson('/api/sessions');
    setSessions(d);
    return d;
  }, []);

  const loadDetail = useCallback(async (id) => {
    if (!id) {
      setDetail(null);
      return;
    }
    const d = await getJson('/api/session?id=' + encodeURIComponent(id));
    if (d && d.ok) {
      setDetail(d);
      setErr('');
    } else {
      setDetail(null);
      setErr((d && d.error) || '读取会话失败');
    }
  }, []);

  // 首屏：列表 + 默认选最新一条
  useEffect(() => {
    let alive = true;
    loadList().then((d) => {
      if (!alive) return;
      const first = d?.sessions?.[0]?.session_id;
      if (first) setSelected((cur) => cur || first);
    });
    return () => {
      alive = false;
    };
  }, [loadList]);

  useEffect(() => {
    if (selected) loadDetail(selected);
  }, [selected, loadDetail]);

  // 跟随：会话文件是追加写的，跑着的 session 需要能看到新记录。节流 4s，
  // 且同一时刻只发一轮请求（in-flight 标志）——避免慢响应叠加成请求风暴。
  useEffect(() => {
    if (!follow) return undefined;
    const t = setInterval(async () => {
      if (busy.current) return;
      busy.current = true;
      try {
        await loadList();
        if (selected) await loadDetail(selected);
      } finally {
        busy.current = false;
      }
    }, 4000);
    return () => clearInterval(t);
  }, [follow, selected, loadList, loadDetail]);

  const records = useMemo(() => mergeRecords(detail), [detail]);
  const toolResults = useMemo(() => pairToolResults(records), [records]);
  const segments = useMemo(() => buildSegments(records), [records]);
  const observations = useMemo(() => records.filter((r) => r.t === 'observation'), [records]);

  const list = sessions?.sessions || [];
  const pick = list.find((s) => s.session_id === selected) || null;
  const diagnoseSeg = segments.find((s) => s.stage === 'diagnose');

  return (
    <Card
      title="agent 轨迹"
      sub={
        detail
          ? `${detail.session_id}（${detail.run_id}） · ${detail.mode || '?'}/${detail.model || '?'} · ${detail.records} 条记录 · ${num(detail.bytes, 0)} 字节`
          : '选一个会话文件（runs/*/sessions/*.jsonl）'
      }
      right={
        <>
          <label className="small muted" style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <input
              type="checkbox"
              data-testid="trace-follow"
              checked={follow}
              onChange={(e) => setFollow(e.target.checked)}
            />
            跟随（4s）
          </label>
          <button
            data-testid="trace-refresh"
            onClick={() => {
              loadList();
              if (selected) loadDetail(selected);
            }}
          >
            刷新
          </button>
        </>
      }
      testid="trace-card"
    >
      <div className="row" style={{ marginBottom: 10 }}>
        <label className="f">
          会话
          <select
            data-testid="session-select"
            value={selected}
            onChange={(e) => setSelected(e.target.value)}
          >
            {list.length === 0 ? <option value="">（没有会话文件）</option> : null}
            {list.map((s) => (
              <option key={`${s.run_id}/${s.session_id}`} value={s.session_id}>
                {s.run_id}/{s.session_id} · {s.final_stage || '?'} · {s.turns} turns
              </option>
            ))}
          </select>
        </label>
        {pick ? (
          <span className="badge muted">更新于 {clock(pick.mtime_us)}</span>
        ) : null}
        {pick?.ended ? (
          <span className="badge ok">已结束（{pick.end_kind || 'end'}）</span>
        ) : pick ? (
          <span className="badge warn">未结束/可恢复</span>
        ) : null}
        {segments.length > 0 ? (
          <span className="badge muted">
            {segments.length} 段 · diagnose {diagnoseSeg ? '有' : '无'}
          </span>
        ) : null}
      </div>

      {sessions?.error ? <div className="empty">{sessions.error}</div> : null}
      {err ? (
        <div className="empty bad" data-testid="trace-error">
          {err}
        </div>
      ) : null}
      {!detail && !err && list.length === 0 && sessions ? (
        <div className="empty" data-testid="trace-empty">
          还没有会话文件。跑一次带 --session 的 agent：<code>./build/cue demo --session runs/e2e/sessions/demo.jsonl</code>
        </div>
      ) : null}

      {detail && detail.warnings?.length ? (
        <div className="warnbox" data-testid="trace-warning">
          {detail.warnings.map((w) => (
            <div className="small" key={w}>
              · {w}
            </div>
          ))}
        </div>
      ) : null}
      {detail?.truncated ? (
        <div className="empty">文件超过解析上限，只显示了前一部分记录</div>
      ) : null}

      {detail && segments.length === 0 ? (
        <div className="empty">这个会话没有可显示的轨迹记录</div>
      ) : null}

      {segments.map((seg, i) => (
        <StageSection
          key={`${seg.stage}-${i}`}
          seg={seg}
          index={i}
          toolResults={toolResults}
          allObservations={observations}
        />
      ))}

      {detail ? (
        <p className="small muted" style={{ marginBottom: 0 }}>
          轨迹来源：<code>{detail.path}</code>（agent append-only 写入；不是从 SSE 重建）。
          段的耗时/turns 是 stage/budget 记录里累计值的差；被 kill 的会话末行可能是残行，会以 warning 列出。
        </p>
      ) : null}
    </Card>
  );
}

function EventStream({ events, sseStatus }) {
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
            <li key={`${e.id}-${e.ev}`} data-testid="event-row" data-ev={e.ev} data-generation={e.generation} data-id={e.id}>
              <span className="t">{clock(e.t)}</span>
              <span className="ev">{e.ev}</span>
              <span className="d">{eventSummary(e) || <span className="muted">（无摘要）</span>}</span>
              <span className="raw">{JSON.stringify(e.data)}</span>
            </li>
          ))}
        </ol>
      )}
      <p className="small muted" style={{ marginBottom: 0 }}>
        断线重连时浏览器自动带 Last-Event-ID，服务端先发 console.ready 确认服务代际，再从环形缓冲补齐
        （其中 replayed_from 是重连点）。事件最多保留 {num(800, 0)} 条在前端。
      </p>
    </Card>
  );
}

export default function Session({ events, sseStatus }) {
  const [view, setView] = useState('trace');
  return (
    <>
      <div className="row" data-testid="session-view-switch">
        <button
          data-testid="view-trace"
          className={view === 'trace' ? 'active' : ''}
          onClick={() => setView('trace')}
        >
          轨迹（按阶段）
        </button>
        <button
          data-testid="view-events"
          className={view === 'events' ? 'active' : ''}
          onClick={() => setView('events')}
        >
          事件流（实时）
        </button>
        <span className="small muted">
          轨迹 = agent 会话文件（完整、可审计）；事件流 = SSE 推送（实时、有上限）。
        </span>
      </div>
      {view === 'trace' ? <TraceView /> : <EventStream events={events} sseStatus={sseStatus} />}
    </>
  );
}
