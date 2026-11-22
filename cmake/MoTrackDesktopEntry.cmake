# Install-time script: write the Linux desktop entry for the actual install prefix, so registering a staged
# build launches that binary. Inputs, relative to the prefix where noted:
#   MOTRACK_DESKTOP_TEMPLATE  source template with @MOTRACK_DESKTOP_EXEC@ and @MOTRACK_DESKTOP_TRY_EXEC@
#   MOTRACK_DESKTOP_OUTPUT    destination (relative)
#   MOTRACK_DESKTOP_BINARY    installed executable (relative)
cmake_minimum_required(VERSION 3.25)

set(binary "${CMAKE_INSTALL_PREFIX}/${MOTRACK_DESKTOP_BINARY}")
if(binary MATCHES "[\r\n]")
    message(FATAL_ERROR "A desktop entry cannot name an executable whose path contains a line break: ${binary}")
endif()
# The specification spells a literal % as %%, but GLib looks up the Exec program before expanding %%
# and then refuses to load the entry, so no spelling launches such a path from a GLib file manager.
if(binary MATCHES "%")
    message(FATAL_ERROR "Desktop launchers cannot start an executable whose path contains %. Install to another prefix: ${binary}")
endif()

# Desktop Entry Specification, "The Exec key": quote the program and escape \ " ` $ inside the quotes,
# then apply the string-value escape to every backslash. (CMake already turned any \ into /.)
string(REPLACE "\\" "\\\\" quoted "${binary}")
string(REPLACE "\"" "\\\"" quoted "${quoted}")
string(REPLACE "`" "\\`" quoted "${quoted}")
string(REPLACE "$" "\\$" quoted "${quoted}")
string(REPLACE "\\" "\\\\" quoted "${quoted}")
set(MOTRACK_DESKTOP_EXEC "\"${quoted}\"")
# TryExec is an unquoted string value; only backslashes need escaping.
string(REPLACE "\\" "\\\\" MOTRACK_DESKTOP_TRY_EXEC "${binary}")

set(output "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${MOTRACK_DESKTOP_OUTPUT}")
message(STATUS "Installing: ${output}")
configure_file("${MOTRACK_DESKTOP_TEMPLATE}" "${output}" @ONLY)
