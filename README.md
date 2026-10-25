# Cue

Cue 是 C++23 的 Agent Harness：驱动一条真实的低延迟视频管线，并用可复现的自动评测回答「agent 到底有没有把系统变好」。之所以这么拼，是因为链路排障的规律（看哪一段的哪个计数在动）本来就可以交给 agent，但业界没有 C++ 的 agent runtime，也没有框架能回答「改完之后是变好还是变坏」。

- **Harness** — 阶段状态机约束下的 ReAct 内核：领域工具、三级审批、沙箱、上下文预算、cassette
- **被测域** — 三条采集路径（摄像头+麦克风 / 屏幕共享 / Canvas 合成源，前两条在真机验证过）→ WebCodecs 编码 → WebSocket 上行 → 解码 → 可插拔转码 → 自研 UDP 下行；另有 `cue-capture`（原生采集端，`lavfi`/`ffmpeg`/V4L2 三后端）走同一套上行协议；`cued` 是独立进程，故障注入在用户态、固定种子
- **评测** — 声明式 case、三重判分（根因 / 证据 / SLA）、pass^k、指纹、回归对比、人读报告
- **可观测** — `cue-serve` + React 控制台五页、SSE 事件流、审批 UI；`cue-probe` 跨机接收端

当前状态：主构建 `ctest` **33 项全绿**（C++ 单测 29 + 静态检查 3 + 起真进程的集成测试 1；2026-10-25 复核）；`build-asan`（ASan+UBSan）与 `build-tsan` 各 **32 项全绿**（不注册集成测试）。控制台五个 tab、SSE 与审批 UI 可用；cassette、benchmark 脚本（`scripts/bench.sh`，数据不入库）、proto fuzz、CI workflow 都在仓库里。**AC-15 全量通过**（18 case × REPEAT=3 = 54 次，根因/证据/SLA 全 1.0，三次 repeat 的 stddev 全 0）。**AC-1 已在真机验证**：Windows 的集成摄像头 + Realtek 麦克风经浏览器推流，cued 侧 `frames_in 594` / `audio_frames_in 1000` / `decode_errors 0`。

CI 分两层：**常驻**（每次 push）跑 build + ctest、AC-13 静态检查、e2e-console；**手动**（`workflow_dispatch`）跑 sanitizer（ASan/UBSan、TSan）与 fuzz。sanitizer 不常驻的原因写在 `.forgejo/workflows/sanitizer.yml` 头部：本机 2 vCPU / 1967 MB（DSH 常驻近 1 GB）+ runner 的 `MemoryHigh=700M`，插桩构建连 `-g1` 都压不进预算（`MemoryCurrent` 恒贴 734M，单 TU 编 5 分钟）。

## 为什么是这个拼法

链路排障长期靠人肉：逐段加埋点、抓包、比对日志，才能判断故障落在网络、缓冲、时钟还是编码。这类判断有规律，agent 可以做；但现有框架回答不了「改完之后是变好还是变坏」。

所以把三件事拆开：**harness**（怎么让 agent 安全地动手）、**被测域**（有指标和参数的真实系统）、**评测**（怎么证明它有效）。音视频管线是第一个被测域，不是唯一一个；`--endpoint` 可以把被测域换成任意实现同一套 REST 控制协议的服务，harness、工具层、评测引擎一行不改。

## 架构

```
设备侧（浏览器）              harness（cue）                  cued                    cue-probe
────────────────       ──────────────────────────      ────────────────────    ─────────────────
推流页（原生 JS）       agent 内核                      媒体管线                下行接收端
 · Canvas 合成源        外层：阶段状态机                  WS 接收 / 重组          · 重组 / NACK / FEC
 · WebCodecs 编码        内层：ReAct 工具循环              解码（libavcodec）      · 帧率 / 端到端分位
 · WS 上行 ───────────▶  工具层（风险级 + 审批 + 沙箱）    算子图（缩放 / 转码）   · 跨机部署
 · SSE / HTTP ◀────────  session / 上下文预算              编码（libx264）
                         评测引擎（case / 注入 / 判分）    发送（整形 / FEC）
                                                          ◀── 损伤注入点（用户态）

控制台（React 五页）──── HTTP + SSE ──▶ cue-serve
```

通道只有两条媒体链路：浏览器 ↔ `cued` 的 WebSocket（二进制，32 字节头 + 分片），`cued` ↔ `cue-probe` 的 UDP（24 字节头，NACK / XOR-FEC / 令牌桶）。harness 不碰帧数据，只能通过工具面看管线；控制台与 harness 共用同一套 `Registry` 和管线控制，所以「控制台能做的 agent 也能做」。

## 部件

### harness（`src/agent`、`src/tool`、`src/llm`、`src/sandbox`）

外层是阶段状态机 `collect → diagnose → repair → verify → done/give_up`，内层是 ReAct 工具循环。工具用声明式 spec 描述参数与风险级，校验分三层（JSON 类型 → 枚举/范围 → 语义），审批分 `ReadOnly` / `Mutating` / `Dangerous` 三级，模式分 `Ask` / `Auto` / `Deny`。上下文有 token 预算：超长观测按字节截断并留 `…[truncated N bytes]` 标记，不静默丢字段。

