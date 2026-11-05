#include "nirqoba/Backend/TargetInfo.h"

#include <string>

#include "llvm/TargetParser/Host.h"

#include "nirqoba/Backend/Optimization.h"
#include "nirqoba/Version.h"  // NIRQOBA_DEFAULT_TARGET_TRIPLE

namespace nirq {
using namespace std;

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

}  // namespace nirq
