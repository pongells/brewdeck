# ESP-Hosted co-processor firmware for the P4 board's ESP32-C6 (built by ../build.sh p4-c6-update, carried by the C6
# updater ../src/c6update). Settings: Espressif's example defaults.
cmake_minimum_required(VERSION 3.16.0)
set(IDF_TARGET esp32c6)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")   # the one component (app_main)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c6")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brewdeck_c6)