边界：harness 层（`agent/`、`tool/`、`eval/`）不得 include `media/`、`pipeline/`——CMake 用**分层 include 视图**在构建期物理隔离：`cue_agent`/`cue_eval` 只拿到 `{common,tool,agent,llm,eval,cli}` 这些目录，越界 include 直接 `fatal error: No such file`。这条由 ctest 的 `layer_isolation` 长期守着（一条正例 + 两条反例）。工具层只认识 `IServicePipeline` 接口，不知道背后是子进程、HTTP 服务还是录制回放。

### 被测域（`src/media`、`src/pipeline`）

`cued` 负责 WS 接收、分片重组、**抖动缓冲**、解码、`ProcessorChain`（passthrough / scale / transcode）、编码、UDP 发送，六段埋点（recv / buffer / decode / op / encode / send）逐帧计时、每段给 p50/p95/p99。令牌桶、XOR-FEC（每 N 包一组，可调）、NACK 与**六种损伤注入**（`drop` / `ingress_drop` / `rate` / `backlog` / `delay` / `jitter`）都在发送路径上，同一 seed 逐包可复现。控制面走 UDS（`SOCK_SEQPACKET` 优先，STREAM + JSON-line 兜底），所以改参数与注入**不重启进程、不断流**；只有结构性参数（端口、bind、FEC 开关、抖动缓冲结构）仍需重启。

`pipeline/` 是适配层：`ProcessPipelineControl` 把 `cued` 当子进程拉起，从 stdout 解析指标与事件；`HttpServicePipeline` 连接实现 `GET /api/state` + `POST /api/action` 的外部服务（`cue-mock-svc` 是其中一个确定性假域）。

边界：帧数据只在浏览器/`cue-capture` ↔ `cued` ↔ `cue-probe` 之间流动，不跨进程进 harness。**端到端分位已回传**：harness 托管探针并解析它的输出，`IServicePipeline::probe_latest()` 把端到端 p50/p95/p99、卡顿、四段分解（recv/reassembly/buffer/decode）交给工具与判分；探针不可用时报 `run_error=probe_unavailable`，不退回 `cued` 侧指标。

### 评测（`src/eval`）

声明式 case：注入参数、prompt、期望根因、允许动作、SLA、预算。引擎按 case 起管线、注入故障、跑 agent、在 agent 结束后重新测量、按三重口径判分，产出 `runs/<run_id>/report.md` 与 `state.jsonl`（断点续跑用）。同一 run 有文件锁，并发 resume 不会双份计费。

边界：`scripted` 模式是引擎自检，不判模型；`real` 模式默认回放 cassette，缺录制直接记 `run_error`，不会静默联网。

### 可观测（`src/eval` 的事件、`apps/cue-serve.cpp`、`web/console`）

`cue-serve` 提供 `GET /events`（SSE，`id` 单调递增，环形缓冲 512 条，支持 `Last-Event-ID` 补齐；更旧的事件靠 `/api/state` 全量快照恢复）、`GET /api/state` 快照、`POST /api/action` 控制动作、`POST /api/approve` 审批决策，并托管 `web/dist`。控制台五页是设备、管线、会话、审批、评测；审批页展示待审批队列与倒计时，动作必须走 `Registry::call_guarded`，不直接碰管线。

运维面另有两个端点：`GET /healthz` 是纯文本 `ok`（给探活脚本/反代，和前端的 JSON `/api/health` 并存），`GET /metrics` 是 JSON（`curl -s http://127.0.0.1:8080/metrics | python3 -m json.tool`）。`/metrics` 覆盖技术设计 §12.3 的 10 项（`cue_agent_turns_total` / `cue_tool_calls_total` / `cue_llm_tokens_total` / `cue_llm_cost_yuan_total` / `cue_pipeline_p99_us` / `cue_pipeline_drop_frames_total` / `cue_probe_nack_sent_total` / `cue_probe_fec_recovered_total` / `cue_eval_cases_total` / `cue_process_rss_bytes`）外加 FR-OBS.3 的进程级读数（CPU、日志队列深度与 debug 丢弃数、连接数、SSE 客户端数）。拿不到的量写 0 并带 `note`/顶层 `_note` 说明"这是没有 run，不是测出来 0"；RSS/CPU 来自 `/proc`，CPU 是两次采样差的单核百分比（首拍为 0）。

结构化日志（FR-OBS.1）走 spdlog 异步 sink：队列 8192，满时丢 `debug` 并计数（计数在 `/metrics` 的 `cue_log_dropped_debug_total` 里），JSON 由 nlohmann 序列化后整串写入，字段固定为 `ts_us/level/proc/run_id/case_id/session_id/component/event/msg`（可选 `err`）。默认 JSON 写 stderr；`--log-file` 额外写轮转文件（`--log-level` / `--log-text` / `--log-json`）。迁移范围只到 `cue-serve` 与 `cue-eval`：`cued` / `cue-probe` 的 stdout 是 harness 的解析输入（`[cue-event]`、`METRICS`、`probe.metric` 行），改格式会直接打断控制面，所以保持原样。

