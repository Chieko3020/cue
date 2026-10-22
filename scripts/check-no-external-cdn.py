#!/usr/bin/env python3
"""AC-13 的可执行证据：前端静态资源不得引用任何外部 URL（断网可加载）。

为什么不用浏览器跑：AC-13 的原文是「无外部 CDN、断网可加载」。用 headless Chromium
去"看它有没有发外部请求"需要 playwright（本机手工装、CI runner 没有），于是 CI 里
这条验收长期是空的（e2e-console 每次都 `exit 0`）。静态检查是同一件事的更便宜判据：
源码与产物里根本没有外部地址，断网自然加载不出来外部请求。

两类检查，口径不同：

  1. 加载位置（src/href/import/fetch/new URL/new WebSocket/url()/@import）——
     这里出现外网地址一定会在运行时发起请求，**没有任何白名单**，一律失败。
     只有回环地址（localhost/127.0.0.1/[::1]）放行：那是本机 dev server 的代理地址，
     不依赖外网。协议相对地址（//cdn.example/x.js）等价于外网，同样失败。
  2. 裸 URL 字面量（文件里任何 http(s)://...）—— 允许显式白名单，但每条都要给出
     理由，且必须逐条打印出来。当前白名单只有两类：
       * w3.org 的 XML/SVG/MathML 命名空间标识：createElementNS 拿它做字符串比较，
         不是网络地址；
       * react-dom 生产包里的 error-decoder 文档链接：字符串常量，生产构建不抛该
         消息，也不会请求它。
     白名单按前缀匹配；将来有人往 dist 里塞 CDN，会以"未白名单"的形式失败。

用法：
  ./scripts/check-no-external-cdn.py            # 退出码 0 = PASS，1 = FAIL
  ./scripts/check-no-external-cdn.py --verbose  # 打印每条本地引用/白名单命中

扫描范围（按技术设计 §13 的 AC-13 口径）：
  web/index.html、web/app.js、web/console/index.html、web/console/src/**、
  web/dist/**（若存在——CI 里通常不存在，本地构建过才有）。
  web/console/vite.config.js 不在范围：那是开发期工具配置（/api 代理到本机 8080），
  不会进入产物；把它算进来只会激励"为了过检查把本地代理写成变量"。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 扫描的文本扩展名：前端产物只有这几类；.map 是调试用的源码映射，不参与加载
TEXT_SUFFIXES = {".html", ".htm", ".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".css", ".svg"}

# 显式白名单：(URL 前缀, 理由)。只用于「裸 URL 字面量」检查，加载位置检查不做例外。
ALLOWLIST: list[tuple[str, str]] = [
    ("http://www.w3.org/", "XML/SVG/MathML 命名空间标识符（createElementNS 的参数，不发起请求）"),
    (
        "https://reactjs.org/docs/error-decoder.html",
        "react-dom 生产包内嵌的错误说明链接（字符串常量，不发起请求）",
    ),
]

# 加载位置：HTML 属性 + JS 导入/请求 + CSS url()
LOAD_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("src/href 属性", re.compile(r"""\b(?:src|href|srcset|poster|action|formaction|data|manifest|background)\s*=\s*["']([^"']*)["']""")),
    ("import", re.compile(r"""\bfrom\s*["']([^"']+)["']""")),
    ("import()", re.compile(r"""\bimport\s*\(\s*["']([^"']+)["']""")),
    ("side-effect import", re.compile(r"""\bimport\s+["']([^"']+)["']""")),
    ("require()", re.compile(r"""\brequire\s*\(\s*["']([^"']+)["']""")),
    ("fetch()", re.compile(r"""\bfetch\s*\(\s*["']([^"']+)["']""")),
    (
        "new URL/Worker/WebSocket/EventSource",
        re.compile(
            r"""new\s+(?:URL|Worker|SharedWorker|EventSource|WebSocket|AudioWorkletNode)\s*\(\s*["']([^"']+)["']"""
        ),
    ),
    ("css url()", re.compile(r"""url\(\s*["']?([^"'()]*)["']?\s*\)""")),
    ("@import", re.compile(r"""@import\s+(?:url\()?\s*["']([^"']+)["']""")),
]

URL_LITERAL = re.compile(r"""https?://[^\s"'<>)\]}`]+""")

# 回环地址：本机 dev server / 开发代理，不属于「外部 CDN」
LOOPBACK = re.compile(r"^(?:https?|wss?)://(?:localhost|127\.0\.0\.1|\[::1\])(?::\d+)?(?:[/?#]|$)")


def collect_files() -> tuple[list[Path], list[Path], list[Path]]:
    """返回 (入口文件, 源码目录文件, dist 文件)，缺文件时返回空列表而不是造一个空跑。"""
    entries = [ROOT / "web" / "index.html", ROOT / "web" / "app.js", ROOT / "web" / "console" / "index.html"]
    sources: list[Path] = []
    dist: list[Path] = []
    for base, out in ((ROOT / "web" / "console" / "src", sources), (ROOT / "web" / "dist", dist)):
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.is_file() and p.suffix in TEXT_SUFFIXES and "node_modules" not in p.parts:
                out.append(p)
    return [p for p in entries if p.is_file()], sources, dist


def rel(p: Path) -> str:
    return str(p.relative_to(ROOT))


def main() -> int:
    verbose = "--verbose" in sys.argv[1:]
    entries, sources, dist = collect_files()

    # 入口文件缺失 = 检查没有对象，不能报 PASS（那又是一次假绿）
    missing = [p for p in ("web/index.html", "web/app.js") if not (ROOT / p).is_file()]
    if missing:
        print(f"[ac13] FAIL: 缺少待检查的入口文件：{', '.join(missing)}")
        return 1
    if not sources:
        print("[ac13] FAIL: web/console/src 下没有可扫描的源码文件（目录结构变了？）")
        return 1

    source_files = list(entries) + sources
    bare_targets = source_files + dist

    failures: list[str] = []

    # ---- 检查项 1：加载位置 ----
    local_refs = 0
    external_refs: list[tuple[str, int, str, str]] = []
    for path in source_files + dist:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            failures.append(f"{rel(path)}: 读不出来（{e}）")
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for kind, pattern in LOAD_PATTERNS:
                for m in pattern.finditer(line):
                    value = m.group(1).strip()
                    if not value:
                        continue
                    # 协议相对地址（//cdn.example/x.js）是外网：没有 scheme 也要挡住
                    if value.startswith("//"):
                        external_refs.append((rel(path), lineno, kind, value))
                        continue
                    if not value.startswith(("http://", "https://", "ws://", "wss://")):
                        local_refs += 1
                        if verbose:
                            print(f"[ac13]   本地引用 {rel(path)}:{lineno} [{kind}] {value}")
                        continue
                    if LOOPBACK.match(value):
                        local_refs += 1
                        if verbose:
                            print(f"[ac13]   回环引用 {rel(path)}:{lineno} [{kind}] {value}")
                        continue
                    external_refs.append((rel(path), lineno, kind, value))

    for path, lineno, kind, value in external_refs:
        failures.append(f"{path}:{lineno} 加载位置引用了外部地址（{kind}）：{value}")

    # ---- 检查项 2：裸 URL 字面量（可白名单） ----
    allowed_hits: dict[str, tuple[int, str, list[str]]] = {}
    unlisted: list[tuple[str, int, str]] = []
    for path in bare_targets:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            failures.append(f"{rel(path)}: 读不出来（{e}）")
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in URL_LITERAL.finditer(line):
                url = m.group(0).rstrip(".,;:")
                prefix_reason = next(((pre, why) for pre, why in ALLOWLIST if url.startswith(pre)), None)
                if prefix_reason is None:
                    unlisted.append((rel(path), lineno, url))
                    continue
                pre, why = prefix_reason
                count, _, files = allowed_hits.setdefault(pre, (0, why, []))
                files = files + [rel(path)] if rel(path) not in files else files
                allowed_hits[pre] = (count + 1, why, files)

    for path, lineno, url in unlisted:
        failures.append(
            f"{path}:{lineno} 出现未白名单的外部 URL 字面量：{url}"
            "（若是误报，在 ALLOWLIST 里加前缀并写清理由）"
        )

    # ---- 输出（每一项都给文件数与结论） ----
    print(f"[ac13] 范围：入口 {len(entries)} 个 + 源码 {len(sources)} 个 + dist {len(dist)} 个"
          f"（共 {len(bare_targets)} 个文件；web/dist 为构建产物，CI checkout 后通常不存在）")
    print(f"[ac13] 检查项 1/2 加载位置（src/href/import/fetch/new URL/url()）："
          f"扫描 {len(source_files) + len(dist)} 个文件，本地引用 {local_refs} 处，"
          f"外网引用 {len(external_refs)} 处 → {'PASS' if not external_refs else 'FAIL'}")
    total_allowed = sum(c for c, _, _ in allowed_hits.values())
    print(f"[ac13] 检查项 2/2 裸 URL 字面量：扫描 {len(bare_targets)} 个文件，"
          f"命中白名单 {total_allowed} 处，未白名单 {len(unlisted)} 处 → "
          f"{'PASS' if not unlisted else 'FAIL'}")
    for pre, (count, why, files) in sorted(allowed_hits.items()):
        print(f"[ac13]   白名单 {pre} × {count}（{', '.join(files)}）：{why}")

    if failures:
        print("[ac13] 失败项：")
        for f in failures:
            print(f"[ac13]   - {f}")
        print("[ac13] 结论：FAIL —— 前端存在外部地址引用，AC-13（无外部 CDN、断网可加载）不成立")
        return 1

    print("[ac13] 结论：PASS —— 前端无外部 CDN 引用（断网可加载）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
