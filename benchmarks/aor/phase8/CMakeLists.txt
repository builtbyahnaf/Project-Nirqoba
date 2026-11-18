# The workload matrix links the development runtime, so the numbers it reports are
# the ones the unit tests audit. The production configuration is measured by
# building the same target in the release tree, which links the release runtime.
add_executable(nirqoba_phase8_matrix gc_workload_matrix.cpp)

set_property(TARGET nirqoba_phase8_matrix PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_phase8_matrix PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_phase8_matrix)

target_link_libraries(nirqoba_phase8_matrix PRIVATE nirqoba_runtime_debug)
