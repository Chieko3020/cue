# Cue

Cue 是 C++23 的 Agent Harness：驱动一条真实的低延迟视频管线，并用可复现的自动评测回答「agent 到底有没有把系统变好」。之所以这么拼，是因为链路排障的规律（看哪一段的哪个计数在动）本来就可以交给 agent，但业界没有 C++ 的 agent runtime，也没有框架能回答「改完之后是变好还是变坏」。

- **Harness** — 阶段状态机约束下的 ReAct 内核：领域工具、三级审批、沙箱、上下文预算、cassette
- **被测域** — 浏览器 Canvas 合成源 → WebCodecs 编码 → WebSocket 上行 → 解码 → 可插拔转码 → 自研 UDP 下行；`cued` 是独立进程，故障注入在用户态、固定种子
- **评测** — 声明式 case、三重判分（根因 / 证据 / SLA）、pass^k、指纹、回归对比、人读报告
- **可观测** — `cue-serve` + React 控制台五页、SSE 事件流、审批 UI；`cue-probe` 跨机接收端

当前状态：M0–M7 已落地。`ctest` 9 项全部通过；控制台五页、SSE 与审批 UI 可用；ASan/UBSan/TSan 构建选项、cassette、benchmark、CI workflow 都在仓库里。

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

边界：harness 层的头文件不得 include `media/`、`pipeline/`（CMake 在配置期扫描，违者直接 configure 失败）；工具层只认识 `IServicePipeline` 接口，不知道背后是子进程还是 HTTP 服务。

### 被测域（`src/media`、`src/pipeline`）

`cued` 负责 WS 接收、分片重组、解码、`ProcessorChain`（passthrough / scale / transcode）、编码、UDP 发送，六段埋点（recv / buffer / decode / op / encode / send）逐帧计时。令牌桶、XOR-FEC、NACK 与四种损伤注入（`drop` / `ingress_drop` / `rate` / `backlog`）都在发送路径上，结果可复现。

`pipeline/` 是适配层：`ProcessPipelineControl` 把 `cued` 当子进程拉起，从 stdout 解析指标与事件；`HttpServicePipeline` 连接实现 `GET /api/state` + `POST /api/action` 的外部服务（`cue-mock-svc` 是其中一个确定性假域）。

边界：帧数据只在浏览器 ↔ `cued` ↔ `cue-probe` 之间流动，不跨进程进 harness；工具面目前只看得到 `cued` 侧指标，`cue-probe` 的端到端分位还没回传（见「已知限制」）。

### 评测（`src/eval`）

声明式 case：注入参数、prompt、期望根因、允许动作、SLA、预算。引擎按 case 起管线、注入故障、跑 agent、在 agent 结束后重新测量、按三重口径判分，产出 `runs/<run_id>/report.md` 与 `state.jsonl`（断点续跑用）。同一 run 有文件锁，并发 resume 不会双份计费。

边界：`scripted` 模式是引擎自检，不判模型；`real` 模式默认回放 cassette，缺录制直接记 `run_error`，不会静默联网。

### 可观测（`src/eval` 的事件、`apps/cue-serve.cpp`、`web/console`）

`cue-serve` 提供 `GET /events`（SSE，`id` 单调递增，环形缓冲 512 条，支持 `Last-Event-ID` 补齐；更旧的事件靠 `/api/state` 全量快照恢复）、`GET /api/state` 快照、`POST /api/action` 控制动作、`POST /api/approve` 审批决策，并托管 `web/dist`。控制台五页是设备、管线、会话、审批、评测；审批页展示待审批队列与倒计时，动作必须走 `Registry::call_guarded`，不直接碰管线。

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
| LLM 接入 | libcurl（OpenAI 兼容接口） |
| 沙箱 | landlock + rlimit |

## 快速开始

以下命令都在仓库根目录执行。依赖（Ubuntu 24.04）：

```bash
sudo apt install -y g++-14 cmake pkg-config \
  libavcodec-dev libavutil-dev libswscale-dev libx264-dev \
  libcurl4-openssl-dev libboost-dev libboost-system-dev nlohmann-json3-dev
```

`libboost-system-dev` 不能省：`find_package(Boost COMPONENTS system)` 在只有头文件时会失败。