边界：控制台是单机开发工具，没有鉴权、没有多租户；跨机部署需要自己在前面加一层（例如 nginx + SSO）。

## 技术栈

| 领域 | 选型 |
|---|---|
| 语言 | C++23（GCC 14，`std::expected`；CMake 拒绝 GCC < 14） |
| 构建 | CMake（`-Wall -Wextra -Wpedantic -Werror`，默认开） |
| HTTP / WS | Boost.Beast |
| 编解码 | FFmpeg 6.1（libavcodec / libavutil / libswscale）+ libx264 |
| 传输 | 自研 UDP 协议（分片 / NACK / XOR-FEC / 令牌桶） |
| 前端 | React 18 + Vite 6（控制台）；推流页原生 HTML + Canvas + WebCodecs |
| 日志 | spdlog 1.12（系统包，异步 sink 8192 + 文件轮转）+ fmt 9.1；JSON 由 nlohmann 序列化 |
| LLM 接入 | libcurl（OpenAI 兼容接口） |
| 沙箱 | landlock + rlimit + 降权（可选 docker 后端） |

## 快速开始

以下命令都在仓库根目录执行。依赖（Ubuntu 24.04）：

```bash
sudo apt install -y g++-14 cmake pkg-config \
  libavcodec-dev libavutil-dev libswscale-dev libx264-dev \
  libcurl4-openssl-dev libboost-dev libboost-system-dev nlohmann-json3-dev \
  libspdlog-dev libfmt-dev
```

`libboost-system-dev` 不能省：`find_package(Boost COMPONENTS system)` 在只有头文件时会失败。

构建（必须显式指定 `g++-14`：CMake 拒绝 GCC < 14，而 Ubuntu 24.04 默认 `g++` 是 13）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j1
```

`-j1` 是这台机器的硬约束：2 vCPU / 1.9 GiB 上 `-j2` 同时编两个 libavcodec 相关 TU 会 OOM。默认 `CUE_WERROR=ON`，本仓库全量构建 0 warning；只看警告不愿被中断时重新 configure 加 `-DCUE_WERROR=OFF`。全量首次构建在 2 vCPU 上约 7 分钟。

测试（22 项：20 个进程内单测 + 集成测试 + AC-13 静态检查；全量约 30 秒）：

```bash
ctest --test-dir build --output-on-failure
```

集成测试（技术设计 §13.2；ctest 里的 `integration` 就是它，起真进程、约 27 秒）：

```bash
python3 tests/test_integration.py            # BUILD_DIR=build-ci 可指定别的构建目录
```

三条判据：本机回环 10 s 的帧率 / 解码错误 / 端到端 p50·p99；`--inject-drop 5` 下 NACK 与 FEC 的真实计数，以及接收端整帧缺失率（≤5%，不修复时约 23%）；`kill -9` 托管中的 `cued` 后 supervisor 的 `pipeline.restart` 事件、pid 变化与重新吃帧——这条是 AC-2 唯一的自动化验证。sanitizer 构建不注册它（它测的不是内存安全，见 `CMakeLists.txt` 的说明）。

AC-13（前端无外部 CDN、断网可加载）的静态检查，不依赖浏览器与网络，`ctest` 里的 `no_external_cdn`：

```bash
./scripts/check-no-external-cdn.py --verbose
```

fuzz（libFuzzer，默认不编译；需要 clang，一次 3×60 s，只在 CI 手动触发，见 `.forgejo/workflows/ci.yml` 的 `fuzz` job）：

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCUE_FUZZ=ON
cmake --build build-fuzz --target fuzz_decode_media fuzz_decode_datagram fuzz_decode_fec_header -j1
ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0 \
  ./build-fuzz/fuzz_decode_media tests/fuzz/corpus/decode_media -max_total_time=60 -max_len=18432
```

`-max_len=18432` 只对 `decode_media` 必要：`FragmentTooLarge`（单分片 > 16 KiB）要求输入超过 16416 字节，而 libFuzzer 默认上限是 4096，不显式放宽就永远到不了那个分支。另外两个入口的合法输入分别 ≤24/8 字节，默认上限已经够。

跑一次脚本化 agent 演示（不联网、不花钱、不启动管线）：

```bash
./scripts/run.sh demo
```

它只构建 `cue` 一个 target，把完整诊断过程与结果写到 `runs/demo-<时间>.txt`，并在最后打印路径。干净 clone 首次运行（含 configure 与构建）实测 134 秒；只跑这一条时不需要先手动构建，脚本会自己 configure 并只编译 `cue`。

## 运行入口

