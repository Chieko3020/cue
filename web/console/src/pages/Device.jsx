// 设备页：采集端能力 + 推流源/管线入口状态 + 启停。
// "设备" 在本项目里有两层：服务器端 cued 管线（pid/端口/存活）与浏览器采集端
// （WebCodecs 能力、摄像头数）。两者都只读本地/同源信息，不连任何外部服务。
import { useEffect, useState } from 'react';
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

  return (
    <>
      <Card title="采集端（本浏览器）" sub="本地能力探测，不上传任何内容">
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
