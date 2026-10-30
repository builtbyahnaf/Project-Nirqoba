// Multi-file module dependency resolver and topological sorter (Phase 5).
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/Frontend/SourceFile.h"

namespace nirq {

struct ModuleUnit {
  std::string name;
  std::filesystem::path file_path;
  std::unique_ptr<SourceFile> source_file;
  std::unique_ptr<Program> ast;
  bool is_root = false;
};

class ModuleResolver {
 public:
  explicit ModuleResolver(DiagnosticEngine& diagnostics,
                          std::vector<std::filesystem::path> search_paths = {})
      : diagnostics_(diagnostics), search_paths_(std::move(search_paths)) {
    if (search_paths_.empty()) {
      search_paths_.push_back(std::filesystem::current_path());
    }
  }

  void add_search_path(const std::filesystem::path& path) {
    search_paths_.push_back(path);
  }

  // Discovers and parses all dependencies starting from root files.
  // Performs cycle detection and returns modules in topological order (dependencies first).
  std::vector<std::unique_ptr<ModuleUnit>> resolve(
      const std::vector<std::filesystem::path>& root_files);

 private:
  enum class NodeState { Unvisited, Visiting, Visited };

  bool load_module(const std::filesystem::path& file_path, bool is_root,
                   std::string& out_module_name);
  std::filesystem::path find_module_file(const std::string& module_name,
                                         const std::filesystem::path& referencing_file);
  bool dfs_sort(const std::string& module_name,
                std::unordered_map<std::string, NodeState>& states,
                std::vector<std::string>& current_path,
                std::vector<std::string>& sorted_names);

  DiagnosticEngine& diagnostics_;
  std::vector<std::filesystem::path> search_paths_;
  std::unordered_map<std::string, std::unique_ptr<ModuleUnit>> loaded_modules_;
  std::unordered_map<std::string, std::vector<std::string>> dependency_edges_;
};

}  // namespace nirq
