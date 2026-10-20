// 评测页：读 /api/runs（服务端解析 runs/<id>/report.md，不额外产出 JSON）。
// 展示分层成功率（根因/证据/SLA/pass^k/Run Error/MTTR）、成本、逐用例与失败用例。
import { useCallback, useEffect, useState } from 'react';
import { Card } from '../components/ui.jsx';
import { getJson, num } from '../api.js';

function RateChip({ label, value }) {
  const v = value ?? '—';
  const ok = /^\d+(\.\d+)?%$/.test(v) ? parseFloat(v) >= 100 : null;
  const bad = ok === false;
  return (
    <span className={`badge ${ok === true ? 'ok' : bad ? 'bad' : 'muted'}`} title={label}>
      {label} {v}
    </span>
  );
}

function RunCard({ r }) {
  const rm = r.rate_map || {};
  const headers = r.case_rows && r.case_rows.length ? Object.keys(r.case_rows[0]).filter((k) => k !== 'ok') : [];
  return (
    <Card
      title={r.run_id || r.dir || '(未命名 run)'}
      sub={`${r.suite || '—'} · ${r.agent || '—'} · repeat=${r.repeat ?? '—'} · ¥${num(r.cost_yuan, 4)}`}
      right={
        <span className={`badge ${r.ok ? 'ok' : 'bad'}`} data-testid="run-status">
          {r.ok ? '全部通过' : `${(r.failed || []).length} 条未通过`}
        </span>
      }
      testid="run-card"
    >
      <div className="row" style={{ marginBottom: 10 }}>
        <RateChip label="根因" value={rm.root} />
        <RateChip label="证据" value={rm.evidence} />
        <RateChip label="SLA" value={rm.sla} />
        <RateChip label="pass^k" value={rm.pass_k} />
        <RateChip label="Run Error" value={rm.run_error} />
        <span className="badge muted">MTTR {rm.mttr ?? '—'}</span>
        <span className="badge muted">
          成本 ¥{num(r.cost_yuan, 4)}（估算）
        </span>
        <span className="badge muted">
          用例 {r.cases?.executed ?? '—'}/{r.cases?.planned ?? '—'}（计分 {r.cases?.scored ?? '—'}，失败{' '}
          {r.cases?.error ?? '—'}）
        </span>
      </div>

      {(r.failed || []).length > 0 ? (
        <div className="row" style={{ marginBottom: 8 }}>
          <span className="small muted">未通过用例：</span>
          {r.failed.map((f) => (
            <span className="badge bad" key={f} data-testid="failed-case">
              {f}
            </span>
          ))}
        </div>
      ) : null}

      {r.case_rows && r.case_rows.length ? (
        <table>
          <thead>
            <tr>
              {headers.map((h) => (
                <th key={h}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {r.case_rows.map((row, i) => (
              <tr key={`${r.run_id}-${i}`} data-testid="case-row">
                {headers.map((h) => (
                  <td key={h} className={row[h] === '✗' ? 'bad' : ''}>
                    {row[h] === '' ? '—' : String(row[h])}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      ) : (
        <div className="empty">这份报告没有逐用例表格</div>
      )}
      <p className="small muted" style={{ marginBottom: 0 }}>
        目录 {r.dir} · 报告由 cue-eval 生成（scripted 模式 = 引擎自检；real 模式每次约 ¥0.13）
      </p>
    </Card>
  );
}

export default function Runs() {
  const [data, setData] = useState(null);
  const [loading, setLoading] = useState(false);

  const load = useCallback(async () => {
    setLoading(true);
    const d = await getJson('/api/runs');
    setData(d);
    setLoading(false);
  }, []);

  useEffect(() => {
    load();
  }, [load]);

  const runs = data?.runs || [];

  return (
    <>
      <Card
        title="评测产物"
        sub={`${data?.dir ?? 'runs'} · ${runs.length} 份报告${loading ? ' · 加载中' : ''}`}
        right={
          <button data-testid="runs-refresh" onClick={load} disabled={loading}>
            刷新
          </button>
        }
      >
        {data?.error ? <div className="empty">{data.error}</div> : null}
        {!data ? <div className="empty">加载中…</div> : null}
        {data && !data.error && runs.length === 0 ? (
          <div className="empty" data-testid="runs-empty">
            没有报告。跑一次评测生成：<code>./build/cue-eval --suite suites/m4-diagnosis.json --mode scripted</code>
          </div>
        ) : null}
      </Card>
      {runs.map((r, i) => (
        <RunCard r={r} key={`${r.run_id}-${i}`} />
      ))}
    </>
  );
}
