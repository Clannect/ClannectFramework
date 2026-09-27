# Benchmarks (spec §7). Always built with optimisation-relevant flags from the
# active configuration; meaningful numbers come from the release preset.
#
# The C++ runtime is linked statically into the benchmark so the replaced
# operator new also sees allocations made inside the standard library.

add_executable(cfw-bench src/CoreBench.cpp src/AllocationCounter.cpp)
target_include_directories(cfw-bench PRIVATE include)
target_link_libraries(cfw-bench PRIVATE cfw-core cfw-io)
cfw_configure_target(cfw-bench)
if(MINGW)
    target_link_options(cfw-bench PRIVATE -static-libgcc -static-libstdc++)
endif()

if(CFW_BUILD_TESTS)
    # Allocation budgets are deterministic, so they gate every build. Timing
    # is compared only against a same-machine baseline (--baseline).
    add_test(NAME BenchAllocationBudgets COMMAND cfw-bench)
    set_tests_properties(BenchAllocationBudgets PROPERTIES LABELS bench TIMEOUT 120)
endif()

# Networking: 200 WebSocket connections on one loop (spec §4.3, §7).
add_executable(cfw-bench-net src/NetBench.cpp)
target_link_libraries(cfw-bench-net PRIVATE cfw-core cfw-net)
cfw_configure_target(cfw-bench-net)