构建（必须显式指定 `g++-14`：CMake 拒绝 GCC < 14，而 Ubuntu 24.04 默认 `g++` 是 13）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j1
```

`-j1` 是这台机器的硬约束：2 vCPU / 1.9 GiB 上 `-j2` 同时编两个 libavcodec 相关 TU 会 OOM。默认 `CUE_WERROR=ON`，本仓库全量构建 0 warning；只看警告不愿被中断时重新 configure 加 `-DCUE_WERROR=OFF`。全量首次构建在 2 vCPU 上约 7 分钟。

单测（9 项，约 1 秒）：

```bash
ctest --test-dir build --output-on-failure
```

跑一次脚本化 agent 演示（不联网、不花钱、不启动管线）：

```bash
./scripts/run.sh demo
```

它只构建 `cue` 一个 target，把完整诊断过程与结果写到 `runs/demo-<时间>.txt`，并在最后打印路径。干净 clone 首次运行（含 configure 与构建）实测 131 秒；只跑这一条时不需要先手动构建，脚本会自己 configure 并只编译 `cue`。

## 运行入口

| 入口 | 作用 | 是否联网 / 启动管线 |
|---|---|---|
| `./scripts/run.sh demo` / `cue demo` | 脚本化 LLM 走完状态机，演示工具调用、阶段迁移、成本记账 | 都不 |
| `cue run` | 真 LLM（`CUE_API_KEY`、`CUE_BASE_URL`、`CUE_MODEL`）+ 真 `cued` 管线，启动前注入 5% 丢包 | 联网，起管线 |
| `cue-eval` | 评测 CLI：suite、模式、repeat、成本闸门、断点续跑、回归对比 | 取决于 `--live` / 后端 |
| `cued` | 媒体管线进程（WS 进、UDP 出） | 起管线 |
| `cue-probe` | 下行接收端：重组、NACK、FEC、帧率与端到端分位 | 起管线 |
| `cue-serve` | 控制台后端：HTTP + SSE + 静态托管 + 审批队列 | 起控制台 |
| `cue-mock-svc` | 确定性假域服务，实现同一套 REST 控制协议 | 起假域 |

`cue demo` 的变体：`--http URL [--record DIR]` 走 OpenAI 兼容端点并录制 cassette；`--replay DIR` 从 cassette 回放。`cue run` 的 `--approval ask|auto|deny`、`--allow-dangerous NAMES|none` 控制审批；`CUE_RECORD=1` 把成功调用录进 `CUE_CASSETTE_DIR`。

`run.sh` 不带参数（或 `demo-min`）会起「`cue-probe` + `cued` + 静态页」，在终端打印 `http://127.0.0.1:8080/?ws_port=9000`，打开页面点「开始推流」。`cued` 默认只监听 `127.0.0.1`；跨机演示用 `BIND=0.0.0.0` 并自行限制 9000/TCP 与 9001/UDP。改过 `WS_PORT` 也能对上。

控制台（可选）：`./scripts/serve.sh` 会先构建 `web/console`（首次需要 Node 与 npm，会联网装前端依赖），再起 `cue-serve`，默认 `http://127.0.0.1:8080/`、审批模式 `ask`。

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

`suites/real-faults.json` 的五类故障都能被 `cued` 侧工具面诊断，每类的判据：

| 故障 | 注入方式 | agent 靠什么区分 |
|---|---|---|
| 上行丢包 `network_loss` | `ingress_drop` 50% | `injected` 在涨而 `dropped` 为 0、`frames_in == frames_out` |
| 带宽不足 `bandwidth_insufficient` | `rate` 0.1 Mbps | `budget_drops` 与 `dropped` 同时涨、`injected` 为 0 |
| 发送排队 `queue_backlog` | `backlog` 12 帧/s | `frames_in − frames_out` 持续增大、`dropped` 与 `budget_drops` 均为 0 |
| 编码太慢 `encoder_too_slow` | 1600×900 + `preset=placebo` | `encode_p50` > 33 ms、`op_p50` < 1 ms |
| 分辨率过高 `resolution_too_high` | 输出 3840×2160 + `veryfast` | `encode_p50` > 33 ms、`op_p50` > 1.5 ms（缩放开销随像素增长） |

五类统一 `fps_min=25`；故障态实测 8.5–16.9 fps，修复态 28.5–28.7 fps，两侧各留 ≥3 fps。被淘汰的两类也记在 suite 的 `note` 里：`keyframe_storm` 在本机 x264 的 ABR+VBV 下没有可校准的 SLA 信号；`network_delay` / `clock_drift` 在 `cued` 侧没有可观测量。

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

