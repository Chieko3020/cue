// Tests use the actual app-private producer wrapper, with synthetic child processes only.
#include <atomic>
#include <array>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "capture_child_process.h"
#include "capture_pipe_budget.h"
#if defined(_WIN32)
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif
using cue::capture::ChildProcess;
using cue::capture::IoStatus;
namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
void output(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}
std::vector<std::string> arguments(int argc, char** argv) {
#if defined(_WIN32)
  (void)argc; (void)argv;
  int count = 0;
  LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  std::vector<std::string> out;
  for (int i = 0; i < count; ++i) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), n, nullptr, nullptr);
    text.pop_back(); out.push_back(std::move(text));
  }
  ::LocalFree(wide);
  return out;
#else
  return {argv, argv + argc};
#endif
}
}
int main(int argc, char** argv) {
  const auto args = arguments(argc, argv);
  if (args.size() > 2 && args[1] == "--helper") {
#if defined(_WIN32)
    // Model ffmpeg binary stdout, not CRT newline translation in this test helper.
    ::_setmode(::_fileno(stdout), _O_BINARY);
#endif
    if (args[2] == "quiet") { std::this_thread::sleep_for(std::chrono::seconds(20)); return 0; }
    if (args[2] == "delayed") {
      output("ab"); std::this_thread::sleep_for(std::chrono::milliseconds(250)); output("cdef"); return 0;
    }
    if (args[2] == "eof") { output("done"); return 0; }
    if (args[2] == "bulk") {
      std::array<uint8_t, 65536> block{};
      block.fill(0xA5);
      for (int i = 0; i < 1280; ++i) {
        if (std::fwrite(block.data(), 1, block.size(), stdout) != block.size()) return 3;
      }
      std::fflush(stdout);
      return 0;
    }
    if (args[2] == "args") {
      for (std::size_t i = 3; i < args.size(); ++i) output(std::to_string(args[i].size())+":"+args[i]+"\n");
      return 0;
    }
#if defined(_WIN32)
    if (args[2] == "closed") {
      ::CloseHandle(::GetStdHandle(STD_OUTPUT_HANDLE));
      std::this_thread::sleep_for(std::chrono::seconds(20)); return 0;
    }
    if (args[2] == "tree") {
      std::wstring self = cue::capture::utf8_to_wide(args[0]);
      std::wstring command = L"\"" + self + L"\" --helper quiet";
      STARTUPINFOW startup{}; startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
      startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
      startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
      PROCESS_INFORMATION info{};
      if (!::CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return 3;
      output(std::to_string(info.dwProcessId)+"\n");
      ::CloseHandle(info.hThread); ::CloseHandle(info.hProcess);
      return 0; // descendant retains stdout, and must still be owned by capture's Job.
    }
#endif
    return 2;
  }
  if (args.empty()) return 2;
  const std::string self = args[0];
  using cue::capture::video_pipe_bytes;
  using cue::media::HostPlatform;
  using cue::media::VideoEncodeMode;
  check(video_pipe_bytes(HostPlatform::Windows,VideoEncodeMode::InProcess,640,360)==345600,
        "small raw-video request is no larger than one YUV420 frame");
  check(video_pipe_bytes(HostPlatform::Windows,VideoEncodeMode::InProcess,1920,1080)==3110400,
        "1080p raw-video request can hold one complete YUV420 frame");
  check(video_pipe_bytes(HostPlatform::Windows,VideoEncodeMode::InProcess,3840,2160)==4194304,
        "larger raw-video requests are capped at four MiB");
  check(video_pipe_bytes(HostPlatform::Linux,VideoEncodeMode::InProcess,1920,1080)==4096 &&
        video_pipe_bytes(HostPlatform::macOS,VideoEncodeMode::InProcess,1920,1080)==4096 &&
        video_pipe_bytes(HostPlatform::Windows,VideoEncodeMode::FfmpegChild,1920,1080)==4096,
        "other platforms and encoded child video retain their pipe request");
  ChildProcess child;
  std::string error;
  uint8_t data[256]{}; std::size_t got = 0;
  check(!child.start({}, error), "empty argv fails");
  check(child.start({self, "--helper", "quiet"}, error), "quiet source starts");
  const auto t0 = std::chrono::steady_clock::now();
  check(child.read_some(data, sizeof(data), got, 80) == IoStatus::Timeout && got == 0, "quiet timeout");
  check(std::chrono::steady_clock::now()-t0 < std::chrono::seconds(1), "read timeout bounded");
  check(child.running(), "quiet source running");
  auto stop_start = std::chrono::steady_clock::now(); child.stop(); child.stop();
  check(std::chrono::steady_clock::now()-stop_start < std::chrono::seconds(5), "stop bounded/idempotent");
  check(!child.running(), "stopped state");

  check(child.start({self, "--helper", "quiet"}, error), "reader cancellation source starts");
  std::atomic<bool> cancel{false}, entered{false};
  IoStatus last_read = IoStatus::Error;
  std::thread reader([&] {
    uint8_t byte = 0;
    std::size_t count = 0;
    entered.store(true, std::memory_order_release);
    // Enter one bounded read even if cancellation wins the first scheduling slice.
    do {
      last_read = child.read_some(&byte, 1, count, 100);
      if (last_read != IoStatus::Timeout) break;
    } while (!cancel.load(std::memory_order_acquire));
  });
  while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  cancel.store(true, std::memory_order_release);
  stop_start = std::chrono::steady_clock::now();
  child.stop_after_reader(reader);
  check(last_read == IoStatus::Timeout && !reader.joinable() && !child.running(),
        "cancel joins the active bounded reader before closing source handles");
  check(std::chrono::steady_clock::now() - stop_start < std::chrono::seconds(5),
        "reader cancellation and source stop are bounded");
  check(child.start({self,"--helper","delayed"},error), "delayed source starts");
  std::size_t filled = 0; bool timed_out_after_prefix = false;
  while (filled < 6) {
    const auto status = child.read_exact(data+filled, 6-filled, got, 80);
    filled += got;
    if (status == IoStatus::Timeout) { if (filled > 0) timed_out_after_prefix = true; continue; }
    if (status != IoStatus::Ok) break;
  }
  check(filled == 6 && std::memcmp(data,"abcdef",6)==0 && timed_out_after_prefix,
        "partial raw frame survives timeout without shifted boundary");
  child.stop();
  check(child.start({self,"--helper","eof"},error), "EOF source starts");
  check(child.read_exact(data,4,got,1000)==IoStatus::Ok && got==4, "EOF source payload");
  check(child.read_some(data,sizeof(data),got,1000)==IoStatus::Eof, "pipe EOF distinct from timeout");
  child.stop();
  const std::vector<std::string> values = {"space argument", "quote\"inside", "backslash\\", "", "测试 path\\"};
  std::vector<std::string> cmd{self,"--helper","args"}; cmd.insert(cmd.end(),values.begin(),values.end());
  check(child.start(cmd,error), "argv source starts");
  std::string actual;
  for (;;) {
    const auto status=child.read_some(data,sizeof(data),got,1000);
    if(status==IoStatus::Eof)break;
    if(status!=IoStatus::Ok){check(false,"argv read");break;}
    actual.append(reinterpret_cast<char*>(data),got);
  }
  std::string expected; for(const auto& value:values)expected+=std::to_string(value.size())+":"+value+"\n";
  check(actual==expected,"space quote empty Unicode argv preserved");
  child.stop();
#if defined(_WIN32)
  // Exercise an executable path, not just Unicode argument values.
  const auto copied_dir = std::filesystem::temp_directory_path() /
      (L"cue capture 测试-" + std::to_wstring(::GetCurrentProcessId()));
  const auto copied_exe = copied_dir / L"helper 测试.exe";
  std::filesystem::create_directory(copied_dir);
  std::filesystem::copy_file(std::filesystem::path(cue::capture::utf8_to_wide(self)), copied_exe);
  const auto copied_u8 = copied_exe.u8string();
  const std::string copied_name(reinterpret_cast<const char*>(copied_u8.data()), copied_u8.size());
  check(child.start({copied_name, "--helper", "eof"}, error), "space/Unicode executable path starts");
  check(child.read_exact(data, 4, got, 1000) == IoStatus::Ok && got == 4 &&
            std::memcmp(data, "done", 4) == 0, "space/Unicode executable output");
  child.stop();
  std::filesystem::remove(copied_exe);
  std::filesystem::remove(copied_dir);
  // Bare executable names must retain PATH lookup after switching away from _popen.
  const auto slash=self.find_last_of("/\\");
  const std::string dir=self.substr(0,slash), name=self.substr(slash+1);
  const DWORD env_size=::GetEnvironmentVariableW(L"PATH",nullptr,0);
  std::wstring old_path(env_size,L'\0');
  if(env_size){::GetEnvironmentVariableW(L"PATH",old_path.data(),env_size);old_path.resize(env_size-1);}
  const std::wstring path=cue::capture::utf8_to_wide(dir)+L";"+old_path;
  ::SetEnvironmentVariableW(L"PATH",path.c_str());
  check(child.start({name,"--helper","eof"},error),"bare executable resolved from PATH");
  check(child.read_exact(data,4,got,1000)==IoStatus::Ok,"PATH executable output");child.stop();
  ::SetEnvironmentVariableW(L"PATH",old_path.c_str());
  check(!child.start({"Z:\\missing-cue-c02\\not-found.exe"},error) && !error.empty(), "missing executable fails at start");
  check(child.start({self,"--helper","closed"},error), "closed stdout source starts");
  check(child.read_some(data,sizeof(data),got,1000)==IoStatus::Eof && child.running(),"EOF while source remains alive");
  child.stop();
  check(child.start({self,"--helper","tree"},error), "tree source starts");
  check(child.read_some(data,sizeof(data),got,1000)==IoStatus::Ok && got>0,"descendant PID visible");
  const DWORD descendant = static_cast<DWORD>(std::stoul(std::string(reinterpret_cast<char*>(data),got)));
  HANDLE owned_descendant=::OpenProcess(SYNCHRONIZE,FALSE,descendant);
  check(owned_descendant!=nullptr,"descendant running");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(child.read_some(data,sizeof(data),got,80)==IoStatus::Timeout,"descendant holds stdout without blocking reader");
  stop_start=std::chrono::steady_clock::now(); child.stop();
  check(std::chrono::steady_clock::now()-stop_start<std::chrono::seconds(5),"tree stop bounded");
  if(owned_descendant){check(::WaitForSingleObject(owned_descendant,1000)==WAIT_OBJECT_0,"Job reaps descendant after parent exits");::CloseHandle(owned_descendant);}
  DWORD handles_before=0,handles_after=0;::GetProcessHandleCount(::GetCurrentProcess(),&handles_before);
  for(int i=0;i<12;++i){check(child.start({self,"--helper","quiet"},error),"repeat start");child.stop();}
  ::GetProcessHandleCount(::GetCurrentProcess(),&handles_after);
  check(handles_after<=handles_before+1,"repeat start/stop does not leak handles");

  // Use the same raw-video pipe request as cue-capture, with a real bounded producer.
  // 80 MiB in four seconds catches the small-pipe polling bottleneck, not an FPS SLA.
  constexpr uint64_t target_bytes = 80ULL * 1024 * 1024;
  const int pipe_bytes = cue::capture::video_pipe_bytes(cue::media::HostPlatform::Windows,
      cue::media::VideoEncodeMode::InProcess, 1920, 1080);
  const bool bulk_started = child.start({self,"--helper","bulk"},error,pipe_bytes);
  check(bulk_started,"raw-video throughput producer starts");
  if (bulk_started) {
    std::array<uint8_t,65536> block{};
    uint64_t received = 0;
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds(4);
    bool payload_ok = true;
    while (received < target_bytes && std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      std::size_t count = 0;
      const auto status = child.read_some(block.data(), block.size(), count,
          static_cast<int>((std::clamp)(remaining, int64_t{0}, int64_t{100})));
      if (status == IoStatus::Ok) {
        received += count;
        if (count && (block.front() != 0xA5 || block[count-1] != 0xA5)) payload_ok = false;
      } else if (status != IoStatus::Timeout) break;
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    child.stop();
    std::printf("raw-video pipe=%d bytes=%llu elapsed_ms=%lld\n",pipe_bytes,
        static_cast<unsigned long long>(received),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
    check(payload_ok && received == target_bytes && elapsed <= std::chrono::seconds(4),
          "actual raw-video pipe request delivers 80 MiB within four seconds");
    check(!child.running(),"throughput producer reaped");
  }
#endif
  std::printf("capture process failures=%d\n",failures);
  return failures?1:0;
}
