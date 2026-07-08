#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include "ssd1306.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif
/* task handler oled_task */
extern TaskHandle_t oled_taskHandle;

void oled_init(SSD1306_t * dev);
void oled_task(void *pvParameters);

#ifdef __cplusplus
}
#endif

#endif // OLED_H