cmake_minimum_required(VERSION 3.25)
include(cmake/MoTrackLibstdcxx.cmake)
project(MoTrack VERSION 0.1.0 LANGUAGES CXX)
motrack_check_libstdcxx()

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

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets LinguistTools Concurrent Multimedia)
include(cmake/MoTrackFFmpeg.cmake)
include(cmake/MoTrackArchive.cmake)
include(cmake/MoTrackTextEncoding.cmake)

# Qt-free science kernels. The expression engine ports OSP's SuryonoParser (docs/expression-engine.md).
add_library(motrack_expression STATIC src/science/decimal_conversion.cpp src/science/expression.cpp)
target_include_directories(motrack_expression PUBLIC src)

# Lossless OSP XMLControl reader/renderer; no Qt, media or OS dependencies.
add_library(motrack_osp STATIC src/osp/java_text.cpp src/osp/render.cpp src/osp/source.cpp src/osp/value.cpp)
target_include_directories(motrack_osp PUBLIC src)

# Qt-free dependency evaluation core for dependent Tracker objects (docs/dependency-graph.md).
add_library(motrack_dependencies STATIC src/model/dependencies/dependency_graph.cpp src/model/dependencies/name_binding.cpp)
target_include_directories(motrack_dependencies PUBLIC src)
# Digital Library collections, libraries and ComPADRE responses on motrack_osp; no Qt, network or file I/O.
add_library(motrack_library STATIC src/library/collection.cpp src/library/compadre.cpp src/library/java_string.cpp src/library/library.cpp
    src/library/osp_values.cpp src/library/paths.cpp src/library/resource.cpp src/library/xml_dom.cpp)
target_include_directories(motrack_library PUBLIC src)
target_link_libraries(motrack_library PUBLIC motrack_osp)

# The typed Tracker document (docs/tracker-document.md): typed loaders, source edits, families, descriptors, identities
# and the unit and name codecs. Public headers are standard-only; Qt Core is linked privately for the codecs' QChar
# classification only.
add_library(motrack_model STATIC src/model/codecs.cpp src/model/descriptors.cpp src/model/families.cpp src/model/source_edits.cpp
    src/model/tracker_document.cpp src/model/tracker_loaders.cpp)
target_include_directories(motrack_model PUBLIC src)
target_link_libraries(motrack_model PUBLIC motrack_osp motrack_dependencies PRIVATE Qt6::Core)

add_library(motrack_project STATIC src/archive_group.cpp src/archive_group_session.cpp src/document_session.cpp src/legacy_project.cpp src/legacy_view.cpp src/local_media_path.cpp src/paired_save.cpp src/save_state.cpp src/save_observation.cpp src/measurements.cpp src/track_edits.cpp src/track_identity.cpp
    src/tracker_archive.cpp src/tracker_archive_writer.cpp src/tracker_compose.cpp src/tracker_references.cpp src/tracker_rename.cpp src/tracker_tabset.cpp src/tracker_writer.cpp src/workspace_document.cpp src/workspace_session.cpp src/workspace_storage.cpp)
target_include_directories(motrack_project PUBLIC src)
# Tracker import/export reads and patches XML through motrack_osp only; QtXml is a test diagnostic dependency.
target_link_libraries(motrack_project PUBLIC Qt6::Core motrack_osp motrack_model PRIVATE motrack_archive motrack_motion)

# Package validation decodes staged images/movies without adding codecs to the project model.
add_library(motrack_image_package STATIC src/tracker_image_package.cpp)
target_include_directories(motrack_image_package PUBLIC src)
target_link_libraries(motrack_image_package PUBLIC motrack_project PRIVATE Qt6::Gui motrack_archive motrack_media)

# Java evaluates a*b+c with two roundings; Clang may otherwise fuse it where FMA exists (arm64).
# Science kernels checked against OSP/Tracker, and code that computes their callbacks (such as
# model rate functions), compile with these options so that results match Java bit for bit.
set(MOTRACK_JAVA_FP_OPTIONS -ffp-contract=off)

