// M6：cassette 录制/回放
// - key = sha256(归一化请求)：同输入同 key；改 model/messages/tools/temperature/max_tokens 必变
// - tool_call_id 的随机后缀不进 key（映射成首次出现序号）
// - 用真实 HTTP（本进程内的假端点）走 OpenAiCompatibleClient 录制，再用 CassetteClient 回放：
//   同序消费、usage 照抄、miss 报错（绝不静默走网络）
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cue/llm/cassette.h"
#include "cue/llm/client.h"

namespace fs = std::filesystem;
namespace llm = cue::llm;

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

std::string make_response(const std::string& content, const std::string& call_id,
                          const std::string& call_name, const std::string& args, int prompt_tokens,
                          int completion_tokens) {
  nlohmann::json msg{{"role", "assistant"}, {"content", content}};
  const char* finish = "stop";
  if (!call_name.empty()) {
    msg["tool_calls"] = nlohmann::json::array(
        {nlohmann::json{{"id", call_id},
                        {"type", "function"},
                        {"function", {{"name", call_name}, {"arguments", args}}}}});
    finish = "tool_calls";
  }
  return nlohmann::json{{"choices", nlohmann::json::array({nlohmann::json{
                                               {"message", msg}, {"finish_reason", finish}}})},
                        {"usage",
                         {{"prompt_tokens", prompt_tokens}, {"completion_tokens", completion_tokens}}}}
      .dump();
}

// 极简 HTTP/1.1 假端点：按序返回预设 JSON。只为让「录制」走真实 libcurl 路径，
// 不测 HTTP 语义（重试/超时由 client 的单测覆盖）。
class FakeServer {
 public:
  explicit FakeServer(std::vector<std::string> responses) : responses_(std::move(responses)) {}
  ~FakeServer() { stop(); }
  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;

  bool start() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    (void)::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    port_ = ntohs(addr.sin_port);
    if (::listen(fd_, 4) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    thread_ = std::thread([this] { serve(); });
    return true;
  }

  void stop() {
    if (fd_ < 0) return;
    stop_.store(true);
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
    if (thread_.joinable()) thread_.join();
  }

  int port() const { return port_; }
  int served() const { return served_.load(); }

 private:
  static std::size_t content_length(const std::string& head) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::size_t pos = lower.find("\r\ncontent-length:");
    if (pos == std::string::npos) return 0;
    std::size_t i = pos + std::strlen("\r\ncontent-length:");
    while (i < lower.size() && lower[i] == ' ') ++i;
    std::size_t n = 0;
    while (i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i]))) {
      n = n * 10 + static_cast<std::size_t>(lower[i] - '0');
      ++i;
    }
    return n;
  }

  static bool read_more(int fd, std::string& buf) {
    char tmp[4096];
    const ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n <= 0) return false;
    buf.append(tmp, static_cast<std::size_t>(n));
    return true;
  }

  void serve() {
    while (!stop_.load()) {
      const int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) {
        if (stop_.load()) break;
        continue;
      }
      std::string buf;
      while (!stop_.load() && buf.find("\r\n\r\n") == std::string::npos) {
        if (!read_more(c, buf) || buf.size() > (1u << 20)) break;
      }
      const std::size_t hdr_end = buf.find("\r\n\r\n");
      if (hdr_end != std::string::npos) {
        const std::size_t want = hdr_end + 4 + content_length(buf.substr(0, hdr_end + 2));
        while (!stop_.load() && buf.size() < want) {
          if (!read_more(c, buf)) break;
        }
      }

      const std::size_t idx = static_cast<std::size_t>(served_.load());
      std::string body;
      if (idx < responses_.size()) {
        body = responses_[idx];
        served_.fetch_add(1);
      } else {
        body = R"({"error":{"message":"fake server exhausted"}})";
      }
      const std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body;
      std::size_t off = 0;
      while (off < out.size()) {
        const ssize_t n = ::write(c, out.data() + off, out.size() - off);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
      }
      ::close(c);
    }
  }

  std::vector<std::string> responses_;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> served_{0};
  std::thread thread_;
};

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

llm::ChatRequest base_request() {
  llm::ChatRequest req;
  req.model = "m";
  llm::ChatMessage sys;
  sys.role = "system";
  sys.content = "你是运维 agent";
  llm::ChatMessage user;
  user.role = "user";
  user.content = "把 p99 压到 120ms";
  req.messages = {sys, user};
  req.tools = nlohmann::json::array(
      {nlohmann::json{{"type", "function"},
                      {"function", {{"name", "query_metrics"}, {"description", "查指标"}}}}});
  req.temperature = 0.0;
  req.max_tokens = 2048;
  return req;
}

llm::ChatMessage assistant_from(const llm::ChatResponse& r) {
  llm::ChatMessage m;
  m.role = "assistant";
  m.content = r.content;
  m.tool_calls = r.tool_calls;
  return m;
}

}  // namespace

