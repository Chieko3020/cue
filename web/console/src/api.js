// 与 cue-serve 通信的最小封装：所有请求同源，没有任何外部依赖（无 CDN）。
// 失败一律返回 {ok:false,error}，绝不 throw 到渲染层——控制台要能显示"后端挂了"。

export async function postJson(url, body) {
  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
    });
    let data = null;
    try {
      data = await res.json();
    } catch {
      data = null;
    }
    return { http: res.status, ...(data || { ok: false, error: `HTTP ${res.status}` }) };
  } catch (e) {
    return { ok: false, status: 'failed', error: `请求失败：${e && e.message ? e.message : e}` };
  }
}

export async function getJson(url, { timeoutMs = 4000, signal } = {}) {
  const controller = new AbortController();
  const abort = () => controller.abort();
  if (signal?.aborted) controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  const timer = setTimeout(abort, timeoutMs);
  try {
    const res = await fetch(url, { cache: 'no-store', signal: controller.signal });
    if (!res.ok) return { ok: false, error: `HTTP ${res.status}` };
    return await res.json();
  } catch (e) {
    return { ok: false, error: `请求失败：${e && e.message ? e.message : e}` };
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}

// 指标缺样本是 null（"没采到 ≠ 0"），渲染成 —，不要偷偷变成 0
export function num(v, digits = 2) {
  if (v === null || v === undefined) return '—';
  if (typeof v === 'number') {
    if (!Number.isFinite(v)) return '—';
    return digits === 0 ? String(Math.round(v)) : v.toFixed(digits);
  }
  return String(v);
}

export function pct(v) {
  if (v === null || v === undefined) return '—';
  return String(v);
}

// 事件时间：服务端给墙钟微秒
export function clock(us) {
  if (!us) return '—';
  const d = new Date(us / 1000);
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
}

export function ago(us) {
  if (!us) return '';
  const s = Math.max(0, (Date.now() * 1000 - us) / 1e6);
  if (s < 60) return `${s.toFixed(0)}s 前`;
  if (s < 3600) return `${(s / 60).toFixed(0)}m 前`;
  return `${(s / 3600).toFixed(1)}h 前`;
}

// 事件一行的摘要：只取最有用的一两个字段，完整 JSON 在下方灰色小字里
export function eventSummary(ev) {
  const d = ev.data || {};
  switch (ev.ev) {
    case 'metric.sample':
      return `fps=${num(d.fps, 1)} in=${d.frames_in} out=${d.frames_out} dropped=${d.dropped} injected=${d.injected}`;
    case 'pipeline.started':
      return `pid=${d.pid ?? '—'} bind=${d.bind ?? '—'} ${d.width ?? '?'}x${d.height ?? '?'} ${d.bitrate_kbps ?? '?'}kbps gop=${d.gop ?? '?'}`;
    case 'pipeline.stopped':
      return `reason=${d.reason ?? '—'} pid=${d.pid ?? '—'}`;
    case 'pipeline.restart':
      return `reason=${d.reason ?? '—'} ${d.key ? `${d.key}=${d.value}` : ''} pid=${d.pid ?? '—'}`;
    case 'fault.applied':
      return `${d.kind ?? '—'}=${d.value ?? '—'}% scope=${d.scope ?? '—'}`;
    case 'fault.cleared':
      return `${d.kind ?? '—'} 已取消`;
    case 'tool.call':
      return `${d.tool}（${d.risk}）`;
    case 'tool.result':
      return `${d.tool} → ${d.status}${d.error ? `：${d.error}` : ''}`;
    case 'approval.request':
      return `#${d.id} ${d.tool}（${d.risk}）等待审批`;
    case 'approval.decision':
      return `#${d.id} ${d.tool} → ${d.outcome}`;
    case 'codec.config':
      return `${d.codec ?? 'h264'} 配置 ${d.sps_len ?? '?'}/${d.pps_len ?? '?'} 字节`;
    default:
      return '';
  }
}
