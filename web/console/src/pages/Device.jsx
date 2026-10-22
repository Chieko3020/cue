// 设备页：接入的设备（cued 会话表）+ 采集端能力 + 推流源/管线入口状态 + 启停。
// "设备" 在本项目里有两层：服务器端 cued 的**接入设备**（会话表：device_id/peer/租约）
// 与**本浏览器**的采集能力（WebCodecs、摄像头数）。两张卡分开写是刻意的：
// 把两者混在一起时，"本浏览器支持 H.264" 会被读成"下面那台设备支持 H.264"。
import { useEffect, useRef, useState } from 'react';
import { Card, Metric, ActionResult } from '../components/ui.jsx';
import { num } from '../api.js';

// 采集端能力探测：与 M0 Spike-A 同一组问题（H.264 是否可编、AVC level 边界）
async function detectCaps() {
  const caps = {
    webcodecs: typeof window.VideoEncoder !== 'undefined',
    h264_l30: null,
    h264_l40: null,
    cameras: null,
    ua: navigator.userAgent,
  };
  if (caps.webcodecs) {
    const probe = async (codec, width, height) => {
      try {
        const r = await window.VideoEncoder.isConfigSupported({
          codec,
          width,
          height,
          bitrate: 1_500_000,
          framerate: 30,
        });
        return !!r.supported;
      } catch {
        return false;
      }
    };
    caps.h264_l30 = await probe('avc1.42001E', 640, 480);
    caps.h264_l40 = await probe('avc1.640028', 1280, 720);
  }
  try {
    const devs = await navigator.mediaDevices.enumerateDevices();
    caps.cameras = devs.filter((d) => d.kind === 'videoinput').length;
  } catch {
    caps.cameras = null;
  }
  return caps;
}

function yesNo(v) {
  if (v === null || v === undefined) return '—';
  return v ? '支持' : '不支持';
}

// 能力单元格：没有能力上报时明确写"未上报"，不要留空白——
// 空白会被读成"这台设备不支持"，而真相是"它没说"。
function capsText(caps) {
  if (!caps || !caps.declared) return '未上报';
  const parts = [];
  const max = caps.max || {};
  const nat = caps.native || {};
  if (max.width > 0 && max.height > 0) {
    parts.push(`≤${max.width}×${max.height}${max.fps > 0 ? `@${max.fps}` : ''}`);
  } else if (nat.width > 0 && nat.height > 0) {
    parts.push(`原生 ${nat.width}×${nat.height}`);
  }
  if (caps.codec) parts.push(caps.codec);
  if (caps.capture_path) parts.push(caps.capture_path);
  // 不拼 caps.name：浏览器上报的名字缺省是 UA 全文，塞进单元格会把这一列撑成两行
  // 而没有多一个信息量（设备身份看 device_id/peer 更直接）。
  if (parts.length === 0) return '已上报（无可用字段）';
  return parts.join(' · ');
}

// 租约倒计时：服务端给的是 expires_in_ms（"查询那一刻还剩多少毫秒"），不是绝对到期时刻，
// 所以这里以"收到这份快照的本地时刻"为起点递减：跨机时钟偏移不影响结果（只用相对量 +
// 本地流逝），误差只有轮询间隔/网络滞后（/api/state 每 1.5 s 一次，见 App.jsx）。
function useLeaseBase(state) {
  const base = useRef(Date.now());
  const [, tick] = useState(0);
  useEffect(() => {
    base.current = Date.now();
  }, [state]);
  useEffect(() => {
    const t = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(t);
  }, []);
  return base.current;
}

function leaseLeftMs(row, baseAt) {
  if (!row || !Number.isFinite(row.expires_in_ms)) return null;
  return row.expires_in_ms - (Date.now() - baseAt);
}

