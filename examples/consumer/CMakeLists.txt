cmake_minimum_required(VERSION 3.20)

# A downstream consumer of the installed Facility Placement Planner package.
#
# This project is deliberately separate from the library's own build. It finds the
# package the way any consumer would - through find_package and the installed
# configuration - and links the exported namespaced target. Building it from the
# library's build tree would not exercise the exported package at all.
#
#   cmake -S examples/consumer -B <dir> -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <dir> --config Release
#   <dir>/consumer

project(FacilityPlacementPlannerConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(FacilityPlacementPlanner 1.0 REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE FacilityPlacementPlanner::FacilityPlacementPlanner)

# A shared build installs the library into the prefix's `bin` directory, which is
# not on the loader's search path for an executable sitting in its own build
# directory. Placing a copy beside the consumer is what makes it runnable straight
# after a build; a real deployment would install it instead.
if(WIN32)
    add_custom_command(TARGET consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:FacilityPlacementPlanner::FacilityPlacementPlanner>
            $<TARGET_FILE_DIR:consumer>
        COMMENT "Placing the facility placement planner library beside the consumer")
endif()

if(MSVC)
    target_compile_options(consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic)
endif()
