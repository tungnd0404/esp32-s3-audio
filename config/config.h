#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define DEVELOPER_CONFIGURATION
/*------------------------------------------------------------
                         CONFIG PROJECT
------------------------------------------------------------*/
#define ON   1
#define OFF  0

/*------------------------I2C---------------------------- */
#define CONFIG_I2C_PORT_0   ON
#define CONFIG_I2C_PORT_1   OFF

#define CONFIG_SDA_GPIO          8  
#define CONFIG_SCL_GPIO         9
#define CONFIG_RESET_GPIO      -1

#define CONFIG_WIDTH         128
#define CONFIG_HEIGHT        64
#define CONFIG_OFFSETX  0

// Gán chân nút bấm
#define BTN_NEXT_PIN   16
#define BTN_PREV_PIN   17
#define BTN_PLAY_PIN   18

// Định nghĩa số frame trên giây (có thể điều chỉnh)
#define FRAME_PER_SECOND 15

/* Gán chân SD card */
#define SD_CLK   GPIO_NUM_39
#define SD_CMD   GPIO_NUM_38
#define SD_D0   GPIO_NUM_40

#ifdef __cplusplus
}
#endif

#endif // CONFIG_H