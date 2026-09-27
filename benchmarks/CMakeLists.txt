# The benchmarks. Built as a normal target; the small run is registered as a test
# so that a benchmark that has stopped working is noticed, and the larger scales
# are left to whoever wants to wait for them.

add_executable(fpp_benchmark benchmark_planner.cpp)
set_target_properties(fpp_benchmark PROPERTIES OUTPUT_NAME fpp-benchmark)
target_link_libraries(fpp_benchmark
    PRIVATE
        FacilityPlacementPlanner
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

fpp_place_library(fpp_benchmark)

add_test(NAME facility_placement_planner.benchmark_small
    COMMAND fpp_benchmark --scale=small --runs=3)
add_test(NAME facility_placement_planner.benchmark_medium
    COMMAND fpp_benchmark --scale=medium --runs=3)
