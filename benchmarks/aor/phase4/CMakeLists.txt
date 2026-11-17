# The representation benchmark links the development runtime, the same library the
# unit tests audit, so the numbers describe the code the tests exercise.
add_executable(nirqoba_phase4_benchmark phase4_benchmark.cpp)

set_property(TARGET nirqoba_phase4_benchmark PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_phase4_benchmark PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_phase4_benchmark)

target_link_libraries(nirqoba_phase4_benchmark PRIVATE nirqoba_runtime_debug)
