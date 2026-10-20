// 审批页：待审批列表（工具名/风险级/参数/倒计时）+ 允许/拒绝 + 最近决定。
// 数据来自 /api/state 的 approvals（服务端审批队列是唯一事实来源），
// SSE 的 approval.request / approval.decision 让页面在 1.5s 轮询之外也立即反应。
import { useState } from 'react';
import { Card, RiskBadge } from '../components/ui.jsx';
import { postJson } from '../api.js';
import { clock, ago } from '../api.js';

export default function Approval({ state, tick }) {
  const [notice, setNotice] = useState(null);
  const [deciding, setDeciding] = useState(false);

  const pending = state?.approvals?.pending || [];
  const recent = (state?.approvals?.recent || []).slice().reverse();
  const timeoutS = state?.approvals?.timeout_s ?? 30;
  const mode = state?.approvals?.mode ?? '—';

  async function decide(id, allow) {
    setDeciding(true);
    const r = await postJson('/api/approve', { id, allow });
    setDeciding(false);
    setNotice({ id, allow, ...r });
  }

  return (
    <>
      <Card
        title="待审批"
        sub={`模式 ${mode} · 超时 ${timeoutS}s 自动拒绝 · ${pending.length} 条待处理`}
        testid="approval-card"
      >
        {mode === 'ask' ? null : (
          <p className="small muted">
            当前不是 ask 模式：{mode === 'auto' ? '危险动作自动批准（已留痕）' : '非只读动作一律拒绝'}。
          </p>
        )}
        {pending.length === 0 ? (
          <div className="empty" data-testid="approval-empty">
            没有待审批请求。在「管线」页点「注入丢包」等危险/变更动作会在这里出现。
          </div>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            {pending.map((it) => {
              const left = Math.max(0, (it.created_us / 1000 + (it.timeout_ms || timeoutS * 1000) - Date.now()) / 1000);
              return (
                <div className="approve-item" data-testid="approval-item" data-tool={it.tool} key={it.id}>
                  <div className="head">
                    <b>#{it.id}</b>
                    <b>{it.tool}</b>
                    <RiskBadge risk={it.risk} />
                    <span className="muted small">{clock(it.created_us)}</span>
                    <span className={'countdown' + (left < 10 ? ' low' : '')} data-testid="approval-countdown">
                      {left.toFixed(0)}s 后自动拒绝
                    </span>
                    <span style={{ marginLeft: 'auto' }} className="row">
                      <button
                        className="ok"
                        data-testid="approval-allow"
                        disabled={deciding}
                        onClick={() => decide(it.id, true)}
                      >
                        允许
                      </button>
                      <button
                        className="danger"
                        data-testid="approval-deny"
                        disabled={deciding}
                        onClick={() => decide(it.id, false)}
                      >
                        拒绝
                      </button>
                    </span>
                  </div>
                  <div className="args">{JSON.stringify(it.args, null, 2)}</div>
                  <div className="small muted">
                    审批只影响这一次调用；批准后由 Registry 执行，结果会推 tool.result
                  </div>
                </div>
              );
            })}
          </div>
        )}
        {notice ? (
          <div
            className={'action-result ' + (notice.ok ? 'ok' : 'bad')}
            data-testid="approval-notice"
            style={{ marginTop: 10 }}
          >
            #{notice.id} {notice.allow ? '允许' : '拒绝'} → {notice.ok ? '已提交' : notice.error || '失败'}
          </div>
        ) : null}
      </Card>

      <Card title="最近决定" sub="来自服务端审批队列历史（最多 64 条，显示最近 20）">
        {recent.length === 0 ? (
          <div className="empty">还没有审批记录</div>
        ) : (
          <table>
            <thead>
              <tr>
                <th>#</th>
                <th>工具</th>
                <th>风险</th>
                <th>结果</th>
                <th>参数</th>
                <th>决定时间</th>
              </tr>
            </thead>
            <tbody>
              {recent.map((h) => (
                <tr key={`${h.id}-${h.decided_us}`} data-testid="approval-history-row">
                  <td>{h.id}</td>
                  <td>{h.tool}</td>
                  <td>
                    <RiskBadge risk={h.risk} />
                  </td>
                  <td>
                    <span className={`badge ${h.outcome === 'allowed' ? 'ok' : 'bad'}`}>{h.outcome}</span>
                  </td>
                  <td className="small muted wrap" style={{ maxWidth: 420 }}>
                    {JSON.stringify(h.args)}
                  </td>
                  <td className="small muted">{ago(h.decided_us)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>
    </>
  );
}
