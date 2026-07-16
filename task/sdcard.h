#ifndef SDCARD_H
#define SDCARD_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "std_types.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số bài hát tối đa hệ thống quản lý được (giới hạn kích thước gaSongNameList) */
#define SDCARD_MAX_SONGS   200U

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* 1 bản ghi trong file database "/sdcard/songs.db": đường dẫn file mp3 và file frame
   animation (.bin) đi kèm của 1 bài hát.
   KHÔNG gộp chung với Sdcard_SongInfoType_s bên dưới dù cả 2 đều mô tả "thông tin 1 bài
   hát", vì kích thước struct này chính là kích thước 1 bản ghi nhị phân trên đĩa
   (Sdcard_ScanAndCreateDb/Sdcard_GetSongByIndex dùng thẳng sizeof(Sdcard_SongDbType_s) để
   fwrite/fseek/fread) - path đầy đủ chỉ cần đọc từ đĩa đúng lúc phát 1 bài (qua
   Sdcard_GetSongByIndex), không cần giữ sẵn trong RAM cho tất cả bài hát. Nếu gộp field
   songPath/framePath (128+128 byte) vào Sdcard_SongInfoType_s rồi dùng cho gaSongNameList[200],
   RAM tĩnh cho danh sách bài hát sẽ tăng từ ~13KB lên ~63KB (gấp gần 5 lần) chỉ để lưu
   trùng lặp path của toàn bộ bài hát trong lúc thực tế chỉ cần path của đúng 1 bài đang
   phát tại một thời điểm. */
typedef struct {
    char songPath[128];
    char framePath[128];
} Sdcard_SongDbType_s;

/* Thông tin 1 bài hát giữ trong RAM, dùng cho Menu_Draw hiển thị danh sách - chỉ giữ đúng
   field cần cho việc vẽ danh sách (tên bài), xem lý do không gộp với Sdcard_SongDbType_s
   ở comment phía trên. Không giữ chỉ số bài hát ở đây - chỉ số đó luôn chính là vị trí
   phần tử trong gaSongNameList[], không cần lưu lặp lại thành 1 field riêng */
typedef struct {
    char songName[64];
} Sdcard_SongInfoType_s;

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Sdcard_Task */
extern TaskHandle_t xSdTaskHandle;

/* Danh sách tên bài hát trong RAM, do Sdcard_ScanAndCreateDb() nạp lúc khởi động,
   dùng chung cho menu hiển thị (xem menu.c) */
extern Sdcard_SongInfoType_s gaSongNameList[SDCARD_MAX_SONGS];

/* Hàng đợi lệnh dùng chung tới Sdcard_Task (phần tử kiểu Srm_Message_s), do Sdcard_Init()
   tạo (Sdcard_Task là owner của thẻ SD - cả double buffer animation lẫn dữ liệu mp3 thô,
   nhận SDCARD_CMD_GET_SINGLE_FRAME qua Srm_SdcardGetSingleFrame(), xem srm.h và
   driver/buffer/double_buffer.c) */
extern QueueHandle_t xSdCommandQueue;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Sdcard_Init
 * Khởi tạo module Sdcard: tạo xSdCommandQueue để các module khác (double_buffer.c) gửi
 * lệnh vào. Gọi bởi chính Sdcard_Task lúc khởi động (đầu Sdcard_Task, trước Sdcard_Mount()),
 * không còn gọi từ app_main() - an toàn vì xSdCommandQueue chỉ được đụng tới lần đầu qua
 * Srm_SdcardGetSingleFrame(), CHỈ gọi trong Oled_PlayAnimation() sau khi người dùng bấm
 * Play/Next/Prev (xem Sdcard_Task trong sdcard.c).
 * @param
 * @return
 */
void Sdcard_Init(void);

/**
 * @brief Sdcard_Mount
 * Mount thẻ nhớ SD - chỉ lo phần chung (đường dẫn mount, cấu hình FAT), phần chân/bus phần
 * cứng thật sự (hiện tại: SDMMC) do driver/sdmmc/sdmmc.c đảm nhiệm, xem sdcard.c
 * @param
 * @return ESP_OK nếu mount thành công
 */
esp_err_t Sdcard_Mount(void);

/**
 * @brief Sdcard_ScanAndCreateDb
 * Quét toàn bộ thư mục gốc thẻ nhớ, tìm các file .mp3 có file .bin (frame animation)
 * đi kèm, ghi thành file database "/sdcard/songs.db" và nạp tên bài hát vào gaSongNameList
 * @param basePath: thư mục gốc cần quét (vd "/sdcard")
 * @return
 */
void Sdcard_ScanAndCreateDb(const char *basePath);

/**
 * @brief Sdcard_ReadDbFile
 * In toàn bộ nội dung file database "/sdcard/songs.db" ra console (debug)
 * @param
 * @return
 */
void Sdcard_ReadDbFile(void);

/**
 * @brief Sdcard_GetSongByIndex
 * Đọc 1 bản ghi trong file database "/sdcard/songs.db" theo chỉ số
 * @param index: chỉ số bài hát cần lấy (0..gsPlayerContext.totalSong-1)
 * @param pOut: struct nhận thông tin bài hát
 * @return E_OK nếu lấy thành công, E_NOT_OK nếu lỗi (index ngoài phạm vi, không mở được file...)
 */
Std_ReturnType Sdcard_GetSongByIndex(uint16_t index, Sdcard_SongDbType_s *pOut);

/**
 * @brief Sdcard_Task
 * Task quản lý việc nạp dữ liệu frame animation từ thẻ nhớ vào double buffer cho bài
 * đang phát, chạy song song để không làm chậm Oled_Task khi vẽ animation.
 * @param pvParameters
 * @return
 */
void Sdcard_Task(void *pvParameters);

#endif /* SDCARD_H */
