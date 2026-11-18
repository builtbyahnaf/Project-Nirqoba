# The collector benchmark links the development runtime, the same library the
# unit tests exercise, so the pauses it reports are the ones the tests audit.
add_executable(nirqoba_phase7_benchmark gc_collection_benchmark.cpp)

set_property(TARGET nirqoba_phase7_benchmark PROPERTY CXX_STANDARD 20)
set_property(TARGET nirqoba_phase7_benchmark PROPERTY CXX_STANDARD_REQUIRED ON)
nirqoba_set_target_warnings(nirqoba_phase7_benchmark)

target_link_libraries(nirqoba_phase7_benchmark PRIVATE nirqoba_runtime_debug)
