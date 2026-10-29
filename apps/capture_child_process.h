// App-private capture producer ownership. One reader owns stdout; stop is called after it joins.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "cue/media/ffmpeg_args.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace cue::capture {
enum class IoStatus : uint8_t { Ok = 0, Timeout, Eof, Error };

#if defined(_WIN32)
class OwnedHandle {
 public:
  explicit OwnedHandle(HANDLE h = nullptr) : handle_(h) {}
  ~OwnedHandle() { reset(); }
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  HANDLE get() const { return handle_; }
  HANDLE release() { HANDLE h = handle_; handle_ = nullptr; return h; }
  void reset(HANDLE h = nullptr) {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
    handle_ = h;
  }
 private:
  HANDLE handle_;
};

inline std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), n);
  return out;
}
#endif

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { stop(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  bool start(const std::vector<std::string>& argv, std::string& err, int pipe_bytes = 0) {
    stop();
    if (argv.empty()) { err = "empty argv"; return false; }
#if defined(_WIN32)
    return start_windows(argv, err, pipe_bytes);
#else
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
      err = std::string("pipe2: ") + std::strerror(errno);
      return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
      ::close(fds[0]);
      ::close(fds[1]);
      err = std::string("fork: ") + std::strerror(errno);
      return false;
    }
    if (pid == 0) {
      // 子进程只保留 stdout 管道；-nostdin 已在参数里，避免 ffmpeg 抢终端输入。
      ::dup2(fds[1], STDOUT_FILENO);
      ::close(fds[0]);
      ::close(fds[1]);
      std::vector<char*> cargv;
      cargv.reserve(argv.size() + 1);
      for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
      cargv.push_back(nullptr);
      ::execvp(cargv[0], cargv.data());
      std::fprintf(stderr, "[cue-capture] execvp(%s) 失败: %s\n", cargv[0], std::strerror(errno));
      ::_exit(127);
    }
    ::close(fds[1]);
    fd_ = fds[0];
    if (pipe_bytes > 0) {
      // 失败不致命（内核可能不允许该尺寸）：指标仍然可用，只是读侧多一点缓冲。
      (void)::fcntl(fd_, F_SETPIPE_SZ, pipe_bytes);
    }
    pid_ = pid;
    argv_ = argv;
    return true;
#endif
  }

  IoStatus read_some(uint8_t* dst, std::size_t cap, std::size_t& got, int timeout_ms) {
    got = 0;
    if (cap == 0) return IoStatus::Ok;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds((std::max)(timeout_ms, 0));
#if defined(_WIN32)
    if (!read_.get()) return IoStatus::Eof;
    for (;;) {
      DWORD available = 0;
      if (!::PeekNamedPipe(read_.get(), nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD error = ::GetLastError();
        return error == ERROR_BROKEN_PIPE ? IoStatus::Eof : IoStatus::Error;
      }
      if (available > 0) {
        DWORD n = 0;
        const DWORD wanted = static_cast<DWORD>((std::min)(cap, static_cast<std::size_t>(available)));
        if (!::ReadFile(read_.get(), dst, wanted, &n, nullptr)) {
          return ::GetLastError() == ERROR_BROKEN_PIPE ? IoStatus::Eof : IoStatus::Error;
        }
        got = n;
        return n ? IoStatus::Ok : IoStatus::Eof;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return IoStatus::Timeout;
      std::this_thread::sleep_for((std::min)(std::chrono::milliseconds(5),
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
#else
    if (fd_ < 0) return IoStatus::Eof;
    for (;;) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      pollfd p{fd_, POLLIN, 0};
      const int pr = ::poll(&p, 1, static_cast<int>((std::max)(int64_t{0}, remaining)));
      if (pr == 0) return IoStatus::Timeout;
      if (pr < 0) { if (errno == EINTR) continue; return IoStatus::Error; }
      const ssize_t n = ::read(fd_, dst, cap);
      if (n == 0) return IoStatus::Eof;
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return IoStatus::Error;
      }
      got = static_cast<std::size_t>(n);
      return IoStatus::Ok;
    }
#endif
  }

  // Caller retains got bytes on Timeout and resumes the same frame at that offset.
  IoStatus read_exact(uint8_t* dst, std::size_t n, std::size_t& got, int timeout_ms) {
    got = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds((std::max)(timeout_ms, 0));
    while (got < n) {
      std::size_t one = 0;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      const IoStatus status = read_some(dst + got, n - got, one,
                                       static_cast<int>((std::max)(int64_t{0}, left)));
      got += one;
      if (status != IoStatus::Ok) return status;
      if (got < n && std::chrono::steady_clock::now() >= deadline) return IoStatus::Timeout;
    }
    return IoStatus::Ok;
  }

  bool running() const {
#if defined(_WIN32)
    return process_.get() && ::WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
#else
    return pid_ > 0;
#endif
  }
  const std::vector<std::string>& argv() const { return argv_; }
  // The caller requests cancellation first; bounded reads let the sole reader exit.
  void stop_after_reader(std::thread& reader) {
    if (reader.joinable()) reader.join();
    stop();
  }
  void stop() {
#if defined(_WIN32)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (job_.get()) {
      ::TerminateJobObject(job_.get(), 1);
      for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (!::QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation,
                                         &info, sizeof(info), nullptr) || info.ActiveProcesses == 0) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        ::Sleep(5);
      }
    }
    // KILL_ON_JOB_CLOSE is also the failure/destructor safety net.
    job_.reset();
    process_.reset();
    read_.reset();
#else
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      for (int i = 0; i < 20; ++i) {
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) { pid_ = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (pid_ > 0) { ::kill(pid_, SIGKILL); ::waitpid(pid_, &status, 0); pid_ = -1; }
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
  }

 private:
#if defined(_WIN32)
  bool start_windows(const std::vector<std::string>& argv, std::string& err, int pipe_bytes) {
    const auto fail = [&err](const char* operation) {
      err = std::string(operation) + ": Win32 error " + std::to_string(::GetLastError());
      return false;
    };
    std::wstring command = utf8_to_wide(cue::media::build_windows_command_line(argv));
    std::wstring executable = utf8_to_wide(argv.front());
    if (command.empty() || executable.empty()) { err = "invalid UTF-8 argv"; return false; }
    // CreateProcessW with lpApplicationName does not search PATH or add .exe.
    const DWORD path_size = ::SearchPathW(nullptr, executable.c_str(), L".exe", 0, nullptr, nullptr);
    if (!path_size) return fail("SearchPathW");
    std::wstring resolved(static_cast<std::size_t>(path_size), L'\0');
    const DWORD path_length = ::SearchPathW(nullptr, executable.c_str(), L".exe", path_size,
                                           resolved.data(), nullptr);
    if (!path_length || path_length >= path_size) return fail("SearchPathW");
    resolved.resize(path_length);
    OwnedHandle job(::CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) return fail("CreateJobObjectW");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                   &limits, sizeof(limits))) return fail("SetInformationJobObject");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!::CreatePipe(&reader, &writer, &sa, pipe_bytes > 0 ? static_cast<DWORD>(pipe_bytes) : 0))
      return fail("CreatePipe");
    OwnedHandle read(reader), write(writer);
    if (!::SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0)) return fail("SetHandleInformation");
    // Duplicate only the intended standard handles; do not leak other inheritable handles.
    OwnedHandle input, error;
    const auto standard = [&sa](DWORD which, OwnedHandle& out, DWORD access) {
      HANDLE source = ::GetStdHandle(which), copy = nullptr;
      if (source && source != INVALID_HANDLE_VALUE &&
          ::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &copy,
                            0, TRUE, DUPLICATE_SAME_ACCESS)) { out.reset(copy); return true; }
      out.reset(::CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      return out.get() != INVALID_HANDLE_VALUE;
    };
    if (!standard(STD_INPUT_HANDLE, input, GENERIC_READ) ||
        !standard(STD_ERROR_HANDLE, error, GENERIC_WRITE)) return fail("standard handle");
    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
      return fail("InitializeProcThreadAttributeList");
    HANDLE inherited[] = {input.get(), write.get(), error.get()};
    const BOOL attr_ok = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                    inherited, sizeof(inherited), nullptr, nullptr);
    if (!attr_ok) {
      const DWORD code = ::GetLastError();
      ::DeleteProcThreadAttributeList(attributes);
      ::SetLastError(code);
      return fail("UpdateProcThreadAttribute");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = write.get();
    startup.StartupInfo.hStdError = error.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    const BOOL created = ::CreateProcessW(resolved.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &info);
    const DWORD create_error = ::GetLastError();
    ::DeleteProcThreadAttributeList(attributes);
    if (!created) { ::SetLastError(create_error); return fail("CreateProcessW"); }
    OwnedHandle process(info.hProcess), thread(info.hThread);
    if (!::AssignProcessToJobObject(job.get(), process.get()) ||
        ::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
      const DWORD code = ::GetLastError();
      ::TerminateProcess(process.get(), 1);
      ::WaitForSingleObject(process.get(), 2000);
      ::SetLastError(code);
      return fail("AssignProcessToJobObject/ResumeThread");
    }
    process_.reset(process.release());
    job_.reset(job.release());
    read_.reset(read.release());
    argv_ = argv;
    return true;
  }
  OwnedHandle process_, job_, read_;
#else
  pid_t pid_ = -1;
  int fd_ = -1;
#endif
  std::vector<std::string> argv_;
};
}  // namespace cue::capture
