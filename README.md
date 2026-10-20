# Cue

用 C++23 写的 Agent Harness：驱动一条真实的低延迟音视频管线，并用可复现的自动评测回答"agent 到底有没有把系统变好"。

- **Harness** — 阶段状态机约束下的 ReAct 内核，领域工具、审批、沙箱、上下文预算、事件流
- **媒体管线** — 浏览器/原生客户端采集编码 → WebSocket 上行 → 解码 → 可插拔转码 → 自研 UDP 下行
- **探针客户端** — 跨机 C++ 接收端，分片重组、NACK、FEC、抖动缓冲，输出端到端指标
- **评测** — 声明式用例、确定性故障注入、三重判分、回归对比

> 状态：**M1 端到端最小链路已跑通**——浏览器 Canvas 合成源 → WebCodecs 编码 → WS 上行 → cued 转发 → 自研 UDP → probe 输出帧率与延迟分位（实测 30 fps、p50 6.4 ms）。M2 起做完整媒体管线（解码/转码/六段埋点/FEC/NACK）。

## 为什么是这个拼法

链路排障长期靠人肉：逐段加埋点、抓包、比对日志，才能判断故障落在网络、缓冲、时钟还是编码。这类判断有规律，agent 可以做；但业界没有 C++ 的 agent runtime（官方 SDK 全在 Python/TS），现有框架也回答不了"改完之后是变好还是变坏"。

所以这里把三件事拆开：**harness**（怎么让 agent 安全地动手）、**被测域**（有指标和参数的真实系统）、**评测**（怎么证明它有效）。音视频管线是第一个被测域，不是唯一一个。

## 架构

```
浏览器（设备侧）              harness                    mediad                  probe
────────────────       ─────────────────────      ─────────────────      ─────────────────
React 控制台           agent 内核                  媒体管线               下行接收端
 · getUserMedia        外层：阶段状态机             WS 接收/重组/抖动缓冲   · 重组 / NACK / FEC
 · getDisplayMedia     内层：ReAct 工具循环         解码（libavcodec）      · 抖动缓冲
 · WebCodecs 编码      工具层（三级风险）           算子图（缩放/转码）     · 解码 + 指标
 · WS 上行  ─────────▶ session / 上下文预算         编码（libx264）        · 跨平台
 · SSE 事件 ◀────────  审批 · 沙箱 · 事件流          发送（整形/FEC）
 · HTTP 控制           评测引擎（case/注入/判分）    ◀── 损伤注入点（用户态）
```

三条通道职责分离：媒体走 WebSocket（二进制），事件走 SSE（`Last-Event-ID` 断点续传），控制走 HTTP。

## 技术栈

| 领域 | 选型 |
|---|---|
| 语言 | C++23（GCC ≥ 14） |
| 构建 | CMake |
| HTTP / WS | Boost.Beast |
| 编解码 | FFmpeg 6.1（libavcodec、libswscale）、libx264、libopus |
| 传输 | 自研 UDP 协议（分片 / NACK / XOR-FEC / 令牌桶） |
| 沙箱 | landlock + rlimit（默认）、docker（可选） |
| 前端 | React + Vite + TypeScript |

## 快速开始

依赖（Ubuntu 24.04）：

```bash
sudo apt install -y g++-14 cmake pkg-config \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libx264-dev libopus-dev libboost-dev nlohmann-json3-dev libspdlog-dev libfmt-dev
```

构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

一键起全套（probe + cued + 静态页），然后打开提示的地址点「开始推流」：

```bash
./scripts/run.sh
```

跨机演示时用 `BIND=0.0.0.0 ./scripts/run.sh`（注意防火墙）。

## 目录

```
apps/          cue / cued / cue-probe / cue-eval 四个入口
include/cue/   proto · media · pipeline · agent · tool · llm · eval · server
web/           React 控制台
suites/        评测用例（含 real-faults 真实故障复现集）
scripts/       run.sh · bench.sh · ci.sh
third_party/   vendored 单头依赖
```

## 设计文档

设计期文档（需求、技术、讨论记录、调研报告）不随仓库分发，位于本地 `docs/`。

## 说明

跨主机场景下的端到端延迟是**相对量**：它依赖握手阶段的时钟偏移估计（目标误差 < 5 ms），不含最小网络传播时间，也不含上屏。任何延迟数字都会同时给出测量口径。
