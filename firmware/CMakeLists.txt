# ESP-IDF project (PlatformIO framework = espidf): the firmware is the one component in src/ plus the components it
# names (src/idf_component.yml), compiled with the settings below and the component versions locked in
# dependencies.lock.<chip>.
cmake_minimum_required(VERSION 3.16.0)
# settings: the chip's base (sdkconfig.base.<chip>), then Brewdeck's changes; named here so ESP-IDF does not also read
# a sdkconfig.defaults
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.base.${IDF_TARGET};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.brewdeck.${IDF_TARGET}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# one lock file per chip (PlatformIO passes the chip as -DIDF_TARGET)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")
project(brewdeck)
