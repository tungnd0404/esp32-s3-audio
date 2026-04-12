idf_component_register(
    SRCS
        "Audio.c"
        "../driver/sdcard/sdcard.c"
        "../driver/ssd1306/oled.c"
        "../driver/ssd1306/ssd1306_i2c_new.c"
        "../driver/ssd1306/ssd1306.c"
        "../driver/ssd1306/ssd1306_spi.c"
        "../driver/ssd1306/menu.c"
        "../app/button.c"

    INCLUDE_DIRS
        "."
        "../driver/sdcard"
        "../driver/ssd1306"
        "../config"
        "../app"
)