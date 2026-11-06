cmake_minimum_required(VERSION 3.25)
project(MoTrackPrerequisites LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "MoTrack requires Clang or Apple Clang.")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Xml LinguistTools)
include(../../cmake/MoTrackFFmpeg.cmake)
include(../../cmake/MoTrackArchive.cmake)
message(STATUS "Qt ${Qt6_VERSION} with Widgets, XML, and Linguist Tools")
add_executable(motrack_prereqs probe.cpp)
target_compile_features(motrack_prereqs PRIVATE cxx_std_23)
target_link_libraries(motrack_prereqs PRIVATE Qt6::Widgets Qt6::Xml motrack_ffmpeg motrack_archive)
