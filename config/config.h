#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif
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
#define BTN_NEXT   16
#define BTN_PREV   17
#define BTN_PLAY   18

#ifdef __cplusplus
}
#endif

#endif // CONFIG_H