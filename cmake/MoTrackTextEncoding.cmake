# Source and execution character sets for MoTrack's own targets (CONTRIBUTING.md, "Text encoding and locale").
# Third-party targets (GoogleTest, Qt, FFmpeg, libarchive) keep their own settings: never call this on them.

include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(MOTRACK_TEXT_ENCODING_FLAGS /utf-8)
else()
    set(MOTRACK_TEXT_ENCODING_FLAGS -finput-charset=UTF-8)
    check_cxx_compiler_flag(-fexec-charset=UTF-8 MOTRACK_HAS_FEXEC_CHARSET_UTF8)
    if(MOTRACK_HAS_FEXEC_CHARSET_UTF8)
        list(APPEND MOTRACK_TEXT_ENCODING_FLAGS -fexec-charset=UTF-8)
    endif()
endif()

# Sources are read as UTF-8 and narrow literals are UTF-8. On Windows, UNICODE and _UNICODE make an
# unsuffixed Win32 macro resolve to its W function; call the W function by name anyway.
function(motrack_apply_text_encoding target)
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${MOTRACK_TEXT_ENCODING_FLAGS}>")
    if(WIN32)
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    endif()
endfunction()

# Applies motrack_apply_text_encoding() to every compiled target defined in @p directory itself. Call it once, after
# the last target: FetchContent dependencies are defined in their own subdirectories and stay untouched.
function(motrack_apply_text_encoding_to_directory directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            motrack_apply_text_encoding(${target})
        endif()
    endforeach()
endfunction()
