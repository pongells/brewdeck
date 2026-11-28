# Writes the firmware's version header (src/CMakeLists.txt runs it on every build): cmake -DOUT=<header>
# -DREPO=<folder in the repository> -P version.cmake. The text is only rewritten when it changes.
set(ver "$ENV{BREWDECK_VERSION}")
if(ver STREQUAL "")
  execute_process(COMMAND git describe --tags --always --dirty WORKING_DIRECTORY "${REPO}"
                  OUTPUT_VARIABLE ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
string(REPLACE "\"" "" ver "${ver}")
set(text "#pragma once\n#define BREWDECK_VERSION \"${ver}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT "${old}" STREQUAL "${text}")
  file(WRITE "${OUT}" "${text}")
endif()
