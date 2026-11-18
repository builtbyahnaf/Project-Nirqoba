# The arena and pool benchmark links the development runtime, the same library the
# unit tests exercise, so the numbers describe the code the tests audit.
add_executable(nirqoba_phase5_benchmark phase5_benchmark.cpp)

set_property(TARGET nirqoba_phase5_benchmark PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_phase5_benchmark PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_phase5_benchmark)

target_link_libraries(nirqoba_phase5_benchmark PRIVATE nirqoba_runtime_debug)