export default function Device({ state, runAction, busy, lastAction }) {
  const [caps, setCaps] = useState(null);
  useEffect(() => {
    let alive = true;
    detectCaps().then((c) => {
      if (alive) setCaps(c);
    });
    return () => {
      alive = false;
    };
  }, []);

  const m = state?.metrics || {};
  const p = state?.pipeline || {};
  const cfg = p.config || {};
  const running = !!p.running;
  const active = (m.frames_in || 0) > 0;

  // 设备表：/api/state 的 devices（cued 会话表原样透传）+ devices_summary（汇总）。
  // devices_error 非空 = 取不到（后端没有会话表 / 控制通道降级），与"没有设备"分开呈现。
  const devices = Array.isArray(state?.devices) ? state.devices : [];
  const dsum = state?.devices_summary || {};
  const devicesError = state?.devices_error || '';
  const liveDevices = devices.filter((d) => d.state === 'live');
  const closedDevices = devices.filter((d) => d.state !== 'live');
  const leaseBase = useLeaseBase(state);
  // 状态列只描述"当前接入"：已关闭的会话是历史，混进在线表会让"杀掉一路之后还是 2 行"
  // 被读成"设备还连着"。它们仍在下面单独一行里列出（cued 的 session.list 带
  // include_closed，用来排查"刚断开的是谁、为什么"）。
  const leaseHint =
    dsum.lease_ms > 0 ? `租约 ${(dsum.lease_ms / 1000).toFixed(0)}s` : '租约未知';

  return (
    <>
      <Card
        title="接入的设备"
        sub={`cued 会话表 · 在线 ${liveDevices.length}${
          dsum.max_sessions > 0 ? `/${dsum.max_sessions}` : ''
        } · ${leaseHint}`}
        testid="device-list-card"
      >
        {devicesError ? (
          <div className="empty" data-testid="devices-error">
            取不到设备表：{devicesError}
          </div>
        ) : null}
        {!devicesError && liveDevices.length === 0 ? (
          <div className="empty" data-testid="devices-empty">
            还没有设备接入。用 <code>cue-capture --backend lavfi</code>（合成源，无需摄像头）或浏览器推流页
            <code> web/index.html</code> 接入 cued。
          </div>
        ) : null}
        {liveDevices.length > 0 ? (
          <table>
            <thead>
              <tr>
                <th>device_id</th>
                <th>peer</th>
                <th>状态</th>
                <th>能力</th>
                <th>租约剩余</th>
              </tr>
            </thead>
            <tbody>
              {liveDevices.map((d) => {
                const left = leaseLeftMs(d, leaseBase);
                // 租约耗尽但会话还在表里 = 已过期（cued 的回收线程每 100 ms 扫一次，
                // 所以这个状态通常只可见 0～100 ms；真正到期后设备会移到下面的「最近断开」）。
                const expired = left !== null && left <= 0;
                return (
                  <tr
                    key={d.session_id}
                    data-testid="device-row"
                    data-device-id={d.device_id}
                    data-state={d.state}
                  >
                    <td>{d.device_id ?? '—'}</td>
                    <td className="muted">{d.peer ?? '—'}</td>
                    <td>
                      <span
                        className={'badge ' + (expired ? 'bad' : 'ok')}
                        data-testid="device-state"
                      >
                        {expired ? '已过期' : 'live'}
                      </span>
                    </td>
                    <td className="small">{capsText(d.caps)}</td>
                    <td
                      className="num"
                      data-testid="device-lease"
                      title={`lease_ms=${d.lease_ms ?? '?'} expires_in_ms=${d.expires_in_ms ?? '?'}`}
                    >
                      {left === null ? '—' : left <= 0 ? '已过期' : `${(left / 1000).toFixed(1)}s`}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        ) : null}
        {closedDevices.length > 0 ? (
          <p className="small muted" data-testid="devices-closed-note" style={{ marginBottom: 0 }}>
            最近断开 {closedDevices.length} 台：
            {closedDevices
              .map(
                (d) =>
                  `${d.device_id ?? '—'}（${
                    d.state === 'closing' ? '断开中' : d.reason || '已关闭'
                  }）`,
              )
              .join('、')}
          </p>
        ) : null}
      </Card>

      <Card title="采集端（本浏览器）" sub="本地能力探测，不上传任何内容">
        <p className="small muted" data-testid="caps-scope-note" style={{ marginTop: 0 }}>
          这一卡探测的是<strong>打开这个页面的浏览器</strong>（WebCodecs / 摄像头），
          与上面「接入的设备」是两回事：上面那些设备是 cued 的 WS 上行连接，各自有自己的能力上报。
        </p>
        {caps ? (
          <table>
            <tbody>
              <tr>
                <th>WebCodecs VideoEncoder</th>
                <td>{yesNo(caps.webcodecs)}</td>
              </tr>
              <tr>
                <th>H.264 L3.0（avc1.42001E，≤640×480）</th>
                <td>{yesNo(caps.h264_l30)}</td>
              </tr>
              <tr>
                <th>H.264 L4.0（avc1.640028，≤1280×720）</th>
                <td>{yesNo(caps.h264_l40)}</td>
              </tr>
              <tr>
                <th>视频输入设备</th>
                <td>{caps.cameras === null ? '未授权/不可读' : `${caps.cameras} 个`}</td>
              </tr>
              <tr>
                <th>User-Agent</th>
                <td className="small muted wrap">{caps.ua}</td>
              </tr>
            </tbody>
          </table>
        ) : (
          <div className="empty">探测中…</div>
        )}
        <p className="small muted" style={{ marginBottom: 0 }}>
          推流源可用 <code>scripts/test-client.mjs</code>（服务器本机合成帧）或 Windows Edge 的真实摄像头；
          编码参数受 M0 实测约束：720p 及以上必须用 avc1.640028。
        </p>
      </Card>

      <Card title="管线入口" sub="WS 上行 / UDP 下行">
        <div className="grid">
          <Metric k="cued 状态" v={running ? '运行中' : '已停止'} small testid="device-cued" />
          <Metric k="pid" v={m.pid ?? '—'} small testid="device-pid" />
          <Metric k="WS 端口" v={cfg.ws_port ?? '—'} small />
          <Metric k="UDP 端口" v={cfg.probe_port ?? '—'} small />
          <Metric k="推流源" v={active ? '有帧在到达' : '无帧'} small testid="device-source" />
          <Metric k="帧率 fps" v={num(m.fps, 1)} small testid="device-fps" />
          <Metric k="累计入帧" v={m.frames_in ?? 0} small testid="device-frames-in" />
          <Metric k="指标采样" v={m.samples ?? 0} small />
        </div>
        <div className="row" style={{ marginTop: 12 }}>
          <button
            className="primary"
            data-testid="ctl-start"
            disabled={busy > 0 || running}
            onClick={() => runAction({ action: 'start' })}
          >
            启动管线
          </button>
          <button
            data-testid="ctl-stop"
            disabled={busy > 0 || !running}
            onClick={() => runAction({ action: 'stop' })}
          >
            停止管线
          </button>
          <button
            data-testid="ctl-restart"
            disabled={busy > 0}
            onClick={() => runAction({ action: 'restart' })}
          >
            重启管线
          </button>
          <span className="small muted">
            启动/停止是 mutating 动作：审批模式为 ask 时会先出现在「审批」页
          </span>
        </div>
        <div style={{ marginTop: 10 }}>
          <ActionResult result={lastAction} />
        </div>
      </Card>
    </>
  );
}
