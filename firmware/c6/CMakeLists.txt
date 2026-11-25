# ESP-Hosted co-processor firmware for the ESP32-C6 (platformio.ini). Settings: Espressif's example defaults.
cmake_minimum_required(VERSION 3.16.0)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32c6")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brewdeck_c6)
