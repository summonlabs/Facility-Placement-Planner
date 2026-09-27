# The examples. Each one is registered as a test: an example that stops working is
# documentation that has started lying, and this is what catches it.

add_executable(fpp_example_basic_placement basic_placement.cpp)
target_link_libraries(fpp_example_basic_placement
    PRIVATE
        FacilityPlacementPlanner
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

add_executable(fpp_example_persistence_reopen persistence_reopen.cpp)
target_link_libraries(fpp_example_persistence_reopen
    PRIVATE
        FacilityPlacementPlanner
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

add_executable(fpp_example_rejection_explanations rejection_explanations.cpp)
target_link_libraries(fpp_example_rejection_explanations
    PRIVATE
        FacilityPlacementPlanner
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

fpp_place_library(fpp_example_basic_placement)
fpp_place_library(fpp_example_persistence_reopen)
fpp_place_library(fpp_example_rejection_explanations)

add_test(NAME facility_placement_planner.example_basic_placement COMMAND fpp_example_basic_placement)
add_test(NAME facility_placement_planner.example_persistence_reopen COMMAND fpp_example_persistence_reopen)
add_test(NAME facility_placement_planner.example_rejection_explanations
    COMMAND fpp_example_rejection_explanations)

# `consumer/` is deliberately not added as a subdirectory. It is a separate CMake
# project that consumes the installed package through find_package, so building it
# from this tree would defeat its purpose. README documents how to run it against
# an install prefix, and the release validation does exactly that.
