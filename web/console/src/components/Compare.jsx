// 评测页的「两 run 对比」（FR-UI.5）。
//
// 关键决定：case 级分类（新增失败/已修复/仍失败/仍通过/不可判定/仅 A 有/仅 B 有）
// **不在前端算**，直接取 /api/diff。后端那条路由调用 eval::diff_runs —— 与
// `cue-eval --diff` 是同一份实现，"run_error 算不算失败""指纹不同要先声明不可比"
// 这些口径只能有一处，否则网页和 CI 迟早会给出不同结论。
// 前端只做展示：把两侧的汇总行摆成两列、把分类计数做成徽章、把逐 case p99 画成
// 并列条形（数据里没有时间序列，所以不画曲线）。
import { useCallback, useEffect, useMemo, useState } from 'react';
import { Card } from '../components/ui.jsx';
import { getJson, num } from '../api.js';

const CLASS_BADGE = {
  new_failure: 'bad',
  fixed: 'ok',
  still_failing: 'warn',
  still_passing: 'muted',
  inconclusive: 'mut',
  only_a: 'mut',
  only_b: 'mut',
};

function rate(run, k) {
  return run?.rate_map?.[k] ?? '—';
}

function casesText(r) {
  const c = r?.cases;
  if (!c) return '—';
  const n = (v) => (v === null || v === undefined ? '—' : v);
  return `${n(c.planned)} 计划 / ${n(c.executed)} 执行 / ${n(c.scored)} 计分 / ${n(c.error)} 运行失败`;
}

// 逐 case p99：报告「逐用例」表里列名带 p99（decode p99 / op p99 …）。取第一列；
// 值可能是 "n/a"（该段没采到分位），Number() 会得到 NaN，直接当"没有数据"处理。
function p99Of(run) {
  const rows = run?.case_rows || [];
  const headers = rows.length ? Object.keys(rows[0]).filter((k) => k !== 'ok') : [];
  const col = headers.find((h) => /p99/i.test(h)) || null;
  const values = new Map();
  if (!col) return { col: null, values };
  for (const row of rows) {
    const id = row.case ?? Object.values(row)[0];
    const v = Number(row[col]);
    if (Number.isFinite(v)) values.set(id, v);
  }
  return { col, values };
}

function FailureModes({ run, side }) {
  const modes = run?.failure_modes || [];
  if (!run) return <span className="muted">—</span>;
  if (modes.length === 0) {
    return (
      <span className="small muted" data-testid={`failure-modes-empty-${side}`}>
        无失败模式（本次没有判分未命中/run_error）
      </span>
    );
  }
  return (
    <ul className="evidence-list" data-testid={`failure-modes-${side}`}>
      {modes.map((m) => (
        <li key={m.text}>
          <span className="badge warn">{m.count ?? '—'}</span>
          <span className="small">{m.text}</span>
        </li>
      ))}
    </ul>
  );
}