`run_shell` 是防御最深的一层：只允许 25 个只读诊断命令（`cat` / `ls` / `ps` / `ss` / `df` / `free` / `stat` / `grep` / `head` 等），按命令名精确匹配、不读 `PATH`、**不经 shell**（`execv` 直接启动，参数不拼接）；进入子进程后设 `RLIMIT_CPU=5s`、`RLIMIT_AS=256 MiB`、`RLIMIT_FSIZE=0`、`RLIMIT_NPROC=16`、`RLIMIT_NOFILE=64`，再用 landlock 只放行 `/usr`、`/lib`、`/lib64`（可读可执行）、`/etc`、`/proc`、`/sys`（只读）、工作目录（只读）与 `/dev/null`（可写）。每次调用——包括被白名单或审批拒绝的——都写一行审计 jsonl（`runs/audit.jsonl` 或 `runs/<run_id>/audit.jsonl`），带参数、退出码、耗时与截断标记。

三条边界要说清楚：

- landlock **不限制 `stat(2)`**：子进程能取到路径的元数据（存在性、大小、时间），只是读不到内容。
- 工作目录是**只读放行**：为了让 `ls`、`du`、`stat` 这类命令可用，读被放宽；写仍然全禁（不在放行清单里）。
- `RLIMIT_FSIZE=0` 只对普通文件生效：写管道、字符设备（如 `/dev/null`）不受它约束，拦截写入依赖的是 landlock 而不是这个 rlimit。

landlock 不可用时会降级（例如内核不支持），降级原因通过子进程状态通道回传给父进程并写进审计，不静默；白名单与「不经 shell」这两道与 landlock 是否可用无关。

## 已知限制 / 未做

- 音频链路未实现（Opus 只出现在协议头的 codec 枚举里）。
- `cued` 是单连接同步服务：同一时刻只服务一个推流端。
- 控制面是「子进程 + stdout 解析」而不是 UDS：SPEC 原计划的 `SOCK_SEQPACKET` 控制通道没有做。
- `inject_fault` 与 `pipeline_set_param` 的生效方式是重启 `cued`，每次会造成数百 ms 断流。
- `cue-probe` 的指标还没回传到工具面：端到端分位只在 probe 侧打印，所以 `network_delay` / `clock_drift` 这两类根因 agent 诊断不了。
- `src/eval/state.cpp` 用 `popen("git describe …")` 算指纹：命令固定、无用户输入，暂未改成直接读仓库元数据。
- TSan 需要 `vm.mmap_rnd_bits=28`，否则 libtsan 会随机报 `unexpected memory mapping`（CI 的 tsan job 里有这一步）。
- 推流页不发送 `codec_config`（`0x03`）：`cued` 侧有接收与缓存实现，浏览器侧没发，`fetch_observation(frames)` 的媒体元信息段因此为空。
- 控制台前端产物（`web/dist`）不入库：干净 clone 首次 `serve.sh` 需要联网装前端依赖。
- CI 的 4 个 job（build+ctest、ASan+UBSan、TSan、可选 e2e）的 `run:` 命令已在本机逐条跑通，但尚未在真实的 Forgejo workflow 里跑过（self-hosted runner 已就位）。

## 目录

```
apps/          cue · cued · cue-probe · cue-eval · cue-serve · cue-mock-svc（+ console 渲染辅助）
include/cue/   proto · media · pipeline · agent · tool · llm · eval · common · sandbox
src/           与 include 分层对应的实现
web/           推流页（Canvas + WebCodecs + WS）；web/console 是 React 控制台
suites/        评测用例：m2-constraints.json · m4-diagnosis.json · real-faults.json · cassettes/
scripts/       run.sh · serve.sh · bench.sh · test-client.mjs · e2e-* · test-*.sh
tests/         无框架单测（手写 check，靠 ctest 退出码）
bench/         benchmark 脚本的产出目录（数据不入库）
```

## 设计文档

设计期文档（需求、技术、讨论记录、调研报告）不随仓库分发，只保留在本地的 `docs/` 与 `research/`。

## 说明

- **延迟口径**：跨主机场景下的端到端延迟是相对量，依赖握手阶段的时钟偏移估计（目标误差 < 5 ms）。页面在连接时锚定时钟，每 30 s 重估，漂移 > 5 ms 重锚；重锚只写页面日志（推流页不接控制台的事件流）。数字不含最小网络传播时间，也不含上屏。
- **上行编码**：关键帧间隔由页面固定为 2 s，`suites` 里的 `pipeline.gop` 只影响服务端转码。
- **重连**：`cued` 重启后页面自动重连，并在重连成功后的第一帧强制 IDR；断线窗口里不消费这个标志，新解码器无需等 2 s GOP 即可恢复（实测 `dec_err=0`）。
