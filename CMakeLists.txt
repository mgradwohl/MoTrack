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
option(MOTRACK_STAGE_DESKTOP "Enable a local install with desktop runtime dependencies" OFF)
option(MOTRACK_BUILD_BENCHMARKS "Build the legacy import benchmark" OFF)
option(MOTRACK_THIN_LTO "Enable Clang ThinLTO for MoTrack" OFF)
option(MOTRACK_COMPATIBLE "Use the baseline desktop CPU instruction set" OFF)
option(MOTRACK_PGO_GENERATE "Instrument MoTrack for profile-guided optimization" OFF)
set(MOTRACK_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM profile to use for optimization")
if(MOTRACK_PGO_PROFILE AND NOT EXISTS "${MOTRACK_PGO_PROFILE}")
    message(FATAL_ERROR "Generate the PGO profile first: ${MOTRACK_PGO_PROFILE}")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Xml LinguistTools Concurrent)
include(cmake/MoTrackFFmpeg.cmake)
include(cmake/MoTrackArchive.cmake)

add_library(motrack_project STATIC src/document_session.cpp src/legacy_project.cpp src/local_media_path.cpp src/paired_save.cpp src/measurements.cpp src/track_edits.cpp src/track_identity.cpp
    src/tracker_archive.cpp src/tracker_archive_writer.cpp src/tracker_references.cpp src/tracker_rename.cpp src/tracker_writer.cpp src/workspace_document.cpp src/workspace_storage.cpp)
target_include_directories(motrack_project PUBLIC src)
target_link_libraries(motrack_project PUBLIC Qt6::Core PRIVATE Qt6::Xml motrack_archive)

qt_add_executable(motrack src/main.cpp)
add_library(motrack_workspace STATIC src/image_sequence.cpp src/main_window.cpp src/movie_worker.cpp src/position_plot.cpp
    src/video_canvas.cpp)
set_target_properties(motrack_workspace PROPERTIES AUTOMOC ON)
add_library(motrack_media STATIC src/template_tracker.cpp src/video_reader.cpp)
target_include_directories(motrack_media PUBLIC src)
target_link_libraries(motrack_media PUBLIC Qt6::Gui PRIVATE motrack_ffmpeg)
target_link_libraries(motrack_workspace PUBLIC motrack_project Qt6::Widgets PRIVATE motrack_media Qt6::Concurrent)
target_link_libraries(motrack PRIVATE motrack_workspace)
qt_add_translations(motrack
    TS_FILES translations/motrack_de.ts translations/motrack_es.ts translations/motrack_fr.ts
    SOURCES src/main_window.cpp src/position_plot.cpp
    RESOURCE_PREFIX "/i18n"
)

if(MOTRACK_STAGE_DESKTOP)
    include(GNUInstallDirs)
    if(APPLE)
        # macdeployqt uses the SDK runpath to locate frameworks and rewrites it for the bundle.
        set_target_properties(motrack PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME MoTrack
            MACOSX_BUNDLE_GUI_IDENTIFIER com.github.mgradwohl.motrack
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            INSTALL_RPATH "@executable_path/../Frameworks;${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
    elseif(WIN32)
        file(GLOB ffmpeg_runtime "${MOTRACK_FFMPEG_ROOT}/bin/avcodec-*.dll"
            "${MOTRACK_FFMPEG_ROOT}/bin/avformat-*.dll"
            "${MOTRACK_FFMPEG_ROOT}/bin/avutil-*.dll"
            "${MOTRACK_FFMPEG_ROOT}/bin/swscale-*.dll")
        list(LENGTH ffmpeg_runtime ffmpeg_runtime_count)
        if(NOT ffmpeg_runtime_count EQUAL 4)
            message(FATAL_ERROR "Run MoTrack's setup scripts to install the four FFmpeg runtime DLLs.")
        endif()
        install(FILES ${ffmpeg_runtime} DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES "${MOTRACK_FFMPEG_ROOT}/bin/archive.dll" "${MOTRACK_FFMPEG_ROOT}/bin/z.dll"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(motrack PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
        qt_import_plugins(motrack INCLUDE Qt6::QXcbIntegrationPlugin Qt6::QOffscreenIntegrationPlugin)
    endif()
    install(TARGETS motrack BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(APPLE)
        foreach(locale IN ITEMS de es fr)
            install(FILES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_${locale}.qm"
                DESTINATION motrack.app/Contents/Resources/translations OPTIONAL)
        endforeach()
    endif()
    qt_generate_deploy_app_script(TARGET motrack OUTPUT_SCRIPT motrack_deploy_script)
    install(SCRIPT "${motrack_deploy_script}")
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/motrack)
endif()

set(motrack_targets motrack_project motrack_media motrack_workspace motrack)
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
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "Match the application CRT on Windows")
    set(INSTALL_GTEST OFF CACHE BOOL "Build tests without installing GoogleTest")
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(motrack_tests tests/legacy_project_test.cpp tests/measurements_test.cpp tests/track_edits_test.cpp
        tests/tracker_archive_test.cpp tests/tracker_archive_writer_test.cpp tests/tracker_references_test.cpp tests/tracker_writer_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_project GTest::gtest_main motrack_archive Qt6::Xml)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME legacy_project COMMAND motrack_tests)
    add_executable(motrack_corpus_probe tests/corpus_probe.cpp)
    target_link_libraries(motrack_corpus_probe PRIVATE motrack_project)
    add_executable(motrack_workspace_tests tests/workspace_test.cpp tests/workspace_document_test.cpp tests/document_session_test.cpp)
    target_link_libraries(motrack_workspace_tests PRIVATE motrack_workspace GTest::gtest Qt6::Test motrack_archive)
    add_dependencies(motrack_workspace_tests motrack)
    target_compile_definitions(motrack_workspace_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        MOTRACK_TRANSLATIONS="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME workspace COMMAND motrack_workspace_tests)
    set_tests_properties(workspace PROPERTIES TIMEOUT 60)
    add_executable(motrack_media_tests tests/template_tracker_test.cpp tests/video_reader_test.cpp)
    target_link_libraries(motrack_media_tests PRIVATE motrack_media GTest::gtest_main)
    target_compile_definitions(motrack_media_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME video_reader COMMAND motrack_media_tests)
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
            CONTENT "$<TARGET_FILE:motrack>\n$<TARGET_FILE:motrack_tests>\n$<TARGET_FILE:motrack_workspace_tests>\n$<TARGET_FILE:motrack_media_tests>\n")
    endif()
endif()
