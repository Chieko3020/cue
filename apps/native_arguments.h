// App-private command-line storage. Parsers borrow pointers only while this owner lives.
#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace cue::app {

class NativeArguments {
 public:
  NativeArguments() = default;
  [[nodiscard]] std::expected<void, std::string> initialize(int argc, char** argv) {
    if (initialized_) return std::unexpected("native arguments already initialized");
    initialized_ = true;
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    // CRT narrow argv follows the active code page; SDL and our parsers use UTF-8.
    const auto fail = [](const char* operation) {
      const DWORD code = ::GetLastError();
      return std::unexpected(std::string(operation) + ": Win32 error " + std::to_string(code));
    };
    int count = 0;
    struct WideArguments {
      LPWSTR* data;
      ~WideArguments() { if (data) ::LocalFree(data); }
    } wide{::CommandLineToArgvW(::GetCommandLineW(), &count)};
    if (!wide.data) return fail("CommandLineToArgvW");
    if (count <= 0) {
      return std::unexpected("native command line has no program argument");
    }
    storage_.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data[i], -1,
                                             nullptr, 0, nullptr, nullptr);
      if (bytes <= 0) return fail("WideCharToMultiByte(size)");
      std::string text(static_cast<std::size_t>(bytes), '\0');
      if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data[i], -1, text.data(),
                               bytes, nullptr, nullptr) != bytes) {
        return fail("WideCharToMultiByte");
      }
      text.pop_back();  // Keep empty arguments, with std::string's own trailing NUL.
      storage_.push_back(std::move(text));
    }
    pointers_.reserve(static_cast<std::size_t>(count) + 1);
    for (auto& text : storage_) pointers_.push_back(text.data());
    pointers_.push_back(nullptr);
    count_ = count;
    data_ = pointers_.data();
#else
    count_ = argc;
    data_ = argv;
#endif
    return {};
  }
  NativeArguments(const NativeArguments&) = delete;
  NativeArguments& operator=(const NativeArguments&) = delete;
  NativeArguments(NativeArguments&&) = delete;
  NativeArguments& operator=(NativeArguments&&) = delete;

  int argc() const { return count_; }
  char** argv() const { return data_; }

 private:
  int count_ = 0;
  char** data_ = nullptr;
  bool initialized_ = false;
  std::vector<std::string> storage_;
  std::vector<char*> pointers_;
};

}  // namespace cue::app