| 入口 | 作用 | 是否联网 / 启动管线 |
|---|---|---|
| `./scripts/run.sh demo` / `cue demo` | 脚本化 LLM 走完状态机，演示工具调用、阶段迁移、成本记账 | 都不 |
| `cue run` | 真 LLM（`CUE_API_KEY`、`CUE_BASE_URL`、`CUE_MODEL`）+ 真 `cued` 管线，启动前注入 5% 丢包 | 联网，起管线 |
| `cue-eval` | 评测 CLI：suite、模式、repeat、成本闸门、断点续跑、回归对比 | 取决于 `--live` / 后端 |
| `cued` | 媒体管线进程（WS 进、UDP 出） | 起管线 |
| `cue-probe` | 下行接收端：重组、NACK、FEC、帧率与端到端分位 | 起管线 |
| `cue-capture` | 原生采集端：`lavfi` / `ffmpeg`（dshow·avfoundation）/ V4L2 三后端，上行 WS 协议与浏览器同一套 | 起管线（`demo-min` 的默认采集源） |
| `cue-serve` | 控制台后端：HTTP + SSE + 静态托管 + 审批队列。**`/` 是控制台（五页），`/stream` 是推流页（原生 JS，不参与构建）** | 起控制台 |
| `cue-mock-svc` | 确定性假域服务，实现同一套 REST 控制协议 | 起假域 |

停全部进程用 `./scripts/stop.sh`：按**精确进程名**（不是 `pkill -f 路径`，那会把自己也杀掉——这个坑踩过两次）依次 SIGTERM → 宽限 5 s → SIGKILL，并清理被 SIGKILL 的进程留下的 `/tmp/cue/<pid>/` 控制 socket 目录。
`--dry-run` 只列不停、`--force` 直接 KILL、`--keep-probe` 保留跨机的探针。

`cue demo` 的变体：`--http URL [--record DIR]` 走 OpenAI 兼容端点并录制 cassette；`--replay DIR` 从 cassette 回放。`cue run` 的 `--approval ask|auto|deny`、`--allow-dangerous NAMES|none` 控制审批；`CUE_RECORD=1` 把成功调用录进 `CUE_CASSETTE_DIR`。

`run.sh` 不带参数（或 `demo-min`）会起「`cue-probe` + `cued` + 采集源 + 静态页」，在终端打印 `http://127.0.0.1:8080/?ws_port=9000`。采集源默认是 `cue-capture --backend lavfi`（`testsrc2` 画面 + `sine` 音频，零采购真实源；走与浏览器同一套上行协议）；没有该二进制时回退到 `scripts/test-client.mjs`（Node 模拟浏览器）。`CUE_PRODUCER=page` 则不自动产帧，改由页面点「开始推流」——`cued` 一次只接一个客户端，采集端跑着时页面连不上，这是预期行为。`cued` 默认只监听 `127.0.0.1`；跨机演示用 `BIND=0.0.0.0` 并自行限制 9000/TCP 与 9001/UDP。 若接收端在 NAT 后面，`cue-probe --hello-to HOST:PORT` 会周期性发 `CUEHELLO`，`cued` 据此学会把下行发回来源地址——**这只是让公网测试能跑起来的脚手架，不是 NAT 穿透方案**（没有 ICE/STUN/TURN），生产部署仍要固定端口或做端口映射。改过 `WS_PORT` 也能对上。

控制台（可选）：`./scripts/serve.sh` 会先构建 `web/console`（首次需要 Node 与 npm，会联网装前端依赖），再起 `cue-serve`，默认 `http://127.0.0.1:8080/`、审批模式 `ask`。

单一配置文件的用法（模板：仓库根的 `cue.example.json`）：

```bash
./build/cue-serve --config cue.example.json            # 命令行 > 配置文件 > 内置默认值
./build/cue-eval  --config cue.example.json --repeat 1 # 命令行覆盖配置文件里的 repeat
./build/cue-serve --config cue.example.json --port 9099 --log-level debug --log-file logs/cue.jsonl
```

配置文件是 JSON（省一个解析器：nlohmann 已在依赖里），启动时校验端口范围、路径存在性、枚举值与互斥项（`record_tools_dir` 与 `replay_tools_dir`），非法配置**拒绝启动并给出行号**，例如 `cue.example.json:5: 端口超出范围 …`。JSON 不支持注释，`_` 开头的键当注释被忽略；不写某个键就保持内置默认值，所以加配置文件不改变任何既有命令行默认语义。

评测示例：

```bash
# scripted：引擎自检，不需要 LLM（不判模型）
./build/cue-eval --suite suites/m4-diagnosis.json --mode scripted --repeat 1

# 仓库里唯一入库的 cassette（假端点合成，只验证录制→回放链路）
./build/cue demo --replay suites/cassettes/demo

# real 模式默认回放；cassette 不随仓库分发，先用自己的 key 录一份
./build/cue-eval --live --record --suite suites/m4-diagnosis.json    # 需要 CUE_API_KEY
./build/cue-eval --replay --suite suites/m4-diagnosis.json --cassette-dir cassettes
# 缺录制时记 run_error=cassette_miss，不会静默联网

# 真管线的工具观测：录制一次、回放 3 次（脚本包含合成推流与逐条断言）
REPEAT=3 ./scripts/test-replay-consistency.sh
REPEAT=3 ./scripts/test-real-faults.sh

# 回归对比（退出码见下）
./build/cue-eval --diff run-<a> run-<b>
```

