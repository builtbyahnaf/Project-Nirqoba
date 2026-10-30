// Driver/ProcessRunner.h — launches child processes and captures their output.
//
// Uses CreateProcessW on Windows so paths containing spaces are handled
// correctly through explicit argument quoting rather than relying on shell
// metacharacter escaping. All quoting is done with the MSVC/CommandLineToArgvW
// convention (backslash-before-quote rules).
#pragma once

#include <string>
#include <vector>

namespace nirq {

struct ProcessResult {
  // Exit code returned by the child process. -1 if the process could not
  // be launched.
  int exit_code = -1;
  // Combined stdout and stderr captured from the child.
  std::string output;
  // True when CreateProcess succeeded (the child may still have exited with
  // a non-zero code).
  bool launched = false;
};

// Launches `executable` with `arguments` as a child process, captures its
// stdout and stderr, and waits for it to exit.
//
// Quoting: every argument that contains a space, tab, double-quote, or
// backslash is quoted using the Windows CommandLineToArgvW convention. The
// executable path is always quoted.
//
// The working directory is inherited from the current process.
ProcessResult run_process(const std::string& executable,
                          const std::vector<std::string>& arguments);

}  // namespace nirq
