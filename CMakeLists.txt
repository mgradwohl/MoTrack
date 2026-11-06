cmake_minimum_required(VERSION 3.25)
project(MoTrack VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "MoTrack requires Clang or Apple Clang.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOTRACK_CCACHE "Use ccache when installed" ON)
if(MOTRACK_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(MOTRACK_CCACHE_PROGRAM ccache)
    if(MOTRACK_CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${MOTRACK_CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MOTRACK_CCACHE_PROGRAM}")
        message(STATUS "Using ccache: ${MOTRACK_CCACHE_PROGRAM}")
    else()
        message(STATUS "ccache was not found. Install it to speed up rebuilds.")
    endif()
endif()

option(MOTRACK_COVERAGE "Instrument MoTrack's production targets for LLVM coverage" OFF)
option(MOTRACK_BUILD_BENCHMARKS "Build the legacy import benchmark" OFF)
option(MOTRACK_THIN_LTO "Enable Clang ThinLTO for MoTrack" OFF)
option(MOTRACK_COMPATIBLE "Use the baseline desktop CPU instruction set" OFF)
option(MOTRACK_PGO_GENERATE "Instrument MoTrack for profile-guided optimization" OFF)
set(MOTRACK_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM profile to use for optimization")
if(MOTRACK_PGO_PROFILE AND NOT EXISTS "${MOTRACK_PGO_PROFILE}")
    message(FATAL_ERROR "Generate the PGO profile first: ${MOTRACK_PGO_PROFILE}")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Xml LinguistTools)

add_library(motrack_project STATIC src/legacy_project.cpp)
target_include_directories(motrack_project PUBLIC src)
target_link_libraries(motrack_project PUBLIC Qt6::Core PRIVATE Qt6::Xml)

add_executable(motrack src/main.cpp)
add_library(motrack_workspace STATIC src/image_sequence.cpp src/main_window.cpp)
target_link_libraries(motrack_workspace PUBLIC motrack_project Qt6::Widgets)
target_link_libraries(motrack PRIVATE motrack_workspace)
qt_add_translations(motrack
    TS_FILES translations/motrack_es.ts
    SOURCES src/main_window.cpp
    RESOURCE_PREFIX "/i18n"
)

set(motrack_targets motrack_project motrack_workspace motrack)
if(MOTRACK_BUILD_BENCHMARKS)
    add_executable(motrack_import_benchmark benchmarks/import_benchmark.cpp)
    target_link_libraries(motrack_import_benchmark PRIVATE motrack_project)
    list(APPEND motrack_targets motrack_import_benchmark)
endif()

foreach(target IN LISTS motrack_targets)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(MOTRACK_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} PUBLIC -fprofile-instr-generate)
    endif()
    if(MOTRACK_THIN_LTO)
        target_compile_options(${target} PRIVATE -flto=thin)
        target_link_options(${target} PUBLIC -flto=thin)
        if(NOT APPLE)
            target_link_options(${target} PUBLIC -fuse-ld=lld)
        endif()
    endif()
    if(MOTRACK_COMPATIBLE AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        target_compile_options(${target} PRIVATE -march=x86-64)
    endif()
    if(MOTRACK_PGO_GENERATE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate)
        target_link_options(${target} PUBLIC -fprofile-instr-generate)
    elseif(MOTRACK_PGO_PROFILE)
        target_compile_options(${target} PRIVATE "-fprofile-instr-use=${MOTRACK_PGO_PROFILE}")
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "Match the application CRT on Windows")
    set(INSTALL_GTEST OFF CACHE BOOL "Build tests without installing GoogleTest")
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(motrack_tests tests/legacy_project_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_project GTest::gtest_main)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME legacy_project COMMAND motrack_tests)
    add_executable(motrack_workspace_tests tests/workspace_test.cpp)
    target_link_libraries(motrack_workspace_tests PRIVATE motrack_workspace GTest::gtest)
    target_compile_definitions(motrack_workspace_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME workspace COMMAND motrack_workspace_tests)
    add_test(NAME desktop_smoke COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
    add_test(NAME desktop_invalid_project COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing.trk")
    set_tests_properties(workspace desktop_smoke desktop_invalid_project PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(desktop_invalid_project PROPERTIES WILL_FAIL TRUE)
    if(MOTRACK_BUILD_BENCHMARKS)
        add_test(NAME import_benchmark_smoke COMMAND motrack_import_benchmark
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
    endif()

    if(MOTRACK_COVERAGE)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/coverage-objects.txt"
            CONTENT "$<TARGET_FILE:motrack>\n$<TARGET_FILE:motrack_tests>\n$<TARGET_FILE:motrack_workspace_tests>\n")
    endif()
endif()
