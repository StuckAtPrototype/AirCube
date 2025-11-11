idf_component_register(SRCS "main.c"
                                "led.c"
                                "led_color_lib.c"
                                "ws2812_control.c"
                       PRIV_REQUIRES spi_flash esp_driver_rmt
                       INCLUDE_DIRS "")
