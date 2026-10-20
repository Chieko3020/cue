# cassette 夹具

本目录的 `.jsonl` 是 **假端点产的合成录制**，用于在没有 API key / 网络时验证回放链路：

```bash
./build/cue demo --replay suites/cassettes/demo
```

- 每行格式：`{"v":1,"key":…,"ts":…,"model":…,"request":{…},"response":{…},"usage":{…}}`
- `key = sha256(model + messages + tools + temperature + max_tokens)`，
  `tool_call_id` 归一化为首次出现序号；**时间戳不进 key**。
- 重新生成（会按当前 system prompt / 工具 schema 重算 key，旧夹具随之失效）：

  ```bash
  python3 scripts/fake-llm.py --port 18765 --mode demo &
  ./build/cue demo --http http://127.0.0.1:18765 --record /tmp/cassette-demo
  rm -f suites/cassettes/demo/*.jsonl
  cp /tmp/cassette-demo/*.jsonl suites/cassettes/demo/
  ```

这些响应不是模型能力证据：它们只证明「录制 → 回放 → usage 照抄 → miss 报错」链路。
真实评测请用 `--live --record` 录自己的 cassette，并不要把含真实调用/成本的目录提交进仓库
（`/cassettes/` 已在 `.gitignore` 里）。
