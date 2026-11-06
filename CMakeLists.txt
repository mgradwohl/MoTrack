cmake_minimum_required(VERSION 3.25)
project(MoTrack VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "MoTrack requires Clang or Apple Clang.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOTRACK_COVERAGE "Instrument MoTrack's production targets for LLVM coverage" OFF)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Xml LinguistTools)

add_library(motrack_project STATIC src/legacy_project.cpp)
target_include_directories(motrack_project PUBLIC src)
target_link_libraries(motrack_project PUBLIC Qt6::Core PRIVATE Qt6::Xml)

add_executable(motrack src/main.cpp)
target_link_libraries(motrack PRIVATE motrack_project Qt6::Widgets)
qt_add_translations(motrack
    TS_FILES translations/motrack_es.ts
    RESOURCE_PREFIX "/i18n"
)

foreach(target IN ITEMS motrack_project motrack)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(MOTRACK_COVERAGE)
        target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${target} PUBLIC -fprofile-instr-generate)
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
    add_test(NAME desktop_smoke COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
    add_test(NAME desktop_invalid_project COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing.trk")
    set_tests_properties(desktop_smoke desktop_invalid_project PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(desktop_invalid_project PROPERTIES WILL_FAIL TRUE)

    if(MOTRACK_COVERAGE)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/coverage-objects.txt"
            CONTENT "$<TARGET_FILE:motrack>\n$<TARGET_FILE:motrack_tests>\n")
    endif()
endif()
