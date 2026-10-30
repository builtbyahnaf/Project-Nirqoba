#pragma once

#include <vector>
#include <string>

#include "nirqoba/CLI/CommandLine.h"

namespace nirq {

// Parses raw argv strings into CommandLineOptions.
// Returns false on invalid usage (with diagnostic message printed or returned).
bool parse_command_line(int argc, const char* const* argv,
                        CommandLineOptions& options,
                        std::string& error_message);

// Main CLI entry point. Returns exit code (0..4).
int run_driver(int argc, const char* const* argv);

}  // namespace nirq
