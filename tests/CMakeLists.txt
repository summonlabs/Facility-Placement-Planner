# The test suite.
#
# One executable holds every case, built against the static library so that cases
# can reach the internal headers as well as the public ones. The multiprocess
# helper is a second executable in the same directory, which is how the
# multiprocess cases find it: they look beside the running test binary rather than
# at a path baked into a source file.

set(FPP_TEST_SOURCES
    test_adversarial.cpp
    test_concurrency.cpp
    test_document.cpp
    test_durability.cpp
    test_harness.cpp
    test_lifecycle.cpp
    test_main.cpp
    test_multiprocess.cpp
    test_planning.cpp
    test_reference.cpp
    test_support.cpp
    test_values.cpp)

# The CLI is exercised as a real process by the suite, so it is placed beside the
# test executable. Only included when the CLI is built at all.
if(FPP_BUILD_CLI)
    list(APPEND FPP_TEST_SOURCES test_cli.cpp)
endif()

add_executable(fpp_tests ${FPP_TEST_SOURCES})
target_include_directories(fpp_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(fpp_tests
    PRIVATE
        FacilityPlacementPlannerStatic
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

add_executable(fpp_multiprocess_helper helper/fpp_multiprocess_helper.cpp)
target_link_libraries(fpp_multiprocess_helper
    PRIVATE
        FacilityPlacementPlannerStatic
        FacilityPlacementPlanner::warnings
        FacilityPlacementPlanner::sanitizer)

fpp_place_library(fpp_tests)
fpp_place_library(fpp_multiprocess_helper)

if(FPP_BUILD_CLI)
    add_custom_command(TARGET fpp_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:fpp_cli>
            $<TARGET_FILE_DIR:fpp_tests>
        COMMENT "Placing the CLI beside the test executable")
endif()

add_test(NAME facility_placement_planner.suite COMMAND fpp_tests)