`scripts/` 下的验证脚本都是自包含的：`test-real-faults.sh`（五类真实故障 × repeat，断言根因、SLA 与诊断特征）、`test-replay-consistency.sh`（真管线录制 + 回放 3 次）、`test-shell-audit.sh`（白名单拒绝与审计）、`test-cancel.sh`（取消后无残留）、`bench.sh`（六段与端到端分位）。前两个需要 node + python3 + ss，`test-cancel.sh` 与 `test-shell-audit.sh` 需要 python3，`bench.sh` 需要 node + ffmpeg；缺依赖时脚本第一行就会报缺什么。它们会占用 9000/9001/8080，跑之前先停掉 `run.sh` 与控制台。

## 评测口径

判分只看三件事，agent 自报的结论与数字不参与：

1. **根因**：`diagnosis.root_cause` 与 `case.expect.root_cause` 做 8 类枚举的精确匹配。
2. **证据**：每条 evidence 必须是对某次工具观测的逐字 `key=value` 摘抄——归一化后必须是观测的子串，或由观测里的同名 key 值一致（计数类精确匹配，连续量允许 ±2%）。出现解释性文字、中文句子、合并多次观测都会判伪；任何一个 key 不存在或值不符即失配（fail-closed）。这条是防幻觉的唯一闸门。
3. **SLA**：agent 结束后由 harness 重新测量（取最后 3 条采样、约 1 秒一条的中位 fps；`dropped` 取窗口增量而非进程累计值），不采信 agent 自报。

`run_error` 的 case 不计入成功率分母。`pass^k` 是 repeat=k 时 k 次全部通过的 case 占比。

**产帧源（`--producer lavfi`）**：默认 `none` = 靠外部推流，行为与以前完全一致（不多起进程、命令行不多参数）。`lavfi` 让 `cue-eval` 自己起 `cue-capture`（`testsrc2` 合成画面，默认 640×360@1200 kbps，参数形式同 `scripts/run.sh`）：每个 case 在 `cued` 起来之后拉起，case 结束（成功/失败/run_error）连同它的 `ffmpeg` 子进程一起收掉，会话日志落在 `<run_dir>/producer.log`。需要它是因为 `rate` 类故障注入的是**发送速率上限**——令牌桶只有在源码率高于上限时才会耗尽；外部脚本推的是同一个 64×64 静止 IDR（约 677 B/帧），经 x264 后码率极低，30 fps 合计远低于 0.1 Mbps，于是 `budget_drops=0`、`fps 30`，故障根本没发生而 SLA（`fps_min=25`）恒真。起不来/找不到时该 case 记 `run_error=producer_start_failed`，不会静默变成「无流」；与 `--endpoint` / `--replay-tools` 同用会直接拒（那两个后端不起进程，没有可推流的目标）。

**注入有效性**：`rate` 在故障窗口判 `budget_drops > 0`，`drop` / `ingress_drop` 判 `injected > 0`；读数为 0 记「注入未生效」——它不改该 case 的 scored 与三级判定（故障没发生不是模型答错，因此不进成功率分母），但本次 run 的退出码不会是 0，报告里有独立小节。`backlog` / `delay` / `jitter` 没有单一计数判据，报告注明「未做自动判定」。

`suites/real-faults.json` 的五类故障都能被 `cued` 侧工具面诊断，每类的判据：

| 故障 | 注入方式 | agent 靠什么区分 |
|---|---|---|
| 上行丢包 `network_loss` | `ingress_drop` 50% | `injected` 在涨而 `dropped` 为 0、`frames_in == frames_out` |
| 带宽不足 `bandwidth_insufficient` | `rate` 0.1 Mbps | `budget_drops` 与 `dropped` 同时涨、`injected` 为 0 |
| 发送排队 `queue_backlog` | `backlog` 12 帧/s | `frames_in − frames_out` 持续增大、`dropped` 与 `budget_drops` 均为 0 |
| 编码太慢 `encoder_too_slow` | 1600×900 + `preset=placebo` | `encode_p50` > 33 ms、`op_p50` < 1 ms |
| 分辨率过高 `resolution_too_high` | 输出 3840×2160 + `veryfast` | `encode_p50` > 33 ms、`op_p50` > 1.5 ms（缩放开销随像素增长） |

五类统一 `fps_min=25`。**2026-10-25 按真码率产帧源（`PRODUCER=lavfi`，640×360@1200 kbps）重测**：故障态 netloss 10.99–17.98、backlog 8.00–18.40、encoder_too_slow 3.28–8.59、bandwidth_insufficient 0.00–0.98 fps，修复态 29.97–30.19 fps；`resolution_too_high` 三条（4K×veryfast/medium/slow）在 2 vCPU 上故障态余量不稳（veryfast 实测 10.24 fps，medium 24.00 fps < 阈值 25 但离阈值不足 3 fps），已登记为待校准项，不写进"已达标"。旧的 8.5–16.9 是 64×64 静态源下的数字，已作废（那套源连 `bw-starved` 的令牌桶都抽不干）。被淘汰的两类也记在 suite 的 `note` 里：`keyframe_storm` 在本机 x264 的 ABR+VBV 下没有可校准的 SLA 信号；`network_delay` / `clock_drift` 在 `cued` 侧没有可观测量。

