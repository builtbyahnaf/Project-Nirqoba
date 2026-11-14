// Internal shutdown hooks for deterministic native resources.
//
// These registries own OS handles independently of the language's GC. Program
// code normally closes them explicitly, but a process that exits with a live
// handle must still hand it back deterministically. The hooks are called once
// from nrq_runtime_shutdown after the user's last return.
#pragma once

namespace nirq::runtime {

// `wait` false is the termination path: if another thread holds a registry or
// operation lock, skip that registry and let ExitProcess reclaim its handles.
// Blocking there could wait forever on a blocked socket/file operation.
void release_file_resources(bool wait) noexcept;
void release_mapping_resources(bool wait) noexcept;
void release_mutex_resources(bool wait) noexcept;

}  // namespace nirq::runtime
