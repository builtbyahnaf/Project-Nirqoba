#include "nirqoba/Driver/ModuleResolver.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"

namespace nirq {

std::vector<std::unique_ptr<ModuleUnit>> ModuleResolver::resolve(
    const std::vector<std::filesystem::path>& root_files) {
  loaded_modules_.clear();
  dependency_edges_.clear();

  std::vector<std::string> root_module_names;
  for (const auto& root_file : root_files) {
    std::string mod_name;
    if (!load_module(root_file, true, mod_name)) {
      return {};
    }
    root_module_names.push_back(mod_name);
  }

  // Iteratively resolve imports of all loaded modules.
  std::vector<std::string> queue = root_module_names;
  std::size_t head = 0;
  while (head < queue.size()) {
    const std::string current_name = queue[head++];
    const auto it = loaded_modules_.find(current_name);
    if (it == loaded_modules_.end() || !it->second->ast) continue;

    const auto& imports = it->second->ast->imports();
    for (const auto& imp : imports) {
      const std::string& target_mod = imp->module_name();

      // Check if standard library virtual namespace (e.g. io, file, math, time, system)
      if (target_mod == "io" || target_mod == "file" || target_mod == "math" ||
          target_mod == "time" || target_mod == "system") {
        continue;
      }

      dependency_edges_[current_name].push_back(target_mod);

      if (loaded_modules_.find(target_mod) == loaded_modules_.end()) {
        std::filesystem::path target_file =
            find_module_file(target_mod, it->second->file_path);
        if (target_file.empty()) {
          diagnostics_.error(diag::kModuleNotFound, imp->location(),
                             "module '" + target_mod + "' not found in search paths");
          return {};
        }

        std::string loaded_name;
        if (!load_module(target_file, false, loaded_name)) {
          return {};
        }
        queue.push_back(loaded_name);
      }
    }
  }

  // Detect cycles and perform topological sort using DFS.
  std::unordered_map<std::string, NodeState> states;
  for (const auto& kv : loaded_modules_) {
    states[kv.first] = NodeState::Unvisited;
  }

  std::vector<std::string> current_path;
  std::vector<std::string> sorted_names;

  for (const auto& root : root_module_names) {
    if (states[root] == NodeState::Unvisited) {
      if (!dfs_sort(root, states, current_path, sorted_names)) {
        return {};
      }
    }
  }

  // Also visit any remaining unvisited modules.
  for (const auto& kv : loaded_modules_) {
    if (states[kv.first] == NodeState::Unvisited) {
      if (!dfs_sort(kv.first, states, current_path, sorted_names)) {
        return {};
      }
    }
  }

  // Assemble result in sorted order.
  std::vector<std::unique_ptr<ModuleUnit>> result;
  result.reserve(sorted_names.size());
  for (const auto& name : sorted_names) {
    auto found = loaded_modules_.find(name);
    if (found != loaded_modules_.end() && found->second) {
      result.push_back(std::move(found->second));
    }
  }

  return result;
}

bool ModuleResolver::load_module(const std::filesystem::path& file_path, bool is_root,
                                 std::string& out_module_name) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    diagnostics_.error(diag::kMissingSourceFile, SourceLocation{},
                       "could not open source file '" + file_path.string() + "'");
    return false;
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  std::string source_text = ss.str();

  auto src = std::make_unique<SourceFile>(file_path.string(), std::move(source_text));
  Lexer lexer(*src, diagnostics_);
  auto tokens = lexer.tokenize();
  if (diagnostics_.has_errors()) return false;

  Parser parser(std::move(tokens), diagnostics_);
  auto ast = parser.parse_program();
  if (!ast || diagnostics_.has_errors()) return false;

  std::string mod_name;
  if (ast->module_decl()) {
    mod_name = ast->module_decl()->name();
  } else {
    mod_name = file_path.stem().string();
  }

  if (loaded_modules_.find(mod_name) != loaded_modules_.end()) {
    diagnostics_.error(diag::kDuplicateModule,
                       ast->module_decl() ? ast->module_decl()->location() : SourceLocation{},
                       "duplicate definition of module '" + mod_name + "'");
    return false;
  }

  auto unit = std::make_unique<ModuleUnit>();
  unit->name = mod_name;
  unit->file_path = file_path;
  unit->source_file = std::move(src);
  unit->ast = std::move(ast);
  unit->is_root = is_root;

  out_module_name = mod_name;
  loaded_modules_[mod_name] = std::move(unit);
  return true;
}

std::filesystem::path ModuleResolver::find_module_file(
    const std::string& module_name, const std::filesystem::path& referencing_file) {
  // First, check directory of referencing file.
  if (!referencing_file.empty()) {
    std::filesystem::path candidate =
        referencing_file.parent_path() / (module_name + ".nrqba");
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  // Then check each configured search path.
  for (const auto& sp : search_paths_) {
    std::filesystem::path candidate = sp / (module_name + ".nrqba");
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  return {};
}

bool ModuleResolver::dfs_sort(const std::string& module_name,
                              std::unordered_map<std::string, NodeState>& states,
                              std::vector<std::string>& current_path,
                              std::vector<std::string>& sorted_names) {
  states[module_name] = NodeState::Visiting;
  current_path.push_back(module_name);

  auto edges_it = dependency_edges_.find(module_name);
  if (edges_it != dependency_edges_.end()) {
    for (const auto& dep : edges_it->second) {
      if (states.find(dep) == states.end()) continue;

      if (states[dep] == NodeState::Visiting) {
        // Cycle detected!
        std::string cycle_str;
        bool in_cycle = false;
        for (const auto& p : current_path) {
          if (p == dep) in_cycle = true;
          if (in_cycle) {
            cycle_str += p + " -> ";
          }
        }
        cycle_str += dep;

        SourceLocation loc;
        auto mod_it = loaded_modules_.find(module_name);
        if (mod_it != loaded_modules_.end() && mod_it->second->ast) {
          for (const auto& imp : mod_it->second->ast->imports()) {
            if (imp->module_name() == dep) {
              loc = imp->location();
              break;
            }
          }
        }

        diagnostics_.error(diag::kCyclicDependency, loc,
                           "cyclic module dependency detected: " + cycle_str);
        return false;
      }

      if (states[dep] == NodeState::Unvisited) {
        if (!dfs_sort(dep, states, current_path, sorted_names)) {
          return false;
        }
      }
    }
  }

  current_path.pop_back();
  states[module_name] = NodeState::Visited;
  sorted_names.push_back(module_name);
  return true;
}

}  // namespace nirq