**指纹**：`sha256(二进制 ⊕ git describe ⊕ 生效配置 ⊕ suite)`，再与 `case_id`、`seed` 拼接成每个 case 的指纹。生效配置包括 agent 模式、repeat、model、`cued` 路径与二进制哈希、单价、warmup、LLM 模式与 cassette 内容哈希、`--endpoint`、`--replay-tools` 目录哈希。改代码、改 suite、换 cassette 都会让缓存失效。

**`--diff` 的退出码**：`0` 可比且无回归；`1` 有回归；`3` 两边不可归因（指纹、suite、mode、repeat 任一不同）；`4` case 集变了（仅 A 有或仅 B 有）；`2` 读不到输入；`5` 写报告失败。3 和 4 是在提醒：这两份 run 的差异不能当成「修好了几个」。

## 可复现性的真实边界

cassette 只覆盖 LLM 侧，不覆盖工具观测。所以：

- 真管线里工具的观测每轮都在抖（fps 有 ±2% 级波动，1 秒窗口的相位不同读数就不同），只回放 LLM 不足以复现一次判定。完整回放要同时录制工具侧：`--record-tools DIR` 把每个 case 的观测写成 `<case_id>.tools.jsonl`，`--replay-tools DIR` 只读这份录制，不 spawn `cued`、不连 endpoint，miss 立即报错。
- 回放 3 次判定一致，证明的是**评测链路的确定性**：同一份观测 → 同一判定。它不证明「真实系统每次跑出同样数值」，后者本来就不成立（需要的是录制，不是期待）。
- `--endpoint` 接假域时，`--mode scripted` 的 agent 是 **oracle**：它按 case 的 `evidence_candidates` 从真实观测里逐字摘证据、按期望根因提交结论。它证明工具链能不改代码驱动假域（AC-16），不证明模型能力；要评模型请用 `--mode real`。
- `--mode scripted` 的普通自检路径（既不给 `--record-tools` 也不给 `--endpoint` / `--replay-tools`）用的是写死的观测（fps 恒 30.3），只用于「判分器 + 引擎」自检，报告里有横幅标注。

## 安全边界

工具按风险分三级：`ReadOnly`（`query_metrics`、`fetch_observation`）、`Mutating`（`pipeline_set_param`）、`Dangerous`（`inject_fault`、`run_shell`）。审批模式 `Ask` 逐条等审批回调：控制台有审批队列，`cue run` 没接回调，非只读动作会被直接拒绝；`Auto` 自动批准但全量留痕，`Deny` 拒绝一切非只读动作。评测里 `Dangerous` 还受 case 的 `allowed_actions` 白名单约束。

`run_shell` 是防御最深的一层：只允许 25 个只读诊断命令（`cat` / `ls` / `ps` / `ss` / `df` / `free` / `stat` / `grep` / `head` 等），按命令名精确匹配、不读 `PATH`、**不经 shell**（`execve` 直接启动，参数不拼接）；进入子进程后设 `RLIMIT_CPU=5s`、`RLIMIT_AS=256 MiB`、`RLIMIT_FSIZE=8 MiB`、`RLIMIT_NPROC=16`、`RLIMIT_NOFILE=64`，再用 landlock 放行 `/usr`、`/lib`、`/lib64`（可读可执行）、`/etc`、`/proc`、`/sys`（只读）、工作目录（只读）、`/dev/null`（可写），并且**只**给临时目录与本次 run 的目录（`runs/<run_id>/`）写权限。以 root 启动时，子进程在 `execve` 前 `setgid`/`setuid` 到 `nobody`（顺序是 rlimit → landlock → 降权：landlock 不需要特权，而降权之后就改不了规则集）；非 root 启动时结果与审计里显式记 `not_root(uid=…)`，不静默。要求可写 `/`、`/etc`、`~/.ssh`、项目根这类路径会被**直接拒绝执行**（不是静默忽略），建不出来的可写路径如实列进 `paths_not_applied`。每次调用——包括被白名单或审批拒绝的——都写一行审计 jsonl（`runs/audit.jsonl` 或 `runs/<run_id>/audit.jsonl`），带参数、退出码、耗时、实际后端、生效的可写路径、降权结果与截断标记。

可选 `docker` 后端（`sandbox_backend = "docker"`，默认仍是 `landlock`）：命令跑在 `docker run --rm --network=none --read-only --cap-drop=ALL --memory=128m` 里，宿主侧白名单默认**仍然生效**（`docker_relax_whitelist` 只为验证容器侧隔离而存在，不是给模型用的开关）；当前用户不在 docker 组时走 `sudo -n`，需要密码时立即失败并回显 sudo 的错误，而不是把父进程挂在密码提示上。

三条边界要说清楚：

