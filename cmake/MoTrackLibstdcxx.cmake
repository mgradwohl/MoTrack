# Include before project(). On Linux, MoTrack builds with LLVM Clang against libstdc++, the standard
# library Qt's Linux binaries use (never libc++: std::string and containers cross the Qt API).
# tools/dev-env.sh and CI set MOTRACK_GCC_INSTALL_DIR to GCC 14's installation, whose libstdc++ has
# more of C++23 than Ubuntu 24.04's default GCC 13 (CONTRIBUTING.md#c-standard-libraries). Empty
# leaves the choice to Clang (its newest GCC installation). Every compile and link of the build,
# CMake's own checks included, gets --gcc-install-dir through CMAKE_<LANG>_FLAGS_INIT.
set(MOTRACK_GCC_INSTALL_DIR "$ENV{MOTRACK_GCC_INSTALL_DIR}" CACHE PATH
    "Linux Clang: the GCC installation whose libstdc++ to use (tools/dev-env.sh selects GCC 14's)")
if(MOTRACK_GCC_INSTALL_DIR)
    if(NOT IS_DIRECTORY "${MOTRACK_GCC_INSTALL_DIR}")
        message(FATAL_ERROR "MOTRACK_GCC_INSTALL_DIR is not a directory: ${MOTRACK_GCC_INSTALL_DIR}")
    endif()
    set(motrack_gcc_flag "--gcc-install-dir=${MOTRACK_GCC_INSTALL_DIR}")
    string(APPEND CMAKE_C_FLAGS_INIT " ${motrack_gcc_flag}")
    string(APPEND CMAKE_CXX_FLAGS_INIT " ${motrack_gcc_flag}")
endif()

# Call after project(): a build tree configured before the selection changed keeps its old flags.
function(motrack_check_libstdcxx)
    if(MOTRACK_GCC_INSTALL_DIR)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "MOTRACK_GCC_INSTALL_DIR applies to Linux builds only; clear it.")
        endif()
        string(FIND " ${CMAKE_CXX_FLAGS} " " --gcc-install-dir=${MOTRACK_GCC_INSTALL_DIR} " position)
        if(position EQUAL -1)
            message(FATAL_ERROR "This build tree's CMAKE_CXX_FLAGS (${CMAKE_CXX_FLAGS}) predate "
                "MOTRACK_GCC_INSTALL_DIR=${MOTRACK_GCC_INSTALL_DIR}. Reconfigure it with cmake --fresh.")
        endif()
    endif()
endfunction()
