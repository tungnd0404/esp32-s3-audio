#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "button.h"
#include "sdcard.h"

// ===== MENU DEFAULT 20 ITEMS =====
//static int song_count = song_count;

volatile int cursor = 0;        // vị trí con trỏ
volatile int start_index = 0;   // vị trí dòng đầu tiên trong vùng hiển thị

// ================== VẼ MENU ==================
void draw_menu(SSD1306_t *dev) 
{
    ssd1306_clear_screen(dev, false);

    int max_display = 8; // Màn hình 128x64 => 8 dòng (font 8x8)

    for (int i = 0; i < max_display; i++) {
        int item_index = start_index + i;
        if (item_index >= song_count) break;

        char line[32];

        if (item_index == cursor)
            snprintf(line, sizeof(line), "> %.27s", song_list[item_index].song_name);
        else
            snprintf(line, sizeof(line), "  %.27s", song_list[item_index].song_name);

        ssd1306_display_text(dev, i, line, strlen(line), false);
    }
}

// ================== CUỘN MENU ==================
void update_scroll() 
{
    int max_display = 8;

    if (cursor < start_index)
        start_index = cursor;

    if (cursor >= start_index + max_display)
        start_index = cursor - max_display + 1;

    if (start_index < 0) start_index = 0;
    if (start_index > song_count - max_display)
        start_index = song_count - max_display;
}

