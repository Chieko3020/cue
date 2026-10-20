# Cue

用 C++23 写的 Agent Harness：驱动一条真实的低延迟视频管线，并用可复现的自动评测回答"agent 到底有没有把系统变好"。

- **Harness** — 阶段状态机约束下的 ReAct 内核：领域工具、三级审批、上下文预算
- **媒体管线** — 浏览器 Canvas 合成源 → WebCodecs 编码 → WebSocket 上行 → 解码 → 可插拔转码 → 自研 UDP 下行
- **探针客户端** — 跨机 C++ 接收端，分片重组、NACK、FEC，输出端到端指标
- **评测** — 声明式用例、确定性故障注入、三重判分、回归对比

> 状态：**M4b 完成**。媒体管线（libavcodec 解码、转码、六段埋点、令牌桶、XOR-FEC、NACK、确定性丢包注入）、agent 内核（阶段状态机 + ReAct 循环 + 4 个工具 + 审批 + 上下文预算）、评测引擎（case/注入/三重判分/断点续跑/回归对比）均已落地，`ctest` 6/6。**M5 控制台（React 五页、SSE 事件流、审批 UI）与 M6 加固（cassette、ASan/UBSan/TSan、CI、benchmark）未开始。**

## 为什么是这个拼法

链路排障长期靠人肉：逐段加埋点、抓包、比对日志，才能判断故障落在网络、缓冲、时钟还是编码。这类判断有规律，agent 可以做；但业界没有 C++ 的 agent runtime（官方 SDK 全在 Python/TS），现有框架也回答不了"改完之后是变好还是变坏"。

所以这里把三件事拆开：**harness**（怎么让 agent 安全地动手）、**被测域**（有指标和参数的真实系统）、**评测**（怎么证明它有效）。音视频管线是第一个被测域，不是唯一一个。

## 架构

```
浏览器（设备侧）              harness                    cued                    cue-probe
────────────────       ─────────────────────      ─────────────────      ─────────────────
推流页（原生 HTML/JS）  agent 内核                  媒体管线               下行接收端
 · Canvas 合成源        外层：阶段状态机             WS 接收/重组            · 重组 / NACK / FEC
 · WebCodecs 编码       内层：ReAct 工具循环         解码（libavcodec）      · 指标
 · WS 上行  ─────────▶  工具层（三级风险）           算子图（缩放/转码）     · 跨机部署
 · SSE 事件（M5 计划）  session / 上下文预算         编码（libx264）
 · HTTP 控制（M5 计划） 审批 · 事件流（M5）           发送（整形/FEC）
                        评测引擎（case/注入/判分）   ◀── 损伤注入点（用户态）
```

当前只实现了**媒体通道**：浏览器 ↔ `cued` 的 WebSocket（二进制，32 字节头 + 分片）。事件 SSE（`Last-Event-ID` 续传）与控制 HTTP 属于 M5，仓库里尚无实现；`cue-probe` 只做重组/NACK/FEC 与指标统计，不解码。

## 技术栈

| 领域 | 选型 |
|---|---|
| 语言 | C++23（GCC ≥ 14，`std::expected`） |
| 构建 | CMake |
| HTTP / WS | Boost.Beast |
| 编解码 | FFmpeg 6.1（libavcodec、libavutil、libswscale）、libx264 |
| 传输 | 自研 UDP 协议（分片 / NACK / XOR-FEC / 令牌桶） |
| 前端 | 原生 HTML + Canvas + WebCodecs（零构建、无外部 CDN） |
| LLM 接入 | libcurl（OpenAI 兼容接口） |
| 音频 | Opus（SPEC §7.8，未实现） |
| 沙箱 | landlock + rlimit（SPEC §6 M3 要求，未实现） |

## 快速开始

依赖（Ubuntu 24.04）：

```bash
sudo apt install -y g++-14 cmake pkg-config \
  libavcodec-dev libavutil-dev libswscale-dev libx264-dev \
  libcurl4-openssl-dev libboost-dev libboost-system-dev nlohmann-json3-dev
```

