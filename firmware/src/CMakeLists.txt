# The firmware's sources: the core, the board layer (common + the board's folder) and one UI folder, as chosen in
# ../CMakeLists.txt (BREWDECK_BOARD, BREWDECK_UI, BREWDECK_ONLY).
idf_build_get_property(board BREWDECK_BOARD)
idf_build_get_property(ui BREWDECK_UI)
idf_build_get_property(only BREWDECK_ONLY)
set(d "${CMAKE_CURRENT_LIST_DIR}")
if(IS_ABSOLUTE "${ui}")   # a UI kept outside this repository
  set(ui_dir "${ui}")
else()
  set(ui_dir "${d}/${ui}")
endif()
if(NOT only AND NOT IS_DIRECTORY "${ui_dir}")
  message(FATAL_ERROR "BREWDECK_UI=${ui}: no such UI folder (a folder under src/ or an absolute path)")
endif()
# a new or removed source file configures again (not while ESP-IDF reads this file for its requirements: script mode)
set(again CONFIGURE_DEPENDS)
if(CMAKE_SCRIPT_MODE_FILE)
  set(again "")
endif()
if(only)   # a standalone program (c6update): only its folder
  file(GLOB_RECURSE srcs ${again} "${d}/${only}/*.c" "${d}/${only}/*.cpp")
else()
  file(GLOB_RECURSE srcs ${again} FOLLOW_SYMLINKS
    "${d}/core/*.c" "${d}/core/*.cpp"
    "${d}/board/common/*.c" "${d}/board/common/*.cpp"
    "${d}/board/${board}/*.c" "${d}/board/${board}/*.cpp"
    "${ui_dir}/*.c" "${ui_dir}/*.cpp")
endif()
# the ESP-IDF components it uses (the ones in idf_component.yml are added by the component manager)
set(reqs app_update esp_app_format esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_uart
         esp_driver_usb_serial_jtag esp_event esp_http_client esp_hw_support esp_lcd esp_mm esp_netif esp_partition
         esp_system esp_timer esp_wifi freertos heap log lwip mbedtls nvs_flash spi_flash)
if(board STREQUAL "p4")
  list(APPEND reqs esp_driver_ppa)
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "core" "board/common" "board/${board}" "../include"
                       REQUIRES ${reqs}
                       # every object file is linked: LVGL and ESP-IDF call functions defined
                       # here (core/lv_mem_psram.c, the S3's cache-freeze wrappers) from libraries linked later
                       WHOLE_ARCHIVE)
if(only STREQUAL "c6update")   # the C6 updater carries the C6 image (firmware/c6, built first)
  set(c6_image "${d}/../c6/build/firmware.bin")   # build.sh copies it here: the name gives the symbol
  if(NOT EXISTS "${c6_image}")
    message(FATAL_ERROR "${c6_image} missing: build the C6 firmware first (./build.sh p4-c6-update does)")
  endif()
  target_add_binary_data(${COMPONENT_LIB} "${c6_image}" BINARY)
endif()
# ESP-IDF compiles with -Wall -Werror=all. GCC's snprintf truncation check assumes every int can be 10 digits and
# every string fills its buffer; text cut short by snprintf is the intended, safe result here and in other UIs
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
if(board STREQUAL "s3")   # flash mappings take the cache-sync lock first (board/s3/cache_freeze.cpp)
  target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_cache_freeze_caches_disable_interrupts"
                                                   "-Wl,--wrap=esp_cache_unfreeze_caches_enable_interrupts")
endif()
# The firmware's version (core.cpp: brewdeck_version.h): the git tag ("v0.2.0" on a release), "v0.2.0-3-gabc1234"
# between releases, "-dirty" with uncommitted changes, a bare commit hash without tags (shallow CI checkouts);
# BREWDECK_VERSION in the environment overrides it (update tests). Written on every build, changed only when it
# changes, so only core.cpp recompiles.
set(gen "${CMAKE_BINARY_DIR}/gen")
add_custom_target(brewdeck_version
  COMMAND ${CMAKE_COMMAND} -DOUT=${gen}/brewdeck_version.h -DREPO=${d}/.. -P ${d}/../version.cmake
  BYPRODUCTS ${gen}/brewdeck_version.h)
add_dependencies(${COMPONENT_LIB} brewdeck_version)
target_include_directories(${COMPONENT_LIB} PRIVATE ${gen})