export default function Compare({ runs }) {
  const [a, setA] = useState('');
  const [b, setB] = useState('');
  const [diff, setDiff] = useState(null);
  const [err, setErr] = useState('');
  const [loading, setLoading] = useState(false);

  // 默认取最新的两份（runs 已按 mtime 倒序）：最常做的对比就是"刚跑的和上一次"。
  useEffect(() => {
    if (!runs.length) return;
    setA((cur) => cur || runs[0]?.dir || '');
    setB((cur) => cur || runs[1]?.dir || runs[0]?.dir || '');
  }, [runs]);

  const load = useCallback(async (ra, rb) => {
    if (!ra || !rb) return;
    setLoading(true);
    const d = await getJson(`/api/diff?a=${encodeURIComponent(ra)}&b=${encodeURIComponent(rb)}`);
    if (d && d.ok) {
      setDiff(d);
      setErr('');
    } else {
      setDiff(null);
      setErr((d && d.error) || '对比失败');
    }
    setLoading(false);
  }, []);

  useEffect(() => {
    load(a, b);
  }, [a, b, load]);

  const runA = useMemo(() => runs.find((r) => r.dir === a) || null, [runs, a]);
  const runB = useMemo(() => runs.find((r) => r.dir === b) || null, [runs, b]);
  const pA = useMemo(() => p99Of(runA), [runA]);
  const pB = useMemo(() => p99Of(runB), [runB]);

  const rows = [
    ['用例数', casesText(runA), casesText(runB)],
    ['根因命中', rate(runA, 'root'), rate(runB, 'root')],
    ['证据成立', rate(runA, 'evidence'), rate(runB, 'evidence')],
    ['SLA 达成', rate(runA, 'sla'), rate(runB, 'sla')],
    ['pass^k', rate(runA, 'pass_k'), rate(runB, 'pass_k')],
    ['Run Error', rate(runA, 'run_error'), rate(runB, 'run_error')],
    ['MTTR', rate(runA, 'mttr'), rate(runB, 'mttr')],
    ['成本', `¥${num(runA?.cost_yuan, 4)}`, `¥${num(runB?.cost_yuan, 4)}`],
    ['agent / 模型', `${runA?.agent || '—'} / ${runA?.model || '—'}`, `${runB?.agent || '—'} / ${runB?.model || '—'}`],
    ['指纹', runA?.run_id ? String(diff?.a?.fingerprint || '—').slice(0, 12) : '—', runB?.run_id ? String(diff?.b?.fingerprint || '—').slice(0, 12) : '—'],
  ];

  // p99 并列条形：只画两侧至少一侧有数值的 case（n/a 不补 0，那会画出"0ms"的假象）
  const barCases = useMemo(() => {
    const ids = new Set([...pA.values.keys(), ...pB.values.keys()]);
    return Array.from(ids).sort();
  }, [pA, pB]);
  const p99Max = useMemo(() => {
    let m = 0;
    for (const v of pA.values.values()) m = Math.max(m, v);
    for (const v of pB.values.values()) m = Math.max(m, v);
    return m;
  }, [pA, pB]);

  const counts = diff?.counts || {};

  return (
    <Card
      title="两 run 对比"
      sub={
        diff
          ? `A ${diff.a.run_id} → B ${diff.b.run_id} · ${
              diff.comparable ? '可比' : '不可比（配置/指纹不同）'
            }${loading ? ' · 加载中' : ''}`
          : '选两个 run（分类由后端 eval::diff_runs 给出，与 cue-eval --diff 同源）'
      }
      right={
        <button data-testid="diff-refresh" onClick={() => load(a, b)} disabled={loading}>
          重新对比
        </button>
      }
      testid="compare-card"
    >
      <div className="row" style={{ marginBottom: 10 }}>
        <label className="f">
          A（基线）
          <select data-testid="diff-a" value={a} onChange={(e) => setA(e.target.value)}>
            {runs.length === 0 ? <option value="">（没有 run）</option> : null}
            {runs.map((r) => (
              <option key={`a-${r.dir}`} value={r.dir}>
                {r.dir}
              </option>
            ))}
          </select>
        </label>
        <label className="f">
          B（当前）
          <select data-testid="diff-b" value={b} onChange={(e) => setB(e.target.value)}>
            {runs.length === 0 ? <option value="">（没有 run）</option> : null}
            {runs.map((r) => (
              <option key={`b-${r.dir}`} value={r.dir}>
                {r.dir}
              </option>
            ))}
          </select>
        </label>
        {a === b ? <span className="badge warn">A 与 B 是同一个 run</span> : null}
      </div>

      {runs.length < 2 ? (
        <div className="empty" data-testid="compare-empty">
          至少需要两份报告才能对比。跑两次评测：
          <code>./build/cue-eval --suite suites/m4-diagnosis.json --mode scripted --out runs</code>
        </div>
      ) : null}

      {err ? (
        <div className="empty bad" data-testid="compare-error">
          {err}
        </div>
      ) : null}

      {diff ? (
        <>
          {!diff.comparable ? (
            <div className="warnbox" data-testid="diff-warning">
              <div className="small">两 run 不可比，差异不能归因于被测对象的改动：</div>
              {(diff.warnings || []).map((w) => (
                <div className="small" key={w}>
                  · {w}
                </div>
              ))}
            </div>
          ) : null}

          <div className="row" style={{ marginBottom: 10 }}>
            {[
              ['new_failures', '新增失败', counts.new_failures],
              ['fixed', '已修复', counts.fixed],
              ['still_failing', '仍失败', counts.still_failing],
              ['still_passing', '不变（仍通过）', counts.still_passing],
              ['inconclusive', '不可判定', counts.inconclusive],
              ['only_a', '仅 A 有', counts.only_a],
              ['only_b', '仅 B 有', counts.only_b],
            ].map(([key, label, n]) => (
              <span
                key={key}
                className={'badge ' + (n > 0 ? CLASS_BADGE[key === 'new_failures' ? 'new_failure' : key] : 'muted')}
                data-testid={`diff-count-${key}`}
              >
                {label} {n ?? '—'}
              </span>
            ))}
          </div>

          <table>
            <thead>
              <tr>
                <th>指标</th>
                <th>A（{diff.a.run_id}）</th>
                <th>B（{diff.b.run_id}）</th>
              </tr>
            </thead>
            <tbody>
              {rows.map(([k, va, vb]) => (
                <tr key={k} data-testid="diff-summary-row">
                  <td className="muted">{k}</td>
                  <td>{va}</td>
                  <td>{vb}</td>
                </tr>
              ))}
            </tbody>
          </table>

          <h3 style={{ fontSize: 13, margin: '14px 0 6px' }}>失败模式（A / B）</h3>
          <div className="row" style={{ alignItems: 'flex-start', gap: 24 }}>
            <div style={{ flex: 1 }}>
              <div className="small muted">A {diff.a.run_id}</div>
              <FailureModes run={runA} side="a" />
            </div>
            <div style={{ flex: 1 }}>
              <div className="small muted">B {diff.b.run_id}</div>
              <FailureModes run={runB} side="b" />
            </div>
          </div>

          <h3 style={{ fontSize: 13, margin: '14px 0 6px' }}>逐 case 对比</h3>
          {diff.cases.length === 0 ? (
            <div className="empty">两边都没有 case 级记录</div>
          ) : (
            <table>
              <thead>
                <tr>
                  <th>case</th>
                  <th>结论</th>
                  <th>A</th>
                  <th>B</th>
                </tr>
              </thead>
              <tbody>
                {diff.cases.map((c) => (
                  <tr key={c.id} data-testid="diff-row" data-class={c.cls}>
                    <td>{c.id}</td>
                    <td>
                      <span className={'badge ' + (CLASS_BADGE[c.cls] || 'muted')} data-testid="diff-class">
                        {c.name}
                      </span>
                    </td>
                    <td className="small">{c.a.text}</td>
                    <td className="small">{c.b.text}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}

          <h3 style={{ fontSize: 13, margin: '14px 0 6px' }}>
            逐 case {pA.col && pB.col && pA.col === pB.col ? pA.col : 'p99'}（并列条形，A 蓝 / B 绿）
          </h3>
          {barCases.length === 0 ? (
            <div className="empty" data-testid="p99-empty">
              两份报告都没有可用的逐 case p99（报告里该列是 n/a：该段没有采到分位），所以没有条形可画。
            </div>
          ) : (
            <div data-testid="p99-chart">
              {barCases.map((id) => {
                const va = pA.values.get(id);
                const vb = pB.values.get(id);
                const max = p99Max || 1;
                return (
                  <div key={id} data-testid="p99-row" data-case={id}>
                    <div className="small muted">{id}</div>
                    <div className="bar-row">
                      <span className="bar-label">A</span>
                      <span className="bar-track">
                        <span
                          className="bar-fill"
                          data-testid="p99-bar-a"
                          style={{ width: va ? `${(va / max) * 100}%` : '0%' }}
                        />
                      </span>
                      <span className="bar-val">{va === undefined ? 'n/a' : `${num(va, 1)} ms`}</span>
                    </div>
                    <div className="bar-row">
                      <span className="bar-label">B</span>
                      <span className="bar-track">
                        <span
                          className="bar-fill b"
                          data-testid="p99-bar-b"
                          style={{ width: vb ? `${(vb / max) * 100}%` : '0%' }}
                        />
                      </span>
                      <span className="bar-val">{vb === undefined ? 'n/a' : `${num(vb, 1)} ms`}</span>
                    </div>
                  </div>
                );
              })}
            </div>
          )}

          <p className="small muted" data-testid="no-timeseries" style={{ marginBottom: 0 }}>
            该 run 没有时间序列：report / state 只存逐 case 汇总（没有按秒采样的成功率曲线），
            所以这里用并列条形 + 逐 case 分类，不画曲线。分类口径：新增失败 / 已修复 / 仍失败 /
            仍通过（不变）/ 不可判定（run_error，不计入新增失败）/ 仅 A 有 / 仅 B 有。
          </p>
        </>
      ) : null}
    </Card>
  );
}