# Qt-free ports of the OSP ODE solvers used by Tracker's dynamic models (docs/ode-solvers.md).
add_library(motrack_ode STATIC src/science/ode/ode_solver.cpp src/science/ode/ode_fixed_step.cpp src/science/ode/ode_adaptive.cpp)
target_include_directories(motrack_ode PUBLIC src)
target_compile_options(motrack_ode PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
target_compile_options(motrack_expression PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})

# Qt-free ports of Tracker's point-mass derivatives and motion filters (docs/motion-kernels.md).
add_library(motrack_motion STATIC src/science/motion/motion_derivatives.cpp src/science/motion/bounce_derivatives.cpp
    src/science/motion/motion_filters.cpp src/science/motion/numerical_derivative.cpp src/science/motion/timestamp_precision.cpp)
target_include_directories(motrack_motion PUBLIC src)
target_compile_options(motrack_motion PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
# Java Math semantics shared by the kernels checked against OSP (src/science/java_math.hpp; #263).
add_library(motrack_java_math STATIC src/science/java_math.cpp)
target_include_directories(motrack_java_math PUBLIC src)
target_compile_options(motrack_java_math PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
target_link_libraries(motrack_expression PUBLIC motrack_java_math)
target_link_libraries(motrack_ode PRIVATE motrack_java_math)

# Qt-free ports of OSP Data Tool statistics, measuring tools and Fourier spectra (docs/data-analysis-kernels.md).
add_library(motrack_analysis STATIC src/science/analysis/data_columns.cpp src/science/analysis/plot_measurements.cpp
    src/science/analysis/fourier_spectrum.cpp src/science/analysis/fft.cpp)
target_include_directories(motrack_analysis PUBLIC src)
target_compile_options(motrack_analysis PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
target_link_libraries(motrack_analysis PRIVATE motrack_java_math)

qt_add_executable(motrack src/main.cpp)
if(WIN32)
    # The UTF-8 process code page (activeCodePage); the test binaries deliberately have no manifest.
    target_sources(motrack PRIVATE src/motrack.manifest)
endif()
# The process locale setup every executable calls after constructing its Q*Application.
add_library(motrack_locale STATIC src/app_locale.cpp)
target_include_directories(motrack_locale PUBLIC src)
target_link_libraries(motrack_locale PUBLIC Qt6::Core)
if(APPLE)
    # macOS refuses camera access to an app without a usage string. Bundles carry it in Info.plist;
    # developer builds (no bundle) embed the same keys in the executable.
    set(MOTRACK_BUNDLE_IDENTIFIER com.github.mgradwohl.motrack)
    set(MOTRACK_CAMERA_USAGE "MoTrack uses the camera only when you choose Capture from camera, to record a clip for motion analysis.")
    if(NOT MOTRACK_STAGE_DESKTOP)
        configure_file(cmake/MoTrackEmbeddedInfo.plist.in "${CMAKE_CURRENT_BINARY_DIR}/motrack-Info.plist" @ONLY)
        target_link_options(motrack PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/motrack-Info.plist")
        set_property(TARGET motrack APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/motrack-Info.plist")
        # Qt's Darwin permission plugins are static, and Qt links them only into targets with a
        # MACOSX_BUNDLE_INFO_PLIST (_qt_internal_darwin_permission_finalizer). Without the camera one,
        # checkPermission reports Denied, so a developer build links it explicitly.
        if(NOT TARGET Qt6::QDarwinCameraPermissionPlugin)
            message(FATAL_ERROR "Qt's QDarwinCameraPermissionPlugin is missing; camera capture needs it.")
        endif()
        qt_import_plugins(motrack INCLUDE Qt6::QDarwinCameraPermissionPlugin)
    endif()
endif()
add_library(motrack_workspace STATIC src/image_sequence.cpp src/main_window.cpp src/movie_worker.cpp src/position_plot.cpp
    src/video_canvas.cpp src/workspace_services.cpp src/workspace_context.cpp src/bound_workspace.cpp src/workspace_window.cpp src/ui_translations.cpp
    src/desktop_launch.cpp)
set_target_properties(motrack_workspace PROPERTIES AUTOMOC ON)
add_library(motrack_media STATIC src/capture_encoder.cpp src/template_tracker.cpp src/video_reader.cpp)
target_include_directories(motrack_media PUBLIC src)
# The index infers the timestamps' precision with the Qt-free motion kernels (#335).
target_link_libraries(motrack_media PUBLIC Qt6::Gui PRIVATE motrack_ffmpeg motrack_motion)

# Qt-free port of the Data Tool curve fitter and its minimizers (docs/curve-fitting.md).
add_library(motrack_fit STATIC src/science/fit/curve_fit.cpp src/science/fit/built_in_fits.cpp src/science/fit/fit_format.cpp
    src/science/fit/minimizers.cpp)
target_include_directories(motrack_fit PUBLIC src)
target_link_libraries(motrack_fit PRIVATE motrack_java_math)
target_compile_options(motrack_fit PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})

# Input adaptation may probe/index media; keep Qt Gui and FFmpeg outside the document model.
add_library(motrack_input STATIC src/workspace_input.cpp)
target_include_directories(motrack_input PUBLIC src)
target_link_libraries(motrack_input PUBLIC motrack_project PRIVATE motrack_media)

# Qt-free RGB-region and line-profile sampling kernels (docs/image-sampling.md).
add_library(motrack_sampling STATIC src/science/sampling/argb_image.cpp src/science/sampling/java_path.cpp
    src/science/sampling/line_profile.cpp src/science/sampling/rgb_region.cpp)
target_include_directories(motrack_sampling PUBLIC src)
target_compile_options(motrack_sampling PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
# Qt-free ports of Tracker's video filters (docs/video-filters.md).
add_library(motrack_filters STATIC src/science/filters/filter_image.cpp src/science/filters/java_arithmetic.cpp src/science/filters/video_filter.cpp
    src/science/filters/point_filters.cpp src/science/filters/temporal_filters.cpp src/science/filters/geometric_filters.cpp)
target_include_directories(motrack_filters PUBLIC src)
target_compile_options(motrack_filters PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})

# Camera capture: Qt Multimedia supplies cameras and frames; MoTrack's FFmpeg (motrack_media) records them.
add_library(motrack_capture STATIC src/camera_source.cpp src/capture_dialog.cpp src/capture_pipeline.cpp)
set_target_properties(motrack_capture PROPERTIES AUTOMOC ON)
target_include_directories(motrack_capture PUBLIC src)
target_link_libraries(motrack_capture PUBLIC Qt6::Widgets Qt6::Multimedia PRIVATE motrack_media Qt6::Concurrent)
target_link_libraries(motrack_workspace PUBLIC motrack_project motrack_input motrack_capture Qt6::Widgets PRIVATE motrack_media motrack_image_package Qt6::Concurrent)
target_link_libraries(motrack PRIVATE motrack_workspace motrack_locale)
# One catalog per non-English locale in Tracker.getLocales(); tools/translations.py check verifies the list.
set(motrack_ts_files
    translations/motrack_ar.ts translations/motrack_ca.ts translations/motrack_cs.ts translations/motrack_da.ts
    translations/motrack_de.ts translations/motrack_el.ts translations/motrack_es.ts translations/motrack_fi.ts
    translations/motrack_fr.ts translations/motrack_he.ts translations/motrack_hu.ts translations/motrack_id.ts
    translations/motrack_it.ts translations/motrack_ja.ts translations/motrack_ko.ts translations/motrack_lv.ts
    translations/motrack_ms.ts translations/motrack_nl.ts translations/motrack_pl.ts translations/motrack_pt.ts
    translations/motrack_pt_BR.ts translations/motrack_ru.ts translations/motrack_sk.ts translations/motrack_sl.ts
    translations/motrack_sv.ts translations/motrack_th.ts translations/motrack_tr.ts translations/motrack_uk.ts
    translations/motrack_vi.ts translations/motrack_zh_CN.ts translations/motrack_zh_TW.ts)
# English plural forms only; shipping it also keeps an English-first language list from selecting a later language.
qt_add_translations(motrack
    TS_FILES ${motrack_ts_files}
    PLURALS_TS_FILE translations/motrack_en.ts
    SOURCES src/main_window.cpp src/bound_workspace.cpp src/workspace_window.cpp src/position_plot.cpp src/tracker_image_package.cpp
            src/tracker_archive.cpp src/workspace_input.cpp
            src/camera_source.cpp src/capture_dialog.cpp src/capture_pipeline.cpp
    RESOURCE_PREFIX "/i18n"
)

if(MOTRACK_STAGE_DESKTOP)
    include(GNUInstallDirs)
    if(APPLE)
        # macdeployqt uses the SDK runpath to locate frameworks and rewrites it for the bundle.
        # The bundle's Info.plist declares the project types Finder, Open With and the Dock may send (#68).
        # LSMinimumSystemVersion is the deployment target; without one, Clang targets the SDK's version.
        set(MOTRACK_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        if(NOT MOTRACK_MACOS_MINIMUM)
            execute_process(COMMAND xcrun --sdk macosx --show-sdk-version OUTPUT_VARIABLE MOTRACK_MACOS_MINIMUM
                OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        endif()
        set_target_properties(motrack PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME MoTrack
            MACOSX_BUNDLE_GUI_IDENTIFIER "${MOTRACK_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoTrackInfo.plist.in"
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
        # Desktop entry and shared-mime-info types for per-user registration of this staged build (#68).
        # The entry names the installed binary, so it is written for the prefix chosen at install time.
        install(FILES desktop/linux/com.github.mgradwohl.motrack.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
        install(CODE "set(MOTRACK_DESKTOP_TEMPLATE [==[${CMAKE_CURRENT_SOURCE_DIR}/desktop/linux/com.github.mgradwohl.motrack.desktop.in]==])
            set(MOTRACK_DESKTOP_OUTPUT [==[${CMAKE_INSTALL_DATADIR}/applications/com.github.mgradwohl.motrack.desktop]==])
            set(MOTRACK_DESKTOP_BINARY [==[${CMAKE_INSTALL_BINDIR}/motrack]==])")
        install(SCRIPT cmake/MoTrackDesktopEntry.cmake)
    endif()
    install(TARGETS motrack BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(APPLE)
        # Qt ships catalogs for most, not all, MoTrack languages; UiTranslations loads the one matching MoTrack's.
        foreach(ts IN LISTS motrack_ts_files)
            string(REGEX REPLACE "^translations/motrack_(.+)[.]ts$" "\\1" locale "${ts}")
            install(FILES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_${locale}.qm"
                DESTINATION motrack.app/Contents/Resources/translations OPTIONAL)
        endforeach()
    endif()
    qt_generate_deploy_app_script(TARGET motrack OUTPUT_SCRIPT motrack_deploy_script)
    install(SCRIPT "${motrack_deploy_script}")
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/motrack)
endif()

set(motrack_targets motrack_dependencies motrack_osp motrack_model motrack_library motrack_expression motrack_ode motrack_fit motrack_analysis motrack_motion motrack_project motrack_input motrack_image_package motrack_media motrack_capture motrack_workspace motrack_locale motrack motrack_java_math)
if(MOTRACK_BUILD_BENCHMARKS)
    add_executable(motrack_import_benchmark benchmarks/import_benchmark.cpp)
    target_link_libraries(motrack_import_benchmark PRIVATE motrack_project)
    list(APPEND motrack_targets motrack_import_benchmark)
    # One AutoTracker step: typed edit, legacy view rebuild, TrackEdits mark and template match (tracker-document.md).
    add_executable(motrack_autotracker_benchmark benchmarks/autotracker_benchmark.cpp)
    target_link_libraries(motrack_autotracker_benchmark PRIVATE motrack_project motrack_media motrack_locale Qt6::Gui)
    list(APPEND motrack_targets motrack_autotracker_benchmark)
endif()

# Qt-free port of OSP's TemplateMatcher behind Tracker's AutoTracker (docs/template-matching.md).
add_library(motrack_matching STATIC src/science/matching/template_matcher.cpp src/science/matching/java_geometry.cpp
    src/science/matching/java_numerics.cpp src/science/matching/java_trigonometry.cpp)
target_include_directories(motrack_matching PUBLIC src)
target_compile_options(motrack_matching PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
list(APPEND motrack_targets motrack_matching)
list(APPEND motrack_targets motrack_sampling)
list(APPEND motrack_targets motrack_filters)

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
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test Xml)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "Match the application CRT on Windows")
    set(INSTALL_GTEST OFF CACHE BOOL "Build tests without installing GoogleTest")
    # SYSTEM keeps GoogleTest headers out of MoTrack's warnings and clang-tidy header filter.
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(motrack_tests tests/workspace_session_test.cpp tests/archive_group_session_test.cpp tests/archive_group_save_test.cpp tests/archive_group_test.cpp tests/paired_save_test.cpp tests/legacy_project_test.cpp tests/measurements_test.cpp tests/track_edits_test.cpp
        tests/tracker_archive_test.cpp tests/tracker_archive_writer_test.cpp tests/tracker_compose_test.cpp tests/tracker_image_package_test.cpp tests/tracker_movie_package_test.cpp tests/tracker_references_test.cpp tests/tracker_tabset_test.cpp tests/tracker_writer_test.cpp
        tests/science/decimal_conversion_test.cpp tests/science/expression_reference_test.cpp tests/science/expression_test.cpp tests/science/java_math_test.cpp
        tests/osp_java_text_test.cpp tests/osp_source_test.cpp tests/source_edits_test.cpp tests/osp_render_test.cpp tests/osp_corpus_test.cpp
        tests/science/ode_solver_test.cpp tests/science/data_analysis_test.cpp tests/science/fft_test.cpp tests/science/analysis_reference_test.cpp
        tests/model/dependency_graph_test.cpp tests/model/name_binding_test.cpp tests/model/tracker_document_test.cpp tests/model/legacy_view_parity_test.cpp
        tests/library_paths_test.cpp tests/library_collection_test.cpp tests/library_compadre_test.cpp tests/library_limits_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_osp motrack_library motrack_model motrack_project motrack_dependencies motrack_java_math motrack_expression motrack_ode motrack_analysis
        motrack_image_package GTest::gtest_main motrack_archive Qt6::Xml Qt6::Gui)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        MOTRACK_EXPRESSION_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/expression-reference.json"
        MOTRACK_CORPUS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/fixtures"
        MOTRACK_ODE_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/ode-reference.json"
        MOTRACK_ANALYSIS_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/analysis-reference.json")
    # The ODE rate functions and the analysis fit polynomials must round like the Java oracles'.
    set_source_files_properties(tests/science/ode_solver_test.cpp tests/science/analysis_reference_test.cpp
        PROPERTIES COMPILE_OPTIONS "${MOTRACK_JAVA_FP_OPTIONS}")
    target_sources(motrack_tests PRIVATE tests/science/video_filter_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_filters)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_FILTER_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/video-filter-reference.json")
    # The filter test computes Java-rounded values too (sinCos arguments, ramps).
    set_source_files_properties(tests/science/video_filter_test.cpp PROPERTIES COMPILE_OPTIONS "${MOTRACK_JAVA_FP_OPTIONS}")
    target_sources(motrack_tests PRIVATE tests/science/curve_fit_api_test.cpp tests/science/curve_fit_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_fit)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_FIT_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/fit-reference.json")
    # The fit models must round like the Java oracle's.
    set_source_files_properties(tests/science/curve_fit_test.cpp tests/science/curve_fit_api_test.cpp
        PROPERTIES COMPILE_OPTIONS "${MOTRACK_JAVA_FP_OPTIONS}")
    add_test(NAME legacy_project COMMAND motrack_tests)
    target_sources(motrack_tests PRIVATE tests/science/sampling_test.cpp tests/science/sampling_reference_test.cpp)
    target_link_libraries(motrack_tests PRIVATE motrack_sampling)
    target_compile_definitions(motrack_tests PRIVATE
        MOTRACK_SAMPLING_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/sampling-reference.json")
    # Independent expectations repeat the kernel's Java-ordered arithmetic (see motrack_sampling).
    set_source_files_properties(tests/science/sampling_test.cpp tests/science/sampling_reference_test.cpp
        PROPERTIES COMPILE_OPTIONS "${MOTRACK_JAVA_FP_OPTIONS}")
    # Motion kernels: FilterSelfTest, analytic expectations and the Java reference fixture.
    set(motrack_motion_tests tests/science/filter_self_test.cpp tests/science/motion_kernels_test.cpp tests/science/motion_reference_test.cpp
        tests/science/timestamp_precision_test.cpp)
    target_sources(motrack_tests PRIVATE ${motrack_motion_tests})
    target_link_libraries(motrack_tests PRIVATE motrack_motion)
    target_compile_definitions(motrack_tests PRIVATE MOTRACK_MOTION_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/motion-reference.json")
    # Test signals must round like the Java oracle's (see motrack_motion above).
    set_source_files_properties(${motrack_motion_tests} PROPERTIES COMPILE_OPTIONS "${MOTRACK_JAVA_FP_OPTIONS}")
    add_executable(motrack_corpus_probe tests/corpus_probe.cpp)
    add_dependencies(motrack_tests motrack_corpus_probe)
    target_compile_definitions(motrack_tests PRIVATE MOTRACK_CORPUS_PROBE="$<TARGET_FILE:motrack_corpus_probe>")
    target_link_libraries(motrack_corpus_probe PRIVATE motrack_osp motrack_library motrack_project motrack_input motrack_image_package motrack_media motrack_locale)
    add_executable(motrack_workspace_tests
        tests/desktop_launch_test.cpp
        tests/workspace_shell_test.cpp
        tests/workspace_tabset_test.cpp
        tests/workspace_input_test.cpp
        tests/bound_workspace_test.cpp
        tests/workspace_services_test.cpp
        tests/image_package_workspace_test.cpp
        tests/paired_workspace_test.cpp
        tests/workspace_test.cpp
        tests/workspace_document_test.cpp
        tests/document_session_test.cpp
        tests/ui_translations_test.cpp
        tests/camera_capture_test.cpp
        tests/non_ascii_round_trip_test.cpp)
    target_link_libraries(motrack_workspace_tests PRIVATE motrack_workspace motrack_locale GTest::gtest Qt6::Test motrack_archive)
    add_dependencies(motrack_workspace_tests motrack)
    target_compile_definitions(motrack_workspace_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        MOTRACK_TRANSLATIONS="${CMAKE_CURRENT_BINARY_DIR}"
        MOTRACK_TS_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/translations"
        MOTRACK_EXECUTABLE="$<TARGET_FILE:motrack>")
    # Keep every case while bounding UI saving, persistence and session groups independently.
    add_test(NAME workspace COMMAND motrack_workspace_tests
        "--gtest_filter=-PairedWorkspace.*:DocumentSession.*:WorkspaceDocument.*:ImagePackageWorkspace.*:WorkspaceShell.*:UiTranslations.*:CameraCapture.*:NonAsciiRoundTrip.*:WorkspaceLocale.*:DesktopLaunch.*:MenuOpen.*")
    # The shell's save/revert flows take seconds each and slow down most on loaded runners (#283).
    add_test(NAME workspace_shell COMMAND motrack_workspace_tests --gtest_filter=WorkspaceShell.*)
    add_test(NAME camera_capture COMMAND motrack_workspace_tests --gtest_filter=CameraCapture.*)
    # Command-line, drop and operating-system opening, including child launches of the real executable.
    add_test(NAME desktop_launch COMMAND motrack_workspace_tests --gtest_filter=DesktopLaunch.*)
    # Files chosen through the real File menu dialogs, in their own time budget.
    add_test(NAME menu_open COMMAND motrack_workspace_tests --gtest_filter=MenuOpen.*)
    add_test(NAME workspace_saving COMMAND motrack_workspace_tests
        "--gtest_filter=PairedWorkspace.*:ImagePackageWorkspace.*")
    # Non-ASCII paths and locale-dependent display, in their own time budget.
    add_test(NAME workspace_text COMMAND motrack_workspace_tests "--gtest_filter=NonAsciiRoundTrip.*:WorkspaceLocale.*")
    add_test(NAME workspace_document COMMAND motrack_workspace_tests --gtest_filter=WorkspaceDocument.*)
    add_test(NAME ui_translations COMMAND motrack_workspace_tests --gtest_filter=UiTranslations.*)
    # Complementary filters retain every session case once, including future additions.
    # Real-file journal matrices can exhaust a shared 60-second budget on slower CI storage (#137).
    add_test(NAME document_session COMMAND motrack_workspace_tests "--gtest_filter=DocumentSession.*-DocumentSession.*Journal*")
    add_test(NAME document_session_journals COMMAND motrack_workspace_tests "--gtest_filter=DocumentSession.*Journal*")
    set_tests_properties(workspace workspace_shell workspace_saving workspace_document ui_translations document_session
        document_session_journals camera_capture workspace_text desktop_launch menu_open PROPERTIES TIMEOUT 60)
    add_executable(motrack_media_tests tests/capture_encoder_test.cpp tests/template_tracker_test.cpp tests/video_frame_conversion_test.cpp
        tests/video_reader_test.cpp)
    # The tests encode synthetic movies and drive swscale with FFmpeg's own types.
    target_link_libraries(motrack_media_tests PRIVATE motrack_media motrack_ffmpeg GTest::gtest_main)
    target_compile_definitions(motrack_media_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME video_reader COMMAND motrack_media_tests)
    # The locale setup and comma-decimal formatting, in their own process: main() starts in a German or French
    # environment where one is installed, and the cases change the process's locales.
    add_executable(motrack_locale_tests tests/app_locale_test.cpp tests/locale_formats_test.cpp)
    target_link_libraries(motrack_locale_tests PRIVATE motrack_locale motrack_project GTest::gtest)
    target_compile_definitions(motrack_locale_tests PRIVATE
        MOTRACK_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME locale COMMAND motrack_locale_tests)
    # The matching kernel's tests read the Java oracle fixture with Qt Core's JSON support only.
    add_executable(motrack_matching_tests tests/science/template_matcher_test.cpp tests/science/template_matcher_reference_test.cpp)
    target_link_libraries(motrack_matching_tests PRIVATE motrack_matching GTest::gtest_main Qt6::Core)
    target_compile_definitions(motrack_matching_tests PRIVATE
        MOTRACK_MATCH_REFERENCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/science/match-reference.json")
    # The synthetic frame generator must round like tools/reference/MatchOracle.java.
    target_compile_options(motrack_matching_tests PRIVATE ${MOTRACK_JAVA_FP_OPTIONS})
    add_test(NAME template_matching COMMAND motrack_matching_tests)
    # Catalog validation: every Tracker locale, lupdate completeness, placeholders, plural forms and lrelease.
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME translation_catalogs COMMAND Python3::Interpreter -I -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/tools/translations.py"
        check --lupdate "$<TARGET_FILE:Qt6::lupdate>" --lrelease "$<TARGET_FILE:Qt6::lrelease>")
    add_test(NAME translation_tool COMMAND Python3::Interpreter -I -X utf8 -m unittest discover -s tests -p test_translations.py
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME desktop_smoke COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
    add_test(NAME desktop_invalid_project COMMAND motrack --smoke-test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/missing.trk")
    set_tests_properties(workspace workspace_shell workspace_saving workspace_document ui_translations document_session
        document_session_journals camera_capture workspace_text desktop_launch menu_open desktop_smoke desktop_invalid_project PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(desktop_invalid_project PROPERTIES WILL_FAIL TRUE)
    # The application's own locale setup and, on Windows, its manifest's UTF-8 code page: the CI runners' system
    # code page is 1252, so GetACP() is 65001 only when activeCodePage took effect. Any locale warning fails.
    add_test(NAME desktop_locale COMMAND motrack --smoke-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
    if(WIN32)
        set(motrack_expected_code_page "65001")
    else()
        set(motrack_expected_code_page "n/a")
    endif()
    set_tests_properties(desktop_locale PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;QT_LOGGING_RULES=motrack.locale.info=true;QT_MESSAGE_PATTERN=%{type} %{category}: %{message}"
        PASS_REGULAR_EXPRESSION "info motrack\\.locale: Locale: GetACP\\(\\)=${motrack_expected_code_page} .* LC_NUMERIC=\"C\" C\\+\\+ global=\"C\""
        FAIL_REGULAR_EXPRESSION "warning motrack\\.locale:")
    if(MOTRACK_BUILD_BENCHMARKS)
        add_test(NAME import_benchmark_smoke COMMAND motrack_import_benchmark
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/point-mass.trk")
        add_test(NAME autotracker_benchmark_smoke COMMAND motrack_autotracker_benchmark
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native/baseline.trk" 300)
    endif()

    # Windows loads Qt's and FFmpeg's DLLs from PATH. Prepend their directories for every test, so CTest also
    # works outside an activated developer shell: without them a test cannot start (STATUS_DLL_NOT_FOUND), and
    # an executable started directly stops at a modal loader dialog ("Qt6Guid.dll was not found"). Child
    # processes the tests start inherit the PATH.
    # Keep this block after the last add_test: it applies to the tests defined so far.
    if(WIN32)
        # Starts from a PATH with neither directory (ENVIRONMENT applies before ENVIRONMENT_MODIFICATION), so it
        # passes only if the modification below supplies both: the executable needs Qt's and FFmpeg's DLLs to load.
        file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32" motrack_system32)
        add_test(NAME test_runtime_path COMMAND motrack_workspace_tests --gtest_list_tests)
        set_tests_properties(test_runtime_path PROPERTIES
            ENVIRONMENT "PATH=${motrack_system32};QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
        get_property(motrack_all_tests DIRECTORY PROPERTY TESTS)
        set(motrack_test_path "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        if(MOTRACK_FFMPEG_ROOT)
            file(TO_CMAKE_PATH "${MOTRACK_FFMPEG_ROOT}/bin" motrack_ffmpeg_bin)
            list(APPEND motrack_test_path "PATH=path_list_prepend:${motrack_ffmpeg_bin}")
        endif()
        set_property(TEST ${motrack_all_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION ${motrack_test_path})
    endif()

    if(MOTRACK_COVERAGE)
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/coverage-objects.txt"
            CONTENT "$<TARGET_FILE:motrack>\n$<TARGET_FILE:motrack_tests>\n$<TARGET_FILE:motrack_workspace_tests>\n$<TARGET_FILE:motrack_media_tests>\n$<TARGET_FILE:motrack_matching_tests>\n$<TARGET_FILE:motrack_locale_tests>\n")
    endif()
endif()

# Every target this project defines compiles as UTF-8 (cmake/MoTrackTextEncoding.cmake), new ones included.
# Third-party targets (GoogleTest) live in their own directories and are not in this directory's list.
motrack_apply_text_encoding_to_directory("${CMAKE_CURRENT_SOURCE_DIR}")