- landlock **不限制 `stat(2)`**：子进程能取到路径的元数据（存在性、大小、时间），只是读不到内容。
- 默认后端防**误操作**，不防恶意代码：landlock 由被限制的进程自己在 `execve` 前施加，一个愿意自己再 exec 其它二进制的程序当然也能绕开白名单；要防恶意代码用 `docker` 后端。
- `docker` 后端在容器内仍是 uid=0（`--read-only` + `--cap-drop=ALL` + `--network=none` 兜住可写面与网络，但没有 `--user`）；宿主上的 docker CLI 不套 `RLIMIT_AS`/`RLIMIT_NPROC`（Go 运行时需要大段虚拟地址空间，套了它自己会 panic），容器里的资源边界由 `--memory=128m` 与父进程超时负责。
- `RLIMIT_FSIZE` 只对普通文件生效：写管道、字符设备（如 `/dev/null`）不受它约束，拦截写入依赖的是 landlock 而不是这个 rlimit。

landlock 不可用时会降级（例如内核不支持），降级原因通过子进程状态通道回传给父进程并写进审计，不静默；白名单与「不经 shell」这两道与 landlock 是否可用无关。

## 已知限制 / 未做

- **音频：链路已通，但没有重采样、也没有音频抖动缓冲**：上行 Opus（48 kHz 单声道 / 20 ms，裸 Opus 包）被 `cued` 用 libopus 真解出指标（`audio_frames_in` / `audio_decoded_frames` / `audio_pcm_frames` / `audio_rms_dbfs`），再**按字节原样透传**成下行 `DownType::Audio`——不重编码，所以 in-band FEC 冗余能原样到接收端（`audio_frames_out` 是真正发出的包数，被出向注入吃掉的不计、单独进 `audio_injected_drops`）。`cue-probe` 解码后播放：`--render` 且机器上有音频设备时出声（`--audio-out NAME` 选设备、`--list-audio` 列设备；设备打不开、或 48 kHz/单声道/s16 协商不一致，就只渲染视频）。听说质量看这一侧的 `audio_datagrams` / `audio_seq_gaps` / `audio_fec_recovered` / `audio_plc_frames` / `audio_unconcealed_frames` / `audio_queue_ms_p50` / `audio_starved`，没有音频设备时这些读数写 `null`（不是 0）；静音流仍是 `audio_decoded=true` + `audio_rms_dbfs=-120`（下限值，真值是 -inf）。
   两处刻意不做：**没有自适应重采样**——源与声卡约 0.5% 时钟漂移（队列约 6 ms/s 上涨），靠播放侧"每越过 250 ms 软上限就丢当前一个 20 ms 包"吸收，听感是每几秒漏一帧；**音频没有抖动缓冲、不做帧重排**——迟到 60 ms 内照播、更晚丢弃，与视频抖动缓冲的 1/2/4/8 帧档位不是一回事。音画对齐的方向是给**视频**补延迟（`--av-delay-ms -1` 补"音频队列深度 EWMA − 视频已有延迟 EWMA"），不是缩短音频缓冲。
- **AC-1 已真机验证，但只在一台机器上**：Windows 的集成摄像头（174f:2460）+ Realtek 麦克风阵列，经 Edge 的 `getUserMedia` + `MediaStreamTrackProcessor` 推流，cued 侧 `frames_in 594` / `audio_frames_in 1000` / `decode_errors 0`。仍缺的是**多样性**：没试过外接 USB 摄像头、虚拟摄像头（OBS/VoiceMeeter 那类）、以及 macOS 的 `avfoundation`。
- **多设备并发上限受采集端开销限制**：实测 2 路（640×360 + 1280×720 各 30 fps）时 `cued` 占 **25% CPU**、load 1.72、两路零丢帧；但每路 720p 合成源的采集+编码本身约 42%，所以这台 2 vCPU 机器上 2 路是舒适档，3 路以上要先算采集端的账。
  多设备时第 i 路（槽位 i）的下行发往 `--probe-port + i`，**每路要有一个 `cue-probe` 在听**；租约（`--lease-ms`，默认 15 s）靠"租约内有没有收到任何一条 WS 报文"判定，所以客户端崩溃但没发 FIN 的半开连接也能被回收（实测 `SIGSTOP` 冻结客户端 4 s 后得到 `device.lease_expired{basis:no_ws_message_within_lease}`）。
  能力协商：设备用 `type=0x05 Stats` 上报分辨率/帧率/codec 上限，服务端只在"配置值超过设备能力"时夹取（分辨率夹到偶数、码率按像素比例下调），并写 `basis=caps_clamped`；没有上报时与之前逐字节一致。
