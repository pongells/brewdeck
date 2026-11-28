idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hosted
                       PRIV_REQUIRES nvs_flash esp_wifi
                       WHOLE_ARCHIVE)   # app_main is called from ESP-IDF libraries linked after this one
