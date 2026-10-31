#include "nirqoba/Driver/LinkerEnvironment.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <fstream>
#include <sstream>

// Very small JSON field extractor to avoid introducing a third-party JSON
// library dependency. toolchain.json is machine-written (by bootstrap.ps1)
// and has a flat, well-known structure, so a regex-free string scan suffices.
//
// Algorithm: scan the JSON text for the key pattern:
//   "key" : "value"
// and extract the value string. Works for string-valued keys only, which is
// all toolchain.json contains.

namespace nirq {
using namespace std;
namespace {

// Extracts the value of a JSON string field named `key` from `json_text`.
// Returns an empty string if the field is missing or the value is not a string.
static string extract_json_string(const string& json_text,
                                  const string& key) {
  const string search = "\"" + key + "\"";
  size_t pos = json_text.find(search);
  if (pos == string::npos) return {};
  pos += search.size();
  // Skip whitespace and ':'
  while (pos < json_text.size() && (json_text[pos] == ' ' || json_text[pos] == ':' || json_text[pos] == '\t' || json_text[pos] == '\n' || json_text[pos] == '\r')) ++pos;
  if (pos >= json_text.size() || json_text[pos] != '"') return {};
  ++pos;  // skip opening quote
  string result;
  while (pos < json_text.size() && json_text[pos] != '"') {
    if (json_text[pos] == '\\' && pos + 1 < json_text.size()) {
      ++pos;
      if (json_text[pos] == '\\') result.push_back('\\');
      else if (json_text[pos] == '"') result.push_back('"');
      else if (json_text[pos] == '/') result.push_back('/');
      else { result.push_back('\\'); result.push_back(json_text[pos]); }
    } else {
      result.push_back(json_text[pos]);
    }
    ++pos;
  }
  return result;
}

static string read_file(const string& path) {
  ifstream file(path);
  if (!file.is_open()) return {};
  ostringstream buf;
  buf << file.rdbuf();
  return buf.str();
}

// Best-effort: walk up from the executable's directory to find the repo root
// (the directory containing toolchain/versions.lock.json). Returns empty if
// not found.
static string find_repo_root() {
  // On Windows we use GetModuleFileNameW for the executable path.
#ifdef _WIN32
  wchar_t buf[4096] = {};
  if (GetModuleFileNameW(nullptr, buf, 4096) == 0) return {};
  // Convert to narrow string (ASCII/UTF-8 paths on our build machine).
  string path;
  for (int i = 0; buf[i]; ++i) path.push_back(static_cast<char>(buf[i]));
  // Walk up to find the directory containing toolchain/
  while (!path.empty()) {
    const auto sep = path.find_last_of("/\\");
    if (sep == string::npos) break;
    path = path.substr(0, sep);
    const string candidate = path + "\\toolchain\\versions.lock.json";
    ifstream probe(candidate);
    if (probe.is_open()) return path;
  }
#endif
  return {};
}

}  // namespace

LinkerEnvironment load_linker_environment(const string& toolchain_json_path,
                                          DiagnosticEngine& diagnostics) {
  LinkerEnvironment env;

  // Determine the toolchain.json path.
  string json_path = toolchain_json_path;
  if (json_path.empty()) {
    const string root = find_repo_root();
    if (root.empty()) {
      diagnostics.error(diag::kToolchainUnavailable, SourceLocation{},
                        "toolchain/toolchain.json not found; run scripts/bootstrap.ps1");
      return env;
    }
    json_path = root + "\\toolchain\\toolchain.json";
  }

  const string json_text = read_file(json_path);
  if (json_text.empty()) {
    diagnostics.error(diag::kToolchainUnavailable, SourceLocation{},
                      "toolchain configuration not found at '" + json_path +
                          "'; run scripts/bootstrap.ps1 (NRQ5003)");
    return env;
  }

  // Prefer lld-link; fall back to MSVC link.exe.
  string linker = extract_json_string(json_text, "linker");
  if (linker.empty()) {
    linker = extract_json_string(json_text, "linkerFallback");
  }
  if (linker.empty()) {
    diagnostics.error(diag::kToolchainUnavailable, SourceLocation{},
                      "toolchain.json contains no linker path; re-run scripts/bootstrap.ps1");
    return env;
  }
  env.linker_path = linker;

  // Library directories: MSVC, UCRT, and UM (Windows SDK).
  for (const char* key : {"msvcLib", "sdkUcrtLib", "sdkUmLib"}) {
    const string dir = extract_json_string(json_text, key);
    if (!dir.empty()) env.lib_dirs.push_back(dir);
  }

  env.is_valid = true;
  return env;
}

}  // namespace nirq
