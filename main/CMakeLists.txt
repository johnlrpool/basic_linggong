idf_component_register(SRCS "电压测试正确.c" "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver nvs_flash esp_psram spi_flash)
