# The inspection and administration CLI.

add_executable(fpp_cli main.cpp)
set_target_properties(fpp_cli PROPERTIES OUTPUT_NAME fpp-cli)
target_link_libraries(fpp_cli
    PRIVATE
        FacilityPlacementPlanner
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

fpp_place_library(fpp_cli)

# The CLI is exercised as part of the suite: a version report that cannot run is a
# packaging defect, and it is cheaper to notice here than after an install.
add_test(NAME facility_placement_planner.cli_version COMMAND fpp_cli version)