int main() {
  // 1) key 稳定性：同输入同 key；任一语义字段变化必变
  {
    const llm::ChatRequest req = base_request();
    const std::string k = llm::cassette_key(req);
    check(k.size() == 64, "key 是 64 位 sha256");
    check(k == llm::cassette_key(req), "同输入 → 同 key");

    auto changed = [&](auto mutate, const char* what) {
      llm::ChatRequest r = req;
      mutate(r);
      check(llm::cassette_key(r) != k, what);
    };
    changed([](llm::ChatRequest& r) { r.model = "m2"; }, "改 model → key 变");
    changed([](llm::ChatRequest& r) { r.messages[1].content += "!"; }, "改 messages → key 变");
    changed([](llm::ChatRequest& r) { r.tools[0]["function"]["name"] = "other"; }, "改 tools → key 变");
    changed([](llm::ChatRequest& r) { r.temperature = 0.5; }, "改 temperature → key 变");
    changed([](llm::ChatRequest& r) { r.max_tokens = 4096; }, "改 max_tokens → key 变");

    // 首次出现序号：随机 tool_call_id 后缀不进 key
    llm::ChatMessage asst;
    asst.role = "assistant";
    llm::ToolCall tc;
    tc.id = "call_abc";
    tc.name = "query_metrics";
    tc.arguments = R"({"window_ms":5000})";
    asst.tool_calls.push_back(tc);
    llm::ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.tool_call_id = "call_abc";
    tool_msg.content = "metrics={}";
    llm::ChatRequest a = req;
    a.messages.push_back(asst);
    a.messages.push_back(tool_msg);
    llm::ChatRequest b = a;
    b.messages[2].tool_calls[0].id = "call_xyz_random";
    b.messages[3].tool_call_id = "call_xyz_random";
    check(llm::cassette_key(a) == llm::cassette_key(b), "tool_call_id 随机后缀不影响 key");
    const std::string canon = llm::cassette_request_json(a).dump();
    check(canon.find("call_abc") == std::string::npos, "归一化请求不含原始 tool_call_id");
    check(canon.find("tcid-0") != std::string::npos, "归一化请求用 tcid 序号");

    // AC-8：cancel 是运行期控制信号（一个地址），不是请求内容。只有它不同的两个请求
    // 必须算出同一个 key，否则「取消过一次」的进程回放永远 miss。
    std::atomic<bool> cancel_off{false};
    std::atomic<bool> cancel_on{true};
    llm::ChatRequest c_off = req;
    c_off.cancel = &cancel_off;
    llm::ChatRequest c_on = req;
    c_on.cancel = &cancel_on;
    check(llm::cassette_key(c_off) == k, "带 cancel 指针的请求与不带时同 key");
    check(llm::cassette_key(c_off) == llm::cassette_key(c_on), "cancel 未置位/已置位不影响 key");
  }

  // 2) 录制（真实 HTTP）→ 回放（同序、usage 照抄、miss 报错）
  const fs::path tmp = fs::temp_directory_path() / ("cue-cassette-test-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);

  FakeServer server({make_response("", "call_abc", "query_metrics", R"({"window_ms":5000})", 111, 22),
                     make_response("done", "", "", "", 333, 44),
                     make_response("repeat-2", "", "", "", 555, 66)});
  const bool started = server.start();
  check(started, "假 HTTP 端点启动");

  if (started) {
    const std::string cassette_dir = (tmp / "cassettes").string();
    llm::OpenAiCompatibleClient::Config cc;
    cc.base_url = "http://127.0.0.1:" + std::to_string(server.port());
    cc.api_key = "test-key";
    cc.model = "m";
    cc.timeout_s = 5;
    cc.max_retries = 0;
    cc.record_dir = cassette_dir;
    llm::OpenAiCompatibleClient live(cc);

    const llm::ChatRequest req1 = base_request();
    const auto r1 = live.chat(req1);
    check(r1.has_value(), "live 调用 1 成功");
    if (r1) {
      check(r1->tool_calls.size() == 1, "录制响应含 tool_call");
      check(r1->usage.prompt_tokens == 111 && r1->usage.completion_tokens == 22, "live usage 1");
    }

    llm::ChatRequest req2 = req1;
    if (r1) {
      req2.messages.push_back(assistant_from(*r1));
      llm::ChatMessage tool_msg;
      tool_msg.role = "tool";
      tool_msg.tool_call_id = r1->tool_calls.empty() ? "" : r1->tool_calls[0].id;
      tool_msg.content = "metrics={}";
      req2.messages.push_back(tool_msg);
    }
    const auto r2 = live.chat(req2);
    check(r2.has_value() && r2->content == "done", "live 调用 2 成功");
    // 同一 key 再调用一次：录制文件里应有两行，回放按行序消费
    const auto r3 = live.chat(req1);
    check(r3.has_value() && r3->content == "repeat-2", "live 调用 3（同 key）成功");
    check(server.served() == 3, "假端点恰好被调用 3 次");

    // 文件落盘：两次调用 = 2 个 key 文件，各 1 行，版本/字段齐全
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(cassette_dir, ec)) {
      if (e.path().extension() == ".jsonl") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    check(files.size() == 2, "录制产生 2 个 cassette 文件");
    {
      std::ifstream in((fs::path(cassette_dir) / (llm::cassette_key(req1) + ".jsonl")).string());
      std::size_t lines = 0;
      std::string line;
      while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
      }
      check(lines == 2, "同一 key 的两次调用写成两行（按行序消费）");
    }
    bool format_ok = !files.empty();
    for (const auto& f : files) {
      std::ifstream in(f, std::ios::binary);
      std::string line;
      std::size_t lines = 0;
      while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++lines;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
          format_ok = false;
          continue;
        }
        const std::string stem = fs::path(f).stem().string();
        if (j.value("v", 0) != 1 || j.value("key", std::string()) != stem || !j.contains("request") ||
            !j["request"].is_object() || !j.contains("response") || !j.contains("usage") ||
            !j.contains("ts") || !j.contains("model")) {
          format_ok = false;
        }
      }
      if (lines == 0) format_ok = false;
    }
    check(format_ok, "cassette 每行含 v:1/key/ts/model/request/response/usage");

    // 回放：同序消费、usage 照抄、tool_call id 原样返回
    llm::CassetteClient::Config rc;
    rc.dir = cassette_dir;
    rc.default_model = "m";
    llm::CassetteClient replay(rc);
    const auto p1 = replay.chat(req1);
    check(p1.has_value(), "回放命中第 1 条");
    if (p1) {
      check(p1->tool_calls.size() == 1 && p1->tool_calls[0].id == "call_abc", "回放保留 tool_call id");
      check(p1->usage.prompt_tokens == 111 && p1->usage.completion_tokens == 22, "回放 usage 照抄 1");
    }
    const auto p2 = replay.chat(req2);
    check(p2.has_value() && p2->content == "done", "回放命中第 2 条");
    if (p2) check(p2->usage.prompt_tokens == 333 && p2->usage.completion_tokens == 44, "回放 usage 照抄 2");
    const auto p3 = replay.chat(req1);
    check(p3.has_value() && p3->content == "repeat-2", "同 key 第二次调用取第 2 行");
    if (p3) check(p3->usage.prompt_tokens == 555 && p3->usage.completion_tokens == 66, "回放 usage 照抄 3");
    const auto p4 = replay.chat(req1);
    const std::string expected_miss = "cassette miss: " + llm::cassette_key(req1);
    check(!p4.has_value(), "同 key 录制耗尽后再次调用 miss");
    if (!p4) {
      check(p4.error().rfind(expected_miss, 0) == 0, "miss 错误以 cassette miss: <key> 开头");
    }
    const auto st = replay.stats();
    check(st.hits == 3 && st.misses == 1, "replay 统计 hits=3 misses=1");

    // 空目录：直接 miss，不碰网络（假端点计数不变）
    llm::CassetteClient::Config empty_cfg;
    empty_cfg.dir = (tmp / "empty").string();
    empty_cfg.default_model = "m";
    llm::CassetteClient empty(empty_cfg);
    const auto miss = empty.chat(req1);
    check(!miss.has_value(), "无 cassette 目录 → miss");
    check(server.served() == 3, "replay miss 没有回退到网络");

    // 版本字段必须校验：v=2 的未来格式不能被当前代码按 v=1 猜读
    const std::string bad_dir = (tmp / "bad").string();
    fs::create_directories(bad_dir, ec);
    const std::string req1_file = (fs::path(cassette_dir) / (llm::cassette_key(req1) + ".jsonl")).string();
    if (!slurp(req1_file).empty()) {
      std::string text = slurp(req1_file);
      const std::size_t pos = text.find("\"v\":1");
      if (pos != std::string::npos) text.replace(pos, 5, "\"v\":2");
      write_text((fs::path(bad_dir) / fs::path(req1_file).filename()).string(), text);
      llm::CassetteClient::Config bad_cfg;
      bad_cfg.dir = bad_dir;
      bad_cfg.default_model = "m";
      llm::CassetteClient bad(bad_cfg);
      const auto bad_resp = bad.chat(req1);
      check(!bad_resp.has_value(), "未知 cassette 版本 → 报错");
      if (!bad_resp) {
        check(bad_resp.error().find("cassette version") != std::string::npos, "错误说明版本不支持");
      }
    }
  }

  server.stop();
  fs::remove_all(tmp, ec);

  if (failures == 0) {
    std::printf("cassette: all checks passed（key 稳定 / 录制 / 回放 / miss / 版本）\n");
    return 0;
  }
  std::printf("cassette: %d failure(s)\n", failures);
  return 1;
}
