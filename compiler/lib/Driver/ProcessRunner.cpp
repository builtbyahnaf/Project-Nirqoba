#include "nirqoba/Driver/ProcessRunner.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <vector>
#include <string>

namespace nirq {
namespace {

#ifdef _WIN32
// Converts a UTF-8 string to UTF-16 std::wstring for Win32 API calls.
static std::wstring to_wstring(const std::string& str) {
  if (str.empty()) return {};
  int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
  if (size_needed <= 0) return {};
  std::wstring result(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), &result[0], size_needed);
  return result;
}

// Converts a UTF-16 string back to UTF-8.
static std::string to_string(const wchar_t* wstr, size_t length) {
  if (length == 0 || !wstr) return {};
  int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  if (size_needed <= 0) return {};
  std::string result(size_needed, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(length), &result[0], size_needed, nullptr, nullptr);
  return result;
}

// Formats a command line argument according to the Microsoft CommandLineToArgvW rules:
// - Arguments with whitespace or quotes must be enclosed in quotes.
// - Backslashes immediately preceding a double quote must be doubled.
// - Trailing backslashes before the closing quote must also be doubled.
static std::wstring quote_argument(const std::wstring& arg) {
  if (arg.empty()) {
    return L"\"\"";
  }

  bool needs_quotes = false;
  for (wchar_t c : arg) {
    if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'\"') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) {
    return arg;
  }

  std::wstring result = L"\"";
  size_t num_backslashes = 0;

  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++num_backslashes;
    } else if (c == L'\"') {
      // 2N + 1 backslashes before quote
      result.append(num_backslashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      num_backslashes = 0;
    } else {
      result.append(num_backslashes, L'\\');
      num_backslashes = 0;
      result.push_back(c);
    }
  }

  // 2N backslashes before closing quote
  result.append(num_backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}
#endif

}  // namespace

ProcessResult run_process(const std::string& executable,
                          const std::vector<std::string>& arguments) {
  ProcessResult result;
#ifdef _WIN32
  // Create pipes for stdout/stderr redirection.
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE hChildRead = INVALID_HANDLE_VALUE;
  HANDLE hChildWrite = INVALID_HANDLE_VALUE;

  if (!CreatePipe(&hChildRead, &hChildWrite, &sa, 0)) {
    result.output = "Failed to create pipe for child process output";
    return result;
  }

  // Ensure read handle is not inherited by child.
  SetHandleInformation(hChildRead, HANDLE_FLAG_INHERIT, 0);

  // Build the complete command line string.
  std::wstring cmdline = quote_argument(to_wstring(executable));
  for (const auto& arg : arguments) {
    cmdline += L" " + quote_argument(to_wstring(arg));
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdOutput = hChildWrite;
  si.hStdError = hChildWrite;
  si.hStdInput = INVALID_HANDLE_VALUE;

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back(L'\0');

  std::wstring exe_w = to_wstring(executable);

  BOOL success = CreateProcessW(
      exe_w.c_str(),
      cmdline_buf.data(),
      nullptr,
      nullptr,
      TRUE,  // inherit handles
      0,
      nullptr,
      nullptr,
      &si,
      &pi);

  // Close the write end of the pipe in parent so EOF can be read.
  CloseHandle(hChildWrite);

  if (!success) {
    CloseHandle(hChildRead);
    result.launched = false;
    result.exit_code = -1;
    DWORD err = GetLastError();
    result.output = "CreateProcessW failed with error " + std::to_string(err);
    return result;
  }

  result.launched = true;

  // Read output from the pipe until EOF.
  char buffer[4096];
  DWORD bytes_read = 0;
  while (ReadFile(hChildRead, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
    result.output.append(buffer, bytes_read);
  }
  CloseHandle(hChildRead);

  // Wait for child process termination.
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
    result.exit_code = static_cast<int>(exit_code);
  } else {
    result.exit_code = -1;
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  result.output = "ProcessRunner is only implemented for Windows in Phase 1";
  result.launched = false;
  result.exit_code = -1;
#endif
  return result;
}

}  // namespace nirq