- **抖动缓冲的粒度受帧率约束**：档位是 1/2/4/8 帧，30 fps 下约 0/33/100/233 ms，做不了音频那种毫秒级目标延迟；也没有时间伸缩（视频本就不适用）。抖动缓冲与 NACK 之间只做了"重传在途时放宽空洞等待"这一处联动，不与 feedback 的拥塞信号联动。
- **抖动估计的基准已改为首片到达**，但"帧在途耗时污染抖动"这条在本机复现不出来（loopback 没有真实带宽整形，`sendmmsg` 一次推完整帧分片）。机制由单测证明，真实链路上的幅度未量化。
- **`src/eval/state.cpp` 用 `popen("git describe …")` 算指纹**：命令固定、无用户输入，暂未改成直接读仓库元数据。
- **TSan 需要 `vm.mmap_rnd_bits=28`**，否则 libtsan 会随机报 `unexpected memory mapping`（手动的 `sanitizer.yml` 里有这一步，`sudo -n` 不可用时只告警）。
- **sanitizer 不在常驻 CI 里**：见顶部说明。`ASan/UBSan/TSan 无报告` 这条由本地验证 + 手动 workflow 承担，不是每次 push 的证据。
- **推流页不发送 `codec_config`（0x03）**：`cued` 侧有接收与缓存实现，浏览器侧没发，`fetch_observation(frames)` 的媒体元信息段因此为空。
- **控制台前端产物（`web/dist`）不入库**：干净 clone 首次 `serve.sh` 需要联网装前端依赖；CI 里同样要现场 `npm ci`，无网时 e2e 走显式跳过（有 `===== 跳过原因 =====` 横幅与 `ci-skipped.txt`，AC-13 由独立的静态检查 job 兜底）。
- **推流页不依赖前端构建**：它是 `web/index.html` + `web/app.js` 的原生实现，`cue-serve` 现在同时托管两个根（控制台 `web/dist`、推流页 `web/`），`/stream` 走后者。此前 dist 存在时推流页会 404——干净 clone（无 dist）反而能打开，行为与直觉相反。
- **集成测试只在 `CUE_SANITIZE=none` 下注册**：它起真进程，而 sanitizer 构建的 `LD_PRELOAD` 会传给子进程，2 vCPU 上不稳定，且它测的不是内存安全；ASan/TSan 仍跑全部单测。
- **能力协商的端到端未测**：夹取逻辑（配置超过设备上限时夹分辨率/码率）有单测覆盖，但"真客户端发 `0x05` 上报能力 → 服务端按能力下发档位"这条链路没跑过——`cue-capture` 不发 `0x05`，只有浏览器发，而服务器无浏览器。实测两路设备时 `caps_declared` 都是 `false`，走的是"无上报按配置下发"的分支。
- **`cue-serve` 不转发 `--max-sessions` / `--lease-ms`**：这两个参数只能直接给 `cued`（控制台走 `ProcessPipelineControl` 的 spawn 参数，目前没暴露）。想让控制台配并发与租约，需要给 `ServeOptions` + `PipelineEndpoint` + spawn args 各加一处。
- **会话页的轨迹是 4 秒轮询，不是推送**：它读 `runs/*/sessions/*.jsonl`（完整审计事实），代价是刷新延迟而非实时；实时看事件流视图（SSE）。事件流有环形缓冲上限、慢消费者丢最旧、断线只补最后一段，所以两者不互相替代。
- **评测页的"对比"是逐 case 并列条形，不是时间序列曲线**：`report.json`/`state.jsonl` 只有逐 case 汇总，没有时间序列，所以界面写死"该 run 没有时间序列"、p99 全 `n/a` 时给空态而不是画 0 ms。另一个已知漂移点：汇总行来自 `report.md` 文本解析、分类来自 `state.jsonl`，两个源，报告被手改时会不一致（未做交叉校验）。
- **fuzz 只覆盖 `proto` 的三个解析入口**（上行头/分片校验、下行头、FEC 载荷头）：FEC 分组与 XOR 恢复、NACK 条目、feedback 编解码不在其中（已有单测，但没有 fuzz）。

## 目录

```
apps/          cue · cued · cue-probe · cue-capture · cue-eval · cue-serve · cue-mock-svc（+ console 渲染辅助）
include/cue/   proto · media · pipeline · agent · tool · llm · eval · common · sandbox
src/           与 include 分层对应的实现
web/           推流页（Canvas + WebCodecs + WS）；web/console 是 React 控制台
suites/        评测用例：m2-constraints.json · m4-diagnosis.json · real-faults.json · cassettes/
scripts/       run.sh · serve.sh · bench.sh · test-client.mjs · e2e-* · test-*.sh
cue.example.json  单一配置文件模板（FR-OBS.5）
tests/         单测（手写 check，靠 ctest 退出码）+ test_integration.py（真进程集成）+ fuzz/（libFuzzer）
bench/         benchmark 脚本的产出目录（数据不入库）
```

## 设计文档

设计期文档（需求、技术、讨论记录、调研报告）不随仓库分发，只保留在本地的 `docs/` 与 `research/`。

## 说明

- **延迟口径**：跨主机场景下的端到端延迟是相对量，依赖握手阶段的时钟偏移估计（目标误差 < 5 ms）。页面在连接时锚定时钟，每 30 s 重估，漂移 > 5 ms 重锚；重锚只写页面日志（推流页不接控制台的事件流）。数字不含最小网络传播时间，也不含上屏。
- **上行编码**：关键帧间隔由页面固定为 2 s，`suites` 里的 `pipeline.gop` 只影响服务端转码。
- **重连**：`cued` 重启后页面自动重连，并在重连成功后的第一帧强制 IDR；断线窗口里不消费这个标志，新解码器无需等 2 s GOP 即可恢复（实测 `dec_err=0`）。
