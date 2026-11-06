// 管线页：实时指标（ffps/六段分位/dropped/injected/serr）+ 参数表单（set_param）+ 启停。
// 指标一律来自 cue-serve 的 /api/state → metrics_json（与工具层同一份口径）：
// 分位无样本显示 —，绝不把 null 画成 0。
import { useEffect, useRef, useState } from 'react';
import { Card, Metric, ActionResult } from '../components/ui.jsx';
import { num } from '../api.js';

const SEGMENTS = ['recv', 'buffer', 'decode', 'op', 'encode', 'send'];

const PRESETS = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium', 'slow'];

export default function Pipeline({ state, runAction, busy, lastAction }) {
  // 算子链（FR-UI.2）：把六段埋点映射成用户能一眼看懂的流水线。
  // 「active」的判据是**这一段真的在做事**，而不是"配置里写了"：
  //   转码关闭时 op/encode 两段是直通，标灰；抖动缓冲没有样本时同样标灰。
  // 这样"配置说开了但实际没跑"与"真的在跑"在界面上是两种样子。

  const m = state?.metrics || {};
  const cfg = (state && state.pipeline && state.pipeline.config) || {};
  const STAGES = [
    { key: 'recv', label: '接收', active: true, detail: num(m.recv_p50_ms, 2), cost: 'WS 上行',
      hint: 'WS 收包到组装的耗时' },
    { key: 'buffer', label: '缓冲', active: m.buffer_p50_ms != null, detail: num(m.buffer_p50_ms, 2),
      cost: '抖动缓冲', hint: '进队 → 放行；没有样本说明这一段没参与' },
    { key: 'decode', label: '解码', active: true, detail: num(m.decode_p50_ms, 2), cost: 'H264',
      hint: '单帧解码耗时' },
    { key: 'op', label: '算子', active: !!cfg.transcode, detail: cfg.transcode ? num(m.op_p50_ms, 2) : '直通',
      cost: cfg.transcode ? `scale/转码 ${cfg.preset || ''}`.trim() : '未转码',
      hint: 'ProcessorChain：passthrough / scale / transcode' },
    { key: 'encode', label: '编码', active: !!cfg.transcode, detail: cfg.transcode ? num(m.encode_p50_ms, 2) : '直通',
      cost: cfg.transcode ? `${cfg.bitrate_kbps || '?'} kbps gop${cfg.gop || '?'}` : '复用原码流',
      hint: '关闭转码时直接封装原码流，不重新编码' },
    { key: 'send', label: '发送', active: true, detail: num(m.send_p50_ms, 2), cost: 'UDP 下行',
      hint: '出队到 syscall 的耗时' },
  ];
  const running = !!state?.pipeline?.running;

  // 表单只在首次拿到配置时初始化，之后不随轮询覆盖（否则用户正在输入的值会被冲掉）
  const [form, setForm] = useState({ bitrate_kbps: '', gop: '', preset: 'veryfast', transcode: false });
  const [inject, setInject] = useState('5');
  const [applyMsg, setApplyMsg] = useState('');
  const inited = useRef(false);
  useEffect(() => {
    if (inited.current || !state?.pipeline?.config) return;
    inited.current = true;
    setForm({
      bitrate_kbps: String(cfg.bitrate_kbps ?? 1500),
      gop: String(cfg.gop ?? 60),
      preset: cfg.preset || 'veryfast',
      transcode: !!cfg.transcode,
    });
  }, [state, cfg]);

  async function applyParams() {
    setApplyMsg('');
    const cur = state?.pipeline?.config || {};
    const changes = [];
    if (String(cur.bitrate_kbps ?? '') !== String(form.bitrate_kbps)) {
      changes.push(['bitrate_kbps', String(form.bitrate_kbps)]);
    }
    if (String(cur.gop ?? '') !== String(form.gop)) changes.push(['gop', String(form.gop)]);
    if ((cur.preset || '') !== form.preset) changes.push(['preset', form.preset]);
    if (!!cur.transcode !== !!form.transcode) changes.push(['transcode', form.transcode ? 'on' : 'off']);
    if (changes.length === 0) {
      setApplyMsg('没有改动');
      return;
    }
    const results = [];
    // 逐个提交：每次 set_param 都会重启 cued（会短暂断流），串行更可控
    for (const [key, value] of changes) {
      const r = await runAction({ action: 'set_param', key, value });
      results.push(`${key}=${value} → ${r.ok ? 'ok' : r.error || '失败'}`);
    }
    setApplyMsg(results.join('；'));
  }

  return (
    <>
      <Card title="实时指标" sub={state ? `采样 ${m.samples ?? 0} 条 · age ${num(m.age_ms / 1000, 1)} s` : '等待快照'}>
        <div className="grid">
          <Metric k="fps" v={num(m.fps, 1)} testid="metric-fps" />
          <Metric k="frames_in" v={m.frames_in ?? 0} testid="metric-frames-in" />
          <Metric k="frames_out" v={m.frames_out ?? 0} testid="metric-frames-out" />
          <Metric k="dropped" v={m.dropped ?? 0} testid="metric-dropped" />
          <Metric k="injected" v={m.injected ?? 0} testid="metric-injected" />
          <Metric k="serr（发送错误）" v={m.send_errors ?? 0} testid="metric-serr" />
          <Metric k="rejected" v={m.rejected ?? 0} />
          <Metric k="encode_errors" v={m.encode_errors ?? 0} />
          <Metric k="decode_drops" v={m.decode_drops ?? 0} />
          <Metric k="budget_drops" v={m.budget_drops ?? 0} />
          <Metric k="alive / pid" v={`${m.alive ? 'yes' : 'no'} / ${m.pid ?? '—'}`} small />
          <Metric k="transcode" v={cfg.transcode ? 'on' : 'off'} small />
        </div>
      </Card>

      <Card title="算子链" sub="数字是该段 p50；灰底 = 这一段的处理被直通（不参与）">
        <div className="flow" data-testid="pipeline-flow" style={{ display: 'flex', alignItems: 'stretch', gap: 6, flexWrap: 'wrap' }}>
          {STAGES.map((s, i) => (
            <span key={s.key} style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
              {i > 0 ? <span style={{ color: 'var(--muted, #888)' }}>→</span> : null}
              <span
                data-testid={`stage-${s.key}`}
                data-active={s.active ? '1' : '0'}
                title={s.hint}
                style={{
                  padding: '6px 10px',
                  borderRadius: 6,
                  border: '1px solid ' + (s.active ? 'var(--line, #444)' : 'transparent'),
                  background: s.active ? 'transparent' : 'rgba(128,128,128,.12)',
                  color: s.active ? 'inherit' : 'var(--muted, #888)',
                  minWidth: 92,
                  textAlign: 'center',
                }}
              >
                <div style={{ fontSize: 12, opacity: .75 }}>{s.label}</div>
                <div style={{ fontSize: 15, fontVariantNumeric: 'tabular-nums' }}>{s.detail}</div>
                <div style={{ fontSize: 11, opacity: .6 }}>{s.cost}</div>
              </span>
            </span>
          ))}
        </div>
        <div style={{ marginTop: 8, fontSize: 12, opacity: .7 }}>
          {cfg.transcode ? '转码开启：解码 → scale/转码 → 重新编码' : '转码关闭（passthrough）：解码后直接封装，不重编'}
          {' · '}
          {m.buffer_p50_ms == null ? '抖动缓冲这段没有样本' : `抖动缓冲 p50 ${num(m.buffer_p50_ms, 2)} ms`}
        </div>
      </Card>

      <Card title="六段耗时（ms）" sub="p50 / p95 / p99；— 表示这段没有样本，不是 0">
        <table>
          <thead>
            <tr>
              <th>段</th>
              <th className="num">p50</th>
              <th className="num">p95</th>
              <th className="num">p99</th>
            </tr>
          </thead>
          <tbody>
            {SEGMENTS.map((s) => (
              <tr key={s} data-testid={`seg-${s}`}>
                <td>{s}</td>
                {['p50', 'p95', 'p99'].map((p) => (
                  <td key={p} className="num">{num(m[`${s}_${p}_ms`], 2)}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </Card>

      <Card title="参数（set_param）" sub="修改会重启 cued，造成数百 ms 断流">
        <div className="row">
          <label className="f">
            码率 kbps
            <input
              data-testid="param-bitrate"
              value={form.bitrate_kbps}
              onChange={(e) => setForm({ ...form, bitrate_kbps: e.target.value })}
            />
          </label>
          <label className="f">
            GOP
            <input
              data-testid="param-gop"
              value={form.gop}
              onChange={(e) => setForm({ ...form, gop: e.target.value })}
            />
          </label>
          <label className="f">
            preset
            <select
              data-testid="param-preset"
              value={form.preset}
              onChange={(e) => setForm({ ...form, preset: e.target.value })}
            >
              {PRESETS.map((p) => (
                <option key={p} value={p}>
                  {p}
                </option>
              ))}
            </select>
          </label>
          <label className="f">
            转码
            <select
              data-testid="param-transcode"
              value={form.transcode ? 'on' : 'off'}
              onChange={(e) => setForm({ ...form, transcode: e.target.value === 'on' })}
            >
              <option value="off">关（透传）</option>
              <option value="on">开</option>
            </select>
          </label>
          <button
            className="primary"
            data-testid="btn-apply-params"
            disabled={busy > 0}
            onClick={applyParams}
          >
            应用参数
          </button>
        </div>
        <p className="small muted">
          当前生效：{cfg.width ?? 0}×{cfg.height ?? 0}（0×0 = 沿用解码尺寸）· {cfg.bitrate_kbps ?? '—'} kbps ·
          gop {cfg.gop ?? '—'} · preset {cfg.preset ?? '—'} · inject_drop {num(cfg.inject_drop, 1)}%
        </p>
        <p className="small muted">
          分辨率（width/height）需成对设置，而 set_param 一次只接受一个 key，因此页面上只读展示；
          降分辨率请走 agent 的 pipeline_set_param 或直接改启动参数。
        </p>
        {applyMsg ? <div className="action-result">{applyMsg}</div> : null}
      </Card>

      <Card title="损伤注入与启停" sub="inject_fault 是 dangerous：ask 模式下必须人工批准">
        <div className="row">
          <label className="f">
            丢包 %
            <input
              data-testid="param-inject-drop"
              value={inject}
              onChange={(e) => setInject(e.target.value)}
            />
          </label>
          <button
            className="danger"
            data-testid="ctl-inject"
            disabled={busy > 0}
            onClick={() => runAction({ action: 'inject', kind: 'drop', value: Number(inject) })}
          >
            注入丢包
          </button>
          <button
            data-testid="ctl-inject-clear"
            disabled={busy > 0}
            onClick={() => runAction({ action: 'inject', kind: 'drop', value: 0 })}
          >
            取消注入
          </button>
          <span className="muted">|</span>
          <button
            className="primary"
            data-testid="ctl-start"
            disabled={busy > 0 || running}
            onClick={() => runAction({ action: 'start' })}
          >
            启动
          </button>
          <button data-testid="ctl-stop" disabled={busy > 0 || !running} onClick={() => runAction({ action: 'stop' })}>
            停止
          </button>
          <button data-testid="ctl-restart" disabled={busy > 0} onClick={() => runAction({ action: 'restart' })}>
            重启
          </button>
        </div>
        <div style={{ marginTop: 10 }}>
          <ActionResult result={lastAction} />
        </div>
      </Card>
    </>
  );
}
