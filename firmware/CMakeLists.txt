# ESP-IDF project (PlatformIO framework = arduino, espidf): the Arduino core is one ESP-IDF component, and everything
# is compiled from source together, with the settings below and the component versions locked in
# dependencies.lock.<chip>.
cmake_minimum_required(VERSION 3.16.0)
# settings: the stock Arduino libraries' settings for the chip (path from ui_select.py), then Brewdeck's changes; named
# here so ESP-IDF does not also read a sdkconfig.defaults (an Arduino-only build writes one into this folder)
# (unset: the S3's Arduino-only build is recompiling the Arduino libraries through this file: its own settings apply)
if(NOT "$ENV{BREWDECK_STOCK_SDKCONFIG}" STREQUAL "")
  set(SDKCONFIG_DEFAULTS "$ENV{BREWDECK_STOCK_SDKCONFIG};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.brewdeck.${IDF_TARGET}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# one lock file per chip (PlatformIO passes the chip as -DIDF_TARGET)
if(NOT "$ENV{BREWDECK_STOCK_SDKCONFIG}" STREQUAL "")
  idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
endif()
project(brewdeck)
