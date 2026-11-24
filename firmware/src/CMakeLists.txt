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
if("$ENV{BREWDECK_STOCK_SDKCONFIG}" STREQUAL "")   # the S3's library recompile: it needs no sources from here
  set(srcs "")
elseif(NOT "$ENV{BREWDECK_SRC_ONLY}" STREQUAL "")   # a standalone program (p4-c6-update): only its folder
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
