# The firmware's sources: the core, the board layer (common + the board's folder) and one UI folder. ui_select.py
# (PlatformIO pre-script) passes the board (s3 / p4) and the UI folder (ui, ui_minimal, ui_ext = a linked outside UI).
set(board "$ENV{BREWDECK_BOARD}")
set(ui "$ENV{BREWDECK_SRC_UI}")
if(NOT board)
  set(board s3)
endif()
if(NOT ui)
  set(ui ui)
endif()
set(d "${CMAKE_CURRENT_LIST_DIR}")
if(NOT "$ENV{BREWDECK_SRC_ONLY}" STREQUAL "")   # a standalone program (p4-c6-update): only its folder
  file(GLOB_RECURSE srcs "${d}/$ENV{BREWDECK_SRC_ONLY}/*.c" "${d}/$ENV{BREWDECK_SRC_ONLY}/*.cpp")
else()
file(GLOB_RECURSE srcs FOLLOW_SYMLINKS
  "${d}/core/*.c" "${d}/core/*.cpp"
  "${d}/board/common/*.c" "${d}/board/common/*.cpp"
  "${d}/board/${board}/*.c" "${d}/board/${board}/*.cpp"
  "${d}/${ui}/*.c" "${d}/${ui}/*.cpp")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "core" "board/common" "board/${board}" "../include")
if("$ENV{BREWDECK_SRC_ONLY}" STREQUAL "c6update")   # the C6 updater carries the C6 image (firmware/c6, built first)
  set(c6_image "${d}/../c6/.pio/build/c6/firmware.bin")
  if(NOT EXISTS "${c6_image}")
    message(FATAL_ERROR "${c6_image} missing: build the C6 firmware first (./build.sh p4-c6-update does)")
  endif()
  target_add_binary_data(${COMPONENT_LIB} "${c6_image}" BINARY)
endif()
if(board STREQUAL "s3")   # flash mappings take the cache-sync lock first (board/s3/cache_freeze.cpp)
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_cache_freeze_caches_disable_interrupts"
                                                   "-Wl,--wrap=esp_cache_unfreeze_caches_enable_interrupts")
endif()
