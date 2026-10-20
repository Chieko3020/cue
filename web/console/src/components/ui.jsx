// 通用小件：卡片 / 按钮 / 风险徽章，避免五个页面各写一套（也没有引任何 UI 库）
export function Card({ title, sub, right, children, testid }) {
  return (
    <section className="card" data-testid={testid}>
      {title ? (
        <h2>
          {title}
          {sub ? <span className="sub">{sub}</span> : null}
          {right ? <span className="right">{right}</span> : null}
        </h2>
      ) : null}
      {children}
    </section>
  );
}

export function Metric({ k, v, small, testid }) {
  return (
    <div className="metric" data-testid={testid} title={k}>
      <div className="k">{k}</div>
      <div className={'v' + (small ? ' small' : '')}>{v}</div>
    </div>
  );
}

export function RiskBadge({ risk }) {
  const cls = risk === 'dangerous' ? 'bad' : risk === 'mutating' ? 'mut' : 'muted';
  return <span className={`badge ${cls}`}>{risk}</span>;
}

export function StatusBadge({ ok, children }) {
  return <span className={`badge ${ok ? 'ok' : 'bad'}`}>{children}</span>;
}

// 动作结果面板：e2e 与人工都靠它确认"批准后真的执行了"
export function ActionResult({ result }) {
  if (!result) {
    return (
      <div className="action-result muted" data-testid="action-result">
        尚无动作结果
      </div>
    );
  }
  const ok = result.ok;
  const status = result.status || (ok ? 'executed' : 'failed');
  const lines = [
    `${result.tool || result.req?.name || result.req?.action || '?'} → ${status}`,
    ok ? '已执行' : `失败：${result.error || '未知原因'}`,
    result.req ? `请求：${JSON.stringify(result.req)}` : '',
    result.content ? `返回：${result.content}` : '',
  ].filter(Boolean);
  return (
    <div className={'action-result ' + (ok ? 'ok' : 'bad')} data-testid="action-result">
      {lines.join('\n')}
    </div>
  );
}
