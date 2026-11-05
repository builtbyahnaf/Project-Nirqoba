#include "nirqoba/Backend/TargetInfo.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Version.h"  // NIRQOBA_DEFAULT_TARGET_TRIPLE

namespace nirq {
using namespace std;

namespace {

void ensure_targets_initialized() {
  // Idempotent; safe to call from both resolution and code generation.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
}

void report_target_error(DiagnosticEngine& diagnostics, string message) {
  diagnostics.error(diag::kInvalidCommandLine, SourceLocation{},
                    move(message));
}

// Converts the host feature map into the comma-separated form accepted by
// LLVM's TargetMachine. Sorting keeps diagnostic output and object files
// reproducible for the same host.
string host_feature_string() {
  llvm::StringMap<bool> features = llvm::sys::getHostCPUFeatures();
  vector<pair<string, bool>> ordered;
  ordered.reserve(features.size());
  for (const auto& feature : features) {
    ordered.emplace_back(feature.first().str(), feature.second);
  }
  sort(ordered.begin(), ordered.end());

  string result;
  for (const auto& feature : ordered) {
    if (!result.empty()) result.push_back(',');
    result.push_back(feature.second ? '+' : '-');
    result += feature.first;
  }
  return result;
}

// Checks every comma-separated feature token against the selected target's
// feature table. LLVM itself only warns on unknown features, which would let a
// build silently fall back to a different instruction set.
bool validate_features(const llvm::MCSubtargetInfo& subtarget,
                       const string& features,
                       const string& triple,
                       DiagnosticEngine& diagnostics) {
  if (features.empty()) return true;

  const auto known = subtarget.getAllProcessorFeatures();
  size_t start = 0;
  while (true) {
    const size_t comma = features.find(',', start);
    const string token =
        features.substr(start, comma == string::npos ? string::npos
                                                     : comma - start);
    if (token.empty() || (token.front() != '+' && token.front() != '-') ||
        token.size() == 1) {
      report_target_error(
          diagnostics,
          "malformed target feature string '" + features + "' for '" +
              triple +
              "'; expected comma-separated entries such as +avx2,-sse4.2");
      return false;
    }

    const string name = token.substr(1);
    const bool found = any_of(
        known.begin(), known.end(),
        [&name](const llvm::SubtargetFeatureKV& feature) {
          return name == feature.key();
        });
    if (!found) {
      report_target_error(diagnostics,
                          "unknown target feature '" + token + "' for '" +
                              triple + "'");
      return false;
    }

    if (comma == string::npos) break;
    start = comma + 1;
  }
  return true;
}

// Resolves and validates the target triple/CPU/features for `profile`.
bool resolve_target(const TargetConfiguration& candidate,
                    TargetConfiguration& resolved,
                    DiagnosticEngine& diagnostics) {
  ensure_targets_initialized();

  const llvm::Triple triple(candidate.triple);
  string lookup_error;
  const llvm::Target* target =
      llvm::TargetRegistry::lookupTarget(triple, lookup_error);
  if (target == nullptr) {
    report_target_error(diagnostics,
                        "unsupported target triple '" + candidate.triple +
                            "': " + lookup_error);
    return false;
  }

  // Use an empty CPU here so LLVM does not print its own "not a recognized
  // processor" warning before this function can report a precise diagnostic.
  unique_ptr<llvm::MCSubtargetInfo> subtarget(
      target->createMCSubtargetInfo(triple, "", ""));
  if (!subtarget) {
    report_target_error(diagnostics,
                        "cannot inspect target features for '" +
                            candidate.triple + "'");
    return false;
  }

  if (!subtarget->isCPUStringValid(candidate.cpu)) {
    report_target_error(diagnostics,
                        "unknown target CPU '" + candidate.cpu + "' for '" +
                            candidate.triple + "'");
    return false;
  }

  if (!validate_features(*subtarget, candidate.features, candidate.triple,
                         diagnostics)) {
    return false;
  }

  resolved = candidate;
  return true;
}

}  // namespace

string default_target_triple() {
  // The Windows x64 target is the supported release target. The triple can be
  // overridden on the command line for experimentation, but the packaged
  // runtime is still a Windows static library.
  return NIRQOBA_DEFAULT_TARGET_TRIPLE;
}

string host_target_triple() {
  return llvm::sys::getDefaultTargetTriple();
}

string_view target_profile_name(TargetProfile profile) {
  switch (profile) {
    case TargetProfile::Portable: return "portable";
    case TargetProfile::Native: return "native";
    case TargetProfile::Explicit: return "explicit";
  }
  return "portable";
}

bool parse_target_profile(string_view text, TargetProfile& profile) {
  if (text == "portable") { profile = TargetProfile::Portable; return true; }
  if (text == "native") { profile = TargetProfile::Native; return true; }
  if (text == "explicit") { profile = TargetProfile::Explicit; return true; }
  return false;
}

bool resolve_target_configuration(const OptimizationOptions& options,
                                  TargetConfiguration& resolved,
                                  DiagnosticEngine& diagnostics) {
  const string default_triple = default_target_triple();

  switch (options.target_profile) {
    case TargetProfile::Portable: {
      if (!options.target_triple.empty() &&
          options.target_triple != default_triple) {
        report_target_error(
            diagnostics,
            "target profile 'portable' does not allow an explicit target "
            "triple ('" + options.target_triple +
                "'); use --target-profile=explicit");
        return false;
      }
      if (!options.target_cpu.empty() && options.target_cpu != "generic") {
        report_target_error(
            diagnostics,
            "target profile 'portable' does not allow a named CPU ('" +
                options.target_cpu +
                "'); use --target-profile=explicit or --native");
        return false;
      }
      if (!options.target_features.empty()) {
        report_target_error(
            diagnostics,
            "target profile 'portable' does not allow target features ('" +
                options.target_features +
                "'); use --target-profile=explicit or --native");
        return false;
      }

      TargetConfiguration candidate;
      candidate.profile = TargetProfile::Portable;
      candidate.triple = default_triple;
      candidate.cpu = "generic";
      return resolve_target(candidate, resolved, diagnostics);
    }

    case TargetProfile::Native: {
      if (!options.target_triple.empty()) {
        report_target_error(
            diagnostics,
            "target profile 'native' derives the triple from the host; remove "
            "the explicit target triple ('" + options.target_triple + "')");
        return false;
      }
      if (!options.target_cpu.empty() && options.target_cpu != "generic") {
        report_target_error(
            diagnostics,
            "target profile 'native' derives the CPU from the host; remove "
            "--target-cpu='" + options.target_cpu + "'");
        return false;
      }
      if (!options.target_features.empty()) {
        report_target_error(
            diagnostics,
            "target profile 'native' derives features from the host; remove "
            "--target-features='" + options.target_features + "'");
        return false;
      }

      TargetConfiguration candidate;
      candidate.profile = TargetProfile::Native;
      candidate.triple = host_target_triple();
      candidate.cpu = llvm::sys::getHostCPUName().str();
      if (candidate.cpu.empty() || candidate.cpu == "unknown") {
        report_target_error(diagnostics,
                            "cannot detect the host CPU model for the native "
                            "target profile");
        return false;
      }
      candidate.features = host_feature_string();
      candidate.from_host = true;
      return resolve_target(candidate, resolved, diagnostics);
    }

    case TargetProfile::Explicit: {
      TargetConfiguration candidate;
      candidate.profile = TargetProfile::Explicit;
      candidate.triple =
          options.target_triple.empty() ? default_triple : options.target_triple;
      candidate.cpu =
          options.target_cpu.empty() ? "generic" : options.target_cpu;
      candidate.features = options.target_features;
      return resolve_target(candidate, resolved, diagnostics);
    }
  }

  report_target_error(diagnostics, "unknown target profile");
  return false;
}

string describe_target_configuration(const TargetConfiguration& config) {
  string description(target_profile_name(config.profile));
  description += " (";
  description += config.triple;
  description += ", cpu ";
  description += config.cpu;
  if (!config.features.empty()) {
    description += ", features ";
    description += config.features;
  }
  description += ")";
  return description;
}

}  // namespace nirq
