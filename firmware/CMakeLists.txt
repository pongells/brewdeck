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
# ESP-Hosted 3.x (P4) force-includes its config header with "-include eh_host_port_master_config.h", two words that
# PlatformIO's ESP-IDF builder sorts apart (pioarduino/platform-espressif32#576). The builder also gives the app's
# flags to every component and to the bootloader, which lack the header's folder and ESP-Hosted's settings. So the
# option becomes one word (-include<file>, the same to GCC) naming a small header made here: it includes ESP-Hosted's
# header by its full path, only where ESP-Hosted is in the settings (that header is macros and sdkconfig.h).
if(TARGET eh_host_config)
  get_target_property(opts eh_host_config INTERFACE_COMPILE_OPTIONS)
  get_target_property(dirs eh_host_config INTERFACE_INCLUDE_DIRECTORIES)
  list(GET dirs 0 dir)
  set(wrap "${CMAKE_BINARY_DIR}/esp_hosted_force_include.h")
  file(WRITE "${wrap}" "/* made by CMakeLists.txt: ESP-Hosted's forced config header, where ESP-Hosted is set up */\n"
                       "#include \"sdkconfig.h\"\n#ifdef CONFIG_ESP_HOSTED_HOST\n"
                       "#include \"${dir}/eh_host_port_master_config.h\"\n#endif\n")
  list(TRANSFORM opts REPLACE "^SHELL:-include +eh_host_port_master_config\\.h$" "-include${wrap}")
  set_target_properties(eh_host_config PROPERTIES INTERFACE_COMPILE_OPTIONS "${opts}")
endif()
