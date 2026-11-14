// Internal shutdown hooks for deterministic native resources.
//
// These registries own OS handles independently of the language's GC. Program
// code normally closes them explicitly, but a process that exits with a live
// handle must still hand it back deterministically. The hooks are called once
// from nrq_runtime_shutdown after the user's last return.
#pragma once

namespace nirq::runtime {

void release_file_resources() noexcept;
void release_mapping_resources() noexcept;
void release_mutex_resources() noexcept;

}  // namespace nirq::runtime