`libboost-system-dev` 不能省：`find_package(Boost COMPONENTS system)` 在只有头文件时会失败（Boost.System 的库虽已 header-only，CMake 的 FindBoost 仍要求该组件可解析）。

构建（CMake 会拒绝 GCC < 14，所以显式指定 `g++-14`）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j
```

> 默认 `CUE_WERROR=ON`：`-Wall -Wextra -Wpedantic -Werror`，与 SPEC §9 的 CI 口径一致，本仓库全量构建 0 warning。只想看警告、不愿被中断时，重新配置加 `-DCUE_WERROR=OFF`。

单测：

```bash
ctest --test-dir build --output-on-failure     # 6 项，约 1 s
```

一键起媒体链路（`cue-probe` + `cued` + 静态页），然后打开脚本打印的地址点「开始推流」：

```bash
./scripts/run.sh            # 等价 ./scripts/run.sh demo-min
```

> `run.sh` 只起媒体链路，不起 agent、不跑评测。脚本会把 URL 打印成 `http://127.0.0.1:8080/?ws_port=9000`，改过 `WS_PORT` 也能对上。
>
> **监听地址**：`cued` 默认只监听 `127.0.0.1`（`--bind 0.0.0.0` 才对外）。`run.sh` 把 `BIND` 同时传给静态页与 cued，跨机演示时 `BIND=0.0.0.0 ./scripts/run.sh`，并自行用防火墙限制 9000/UDP 9001。

agent 演示（脚本化 LLM，不联网、不花钱）：

```bash
./build/cue demo
```

评测（scripted 模式，不需要真实 LLM）：

```bash
./build/cue-eval --suite suites/m4-diagnosis.json --mode scripted --repeat 1
```

链路自检（另开一个终端，在 `run.sh` 跑着的时候）：

```bash
node scripts/test-client.mjs ws://127.0.0.1:9000/ 5
```

它发送一段内嵌的 64×64 Annex-B IDR，`cued` 会真实解码；不含编码器，也不覆盖多分片重组。

浏览器端到端验证（可选，需要 playwright 和 headless Chromium，非运行依赖）：

```bash
NODE_PATH=<playwright 的 node_modules> node scripts/e2e-browser.cjs 6
```

页面 0 帧、编码器报错、`bad` 级日志或页面异常都会让脚本以非 0 退出。

## 目录

```
apps/          cue / cued / cue-probe / cue-eval 四个入口
include/cue/   proto · media · pipeline · agent · tool · llm · eval · common
src/           与 include 分层对应的实现
web/           推流页（Canvas 合成源 + WebCodecs + WS 上行）
suites/        评测用例（m2-constraints.json、m4-diagnosis.json）
scripts/       run.sh · test-client.mjs · e2e-browser.cjs
tests/         无框架单测（手写 check，靠 ctest 退出码）
```

## 设计文档

设计期文档（需求、技术、讨论记录、调研报告）不随仓库分发，位于本地 `docs/`。

## 说明

- **延迟口径**：跨主机场景下的端到端延迟是相对量，依赖握手阶段的时钟偏移估计（目标误差 < 5 ms）。页面在连接时锚定时钟，此后每 30 s 重估，漂移 > 5 ms 重锚；重锚目前只写页面日志，结构化事件留痕要等 M5 的事件流。数字不含最小网络传播时间，也不含上屏。
- **上行编码**：关键帧间隔由页面固定为 2 s（`fps × 2`），`suites` 里的 `pipeline.gop` 只影响服务端转码，不影响浏览器上行。
- **重连**：`cued` 重启后页面自动重连，并在重连成功后的第一帧强制 IDR（断线窗口里不消费这个标志），新解码器无需等 2 s GOP 即可恢复（实测 kill/restart cued 后 `dec_err=0`）；上行 `codec_config`（type `0x03`）尚未实现。
