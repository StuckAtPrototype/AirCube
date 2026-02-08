idf_component_register(SRCS "main.c"
                                "led.c"
                                "led_color_lib.c"
                                "ws2812_control.c"
                                "ens210.c"
                                "ens16x_driver.c"
                                "i2c_driver.c"
                                "serial_protocol.c"
                                "button.c"
                                "history.c"
                       PRIV_REQUIRES spi_flash esp_partition esp_timer esp_driver_rmt esp_driver_i2c esp_driver_uart esp_driver_usb_serial_jtag vfs esp_pm esp_driver_gpio nvs_flash
                       INCLUDE_DIRS "")
