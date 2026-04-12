#ifndef MENU_H_
#define MENU_H_

#include "ssd1306.h"
#include "sdcard.h"
#include "driver/gpio.h"

void draw_menu(SSD1306_t *dev);
void update_scroll();

extern volatile int cursor;        // vị trí con trỏ
extern volatile int start_index;

#endif