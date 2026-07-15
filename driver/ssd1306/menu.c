/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "menu.h"
#include "player_manager.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Màn hình 128x64, font 8x8 -> hiển thị tối đa 8 dòng cùng lúc */
#define MENU_MAX_DISPLAY_LINES      8

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* Vị trí dòng đầu tiên trong vùng hiển thị (cửa sổ cuộn menu).
   Kiểu int32_t (có dấu) vì thuật toán Menu_UpdateScroll có phép trừ có thể ra số âm tạm thời. */
static int32_t gi32StartIndex = 0;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Menu_Draw
 * Vẽ danh sách bài hát lên màn hình OLED, đánh dấu vị trí cursor hiện tại (gsPlayerContext.cursor)
 * @param dev: con trỏ device SSD1306
 * @return
 */
void Menu_Draw(SSD1306_t *dev)
{
    /* Cursor dùng chung với player_manager, không giữ bản sao riêng trong menu.c.
       Luôn là chỉ số hợp lệ (không âm) nên giữ nguyên kiểu uint32_t như gsPlayerContext.cursor */
    uint32_t lu32Cursor = gsPlayerContext.cursor;

    ssd1306_clear_screen(dev, false);

    for (uint32_t lu32Line = 0; lu32Line < MENU_MAX_DISPLAY_LINES; lu32Line++)
    {
        /* Menu_UpdateScroll luôn đảm bảo gi32StartIndex >= 0 trước khi Draw được gọi */
        uint32_t lu32ItemIndex = (uint32_t)gi32StartIndex + lu32Line;
        if (lu32ItemIndex >= gu16SongCount) break;

        char lacLine[32];

        if (lu32ItemIndex == lu32Cursor)
        {
            snprintf(lacLine, sizeof(lacLine), "> %.27s", gaSongNameList[lu32ItemIndex].songName);
        }
        else
        {
            snprintf(lacLine, sizeof(lacLine), "  %.27s", gaSongNameList[lu32ItemIndex].songName);
        }

        ssd1306_display_text(dev, lu32Line, lacLine, strlen(lacLine), false);
    }
}

/**
 * @brief Menu_UpdateScroll
 * Cập nhật lại gi32StartIndex để cursor hiện tại luôn nằm trong vùng hiển thị
 * @param
 * @return
 */
void Menu_UpdateScroll(void)
{
    /* Cursor dùng chung với player_manager. Ở đây cần trừ nên phải là int32_t có dấu,
       khác với Menu_Draw (chỉ so sánh bằng nên giữ uint32_t) */
    int32_t li32Cursor = (int32_t)gsPlayerContext.cursor;

    if (li32Cursor < gi32StartIndex)
    {
        gi32StartIndex = li32Cursor;
    }

    if (li32Cursor >= gi32StartIndex + MENU_MAX_DISPLAY_LINES)
    {
        gi32StartIndex = li32Cursor - MENU_MAX_DISPLAY_LINES + 1;
    }

    if (gi32StartIndex < 0)
    {
        gi32StartIndex = 0;
    }

    /* Chỉ giới hạn theo gu16SongCount - MAX_LINES khi thực sự nhiều bài hơn số dòng hiển thị,
       tránh (gu16SongCount - MENU_MAX_DISPLAY_LINES) âm khi danh sách ít bài hơn 1 màn hình */
    if ((int32_t)gu16SongCount > MENU_MAX_DISPLAY_LINES)
    {
        int32_t li32MaxStartIndex = (int32_t)gu16SongCount - MENU_MAX_DISPLAY_LINES;
        if (gi32StartIndex > li32MaxStartIndex)
        {
            gi32StartIndex = li32MaxStartIndex;
        }
    }
    else
    {
        gi32StartIndex = 0;
    }
}
