cmake_minimum_required(VERSION 3.20)

project(FacilityPlacementPlanner
    VERSION 1.0.0
    DESCRIPTION "DCCP Tranche 2 deterministic facility placement planning and explanation authority"
    HOMEPAGE_URL "https://github.com/summonlabs/Facility-Placement-Planner"
    LANGUAGES CXX)

# The standard is set once, globally, so every target in every subdirectory and
# every consumer of this build tree compiles at the same language level. Setting
# it per target is easy to forget for a new target, and forgetting it silently
# downgrades the language level instead of failing.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FPP_IS_TOP_LEVEL ON)
else()
    set(FPP_IS_TOP_LEVEL OFF)
endif()

option(FPP_BUILD_SHARED "Build the facility placement planner library as a shared library" ON)
option(FPP_BUILD_TESTS "Build the facility placement planner test suite" ${FPP_IS_TOP_LEVEL})
option(FPP_BUILD_CLI "Build the facility-placement CLI" ON)
option(FPP_BUILD_EXAMPLES "Build the facility placement planner examples" ${FPP_IS_TOP_LEVEL})
option(FPP_BUILD_BENCHMARKS "Build the facility placement planner benchmarks" ${FPP_IS_TOP_LEVEL})
option(FPP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
set(FPP_SANITIZE "" CACHE STRING
    "Sanitizer instrumentation to apply to first-party targets (empty, 'address', or 'thread')")

if(NOT FPP_BUILD_SHARED)
    set(FPP_LIBRARY_TYPE STATIC)
else()
    set(FPP_LIBRARY_TYPE SHARED)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Shared warning / sanitizer interface
# ---------------------------------------------------------------------------

add_library(fpp_warnings INTERFACE)
add_library(FacilityPlacementPlanner::warnings ALIAS fpp_warnings)

if(MSVC)
    target_compile_options(fpp_warnings INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc)
    # C4251 warns that standard-library members of an exported class are not
    # themselves exported. This library's public surface is deliberately made of
    # value types with standard-library members, so the warning fires on almost
    # every public type without describing a defect: consumers are built with a
    # compatible toolchain and standard library, and no standard-library object is
    # ever constructed on one side of the boundary and destroyed on the other. The
    # suppression is applied once, here, rather than per class.
    target_compile_options(fpp_warnings INTERFACE /wd4251)
    if(FPP_WARNINGS_AS_ERRORS)
        target_compile_options(fpp_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(fpp_warnings INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(fpp_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2)
    if(FPP_WARNINGS_AS_ERRORS)
        target_compile_options(fpp_warnings INTERFACE -Werror)
    endif()
endif()

set(FPP_SANITIZER_FLAGS "")
if(FPP_SANITIZE STREQUAL "address")
    if(MSVC)
        list(APPEND FPP_SANITIZER_FLAGS /fsanitize=address)
        set(FPP_MSVC_ASAN_RUNTIME ON)
    else()
        list(APPEND FPP_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    endif()
elseif(FPP_SANITIZE STREQUAL "thread")
    if(MSVC)
        message(FATAL_ERROR "FPP_SANITIZE=thread is not supported by MSVC")
    endif()
    list(APPEND FPP_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT FPP_SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unsupported FPP_SANITIZE value: ${FPP_SANITIZE}")
endif()

add_library(fpp_sanitizer INTERFACE)
add_library(FacilityPlacementPlanner::sanitizer ALIAS fpp_sanitizer)
if(FPP_SANITIZER_FLAGS)
    target_compile_options(fpp_sanitizer INTERFACE ${FPP_SANITIZER_FLAGS})
    if(MSVC)
        # MSVC records the sanitizer runtime in the instrumented objects' metadata and
        # treats /fsanitize=address as a compile option only, so passing it to the linker
        # makes the linker warn about an option it does not recognise. Incremental linking
        # is incompatible with that metadata, so it is turned off here rather than left for
        # the linker to refuse with a warning.
        target_link_options(fpp_sanitizer INTERFACE /INCREMENTAL:NO)
    else()
        target_link_options(fpp_sanitizer INTERFACE ${FPP_SANITIZER_FLAGS})
    endif()
endif()

# ---------------------------------------------------------------------------
# Core object library (all first-party translation units)
# ---------------------------------------------------------------------------

set(FPP_PUBLIC_HEADERS
    include/facility_placement_planner/candidate.hpp
    include/facility_placement_planner/core.hpp
    include/facility_placement_planner/engine.hpp
    include/facility_placement_planner/error.hpp
    include/facility_placement_planner/evidence.hpp
    include/facility_placement_planner/limits.hpp
    include/facility_placement_planner/persistence.hpp
    include/facility_placement_planner/plan.hpp
    include/facility_placement_planner/policy.hpp
    include/facility_placement_planner/request.hpp
    include/facility_placement_planner/requirements.hpp
    include/facility_placement_planner/revalidate.hpp
    include/facility_placement_planner/snapshot.hpp
    include/facility_placement_planner/strong_types.hpp
    include/facility_placement_planner/text.hpp
    include/facility_placement_planner/version.hpp)

set(FPP_INTERNAL_HEADERS
    src/checked.hpp
    src/crc32.hpp
    src/fs_atomic.hpp
    src/fingerprint.hpp
    src/store_format.hpp)

set(FPP_SOURCES
    src/candidate.cpp
    src/core.cpp
    src/engine_admissibility.cpp
    src/engine_ranking.cpp
    src/engine_search.cpp
    src/error.cpp
    src/evidence.cpp
    src/fs_atomic.cpp
    src/limits.cpp
    src/persistence.cpp
    src/plan.cpp
    src/policy.cpp
    src/request.cpp
    src/requirements.cpp
    src/revalidate.cpp
    src/snapshot.cpp
    src/store_format.cpp
    src/strong_types.cpp
    src/text_parse.cpp
    src/text_render.cpp)

add_library(fpp_objects OBJECT ${FPP_SOURCES} ${FPP_INTERNAL_HEADERS})
set_target_properties(fpp_objects PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(fpp_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fpp_objects
    PRIVATE FacilityPlacementPlanner::warnings FacilityPlacementPlanner::sanitizer)

if(FPP_MSVC_ASAN_RUNTIME)
    target_compile_options(fpp_objects PRIVATE /fsanitize=address)
endif()

if(WIN32 AND FPP_BUILD_SHARED)
    # The symbols are compiled into the object library, so the export and import
    # declarations must reach that target rather than only the library target that
    # collects its objects. Without FPP_BUILDING_LIBRARY here, every declaration
    # would carry an empty annotation, no symbol would be exported, and the link
    # would fail for want of an import library.
    target_compile_definitions(fpp_objects
        PRIVATE FPP_BUILDING_LIBRARY
        PUBLIC FPP_SHARED)
endif()

# ---------------------------------------------------------------------------
# Public library target
# ---------------------------------------------------------------------------

add_library(FacilityPlacementPlanner ${FPP_LIBRARY_TYPE}
    $<TARGET_OBJECTS:fpp_objects>)
add_library(FacilityPlacementPlanner::FacilityPlacementPlanner ALIAS FacilityPlacementPlanner)

set_target_properties(FacilityPlacementPlanner PROPERTIES
    OUTPUT_NAME facility_placement_planner
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(FacilityPlacementPlanner
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(FacilityPlacementPlanner
    PRIVATE FacilityPlacementPlanner::sanitizer FacilityPlacementPlanner::warnings)

if(FPP_MSVC_ASAN_RUNTIME)
    target_compile_options(FacilityPlacementPlanner PRIVATE /fsanitize=address)
endif()

# Static build of the same objects, used by the test suite and the benchmarks. It
# exists so tests can reach internal accessors (for example the decoded durable
# state a recovery produced) without exporting implementation detail from the
# shared library that consumers see. It is never installed and never exported.
add_library(FacilityPlacementPlannerStatic STATIC $<TARGET_OBJECTS:fpp_objects>)
target_include_directories(FacilityPlacementPlannerStatic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------

install(TARGETS FacilityPlacementPlanner
    EXPORT FacilityPlacementPlannerTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/facility_placement_planner
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

set(FPP_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/FacilityPlacementPlanner)

install(EXPORT FacilityPlacementPlannerTargets
    FILE FacilityPlacementPlannerTargets.cmake
    NAMESPACE FacilityPlacementPlanner::
    DESTINATION ${FPP_CMAKE_DIR})

configure_package_config_file(
    cmake/FacilityPlacementPlannerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityPlacementPlannerConfig.cmake
    INSTALL_DESTINATION ${FPP_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityPlacementPlannerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityPlacementPlannerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FacilityPlacementPlannerConfigVersion.cmake
    DESTINATION ${FPP_CMAKE_DIR})

install(FILES README.md LICENSE NOTICE CONTRIBUTING.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# CLI, examples, tests, benchmarks
# ---------------------------------------------------------------------------

# A multi-configuration generator puts the shared library in a per-configuration
# directory, and that directory is not on the loader's search path for an executable
# sitting in its own directory. First-party executables that link the shared library
# therefore get a copy placed beside them, which is what keeps the CLI and the example
# tests runnable straight from the build tree.
function(fpp_place_library target)
    if(FPP_BUILD_SHARED)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:FacilityPlacementPlanner>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Placing the facility placement planner library beside ${target}")
    endif()
endfunction()

# CTest has to be enabled before any directory that registers a test, and the
# examples register tests as well as the test suite does. A directory added before
# enable_testing() has its add_test() calls dropped silently, so the ordering here
# is what keeps the examples inside `ctest`.
if(FPP_BUILD_TESTS OR FPP_BUILD_EXAMPLES)
    enable_testing()
endif()

if(FPP_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(FPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(FPP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
