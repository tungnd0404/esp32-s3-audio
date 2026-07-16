/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdmmc.h"
#include "sdcard.h"
#include "sdcard_config.h"
#include "feature_config.h"
#include "player_manager.h"
#include "double_buffer.h"
#include "ring_buffer.h"
#include "mp3.h"
#include "srm.h"
#include "oled.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Đường dẫn file database chứa danh sách bài hát, tạo bởi Sdcard_ScanAndCreateDb() - ghép từ
   SDCARD_MOUNT_POINT (sdcard_config.h) thay vì hardcode lại "/sdcard", tránh 2 nơi cùng chứa
   1 chuỗi có thể lệch nhau nếu sau này đổi mount point */
#define SDCARD_DB_PATH              SDCARD_MOUNT_POINT "/songs.db"

/* Chu kỳ vTaskDelay giữa các lần lặp trong Sdcard_LoadSong() - nhịp nghỉ để nhường
   CPU cho task khác (Mp3_Task, Oled_Task...) thay vì busy-poll liên tục, vì xQueueReceive
   trên xSdCommandQueue giờ non-blocking (xem Sdcard_ServicePendingCommand) không còn tự
   chờ hộ vòng lặp nữa - giống hệt OLED_ANIMATION_DELAY_MS bên oled.c */
#define SDCARD_LOAD_WAIT_MS         50U

/* Số request tối đa có thể chờ xử lý trong xSdCommandQueue cùng lúc */
#define SDCARD_COMMAND_QUEUE_LENGTH 4U

/* Kích thước mỗi lần đọc file mp3 để nạp vào xMp3RingBuffer (byte). Lớn hơn nhiều so với
   VS1053_CHUNK_SIZE (32 byte - kích thước Mp3_Task gửi cho VS1053 mỗi lần, xem vs1053.h) để
   giảm số lần gọi fread() trên thẻ SD - việc tách "đọc thẻ SD theo khối lớn" khỏi "gửi cho
   VS1053 theo khối nhỏ" chính là lý do cần xMp3RingBuffer làm lớp đệm trung gian */
#define SDCARD_MP3_READ_CHUNK_SIZE  512U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Sdcard_Task */
TaskHandle_t xSdTaskHandle = NULL;

/* Danh sách tên bài hát trong RAM, dùng cho menu hiển thị */
Sdcard_SongInfoType_s gaSongNameList[SDCARD_MAX_SONGS];

/* Hàng đợi lệnh dùng chung tới Sdcard_Task, tạo trong Sdcard_Init() vì Sdcard_Task là owner
   dữ liệu double buffer animation (xem driver/buffer/double_buffer.c) */
QueueHandle_t xSdCommandQueue = NULL;

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

static const char *TAG = "SDCARD";

/* File mp3 của bài ĐANG THỰC SỰ PHÁT, chỉ Sdcard_Task được đụng vào (kiến trúc Owner Task -
   Mp3_Task không còn tự fopen/fread thẳng trên thẻ SD nữa, xem Mp3_StreamSong trong
   mp3.c). Đóng và mở lại mỗi khi đổi bài (xem Sdcard_LoadSong), KHÔNG đóng lúc Pause -
   giữ nguyên vị trí đọc dở để Resume tiếp tục đúng chỗ. NULL khi chưa có bài nào đang mở. */
static FILE *gpMp3File = NULL;

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Sdcard_HandleCommand
 * Xử lý 1 request nhận được từ xSdCommandQueue (xem srm.h - kiến trúc Owner Task): dựa vào
 * cmdId gọi đúng hàm DoubleBuffer_* tương ứng (Sdcard_Task là owner duy nhất được phép đụng
 * vào double buffer animation, xem driver/buffer/double_buffer.c).
 * @param pRequest: request nhận được từ xSdCommandQueue
 * @return
 */
static void Sdcard_HandleCommand(const Srm_Message_s *pRequest)
{
    /* Return value */
    Std_ReturnType lRet = (uint32_t)E_NOT_OK;

    switch ((Srm_CommandType_e)pRequest->cmdId)
    {
        case SDCARD_CMD_GET_SINGLE_FRAME:
        {
            /* Chạy thẳng trên thread của Sdcard_Task - DoubleBuffer_GetFrame() tự lo nạp
               trước/nạp gấp bằng lời gọi hàm thường nếu thiếu dữ liệu, ghi thẳng vào
               pRequest->pData (buffer của Oled_Task truyền vào qua Srm_SdcardGetSingleFrame(),
               xem srm.c) rồi mới trả lời - Oled_Task nhận được phản hồi thành công là biết
               chắc dữ liệu đã sẵn sàng trong buffer của chính nó */
            lRet = DoubleBuffer_GetFrame(pRequest->payload, (uint8_t *)pRequest->pData);
            Srm_Reply(pRequest, (uint32_t)lRet);
            break;
        }

        default:
            /* cmdId lạ (chưa định nghĩa xử lý) -> vẫn trả lời E_NOT_OK để bên gửi (nếu có
               chờ) không phải đợi hết timeout mới coi là lỗi */
            Srm_Reply(pRequest, (uint32_t)E_NOT_OK);
            break;
    }
}

/**
 * @brief Sdcard_ServicePendingCommand
 * Check non-blocking (timeout = 0) xem xSdCommandQueue có request nào đang chờ không, có
 * thì xử lý ngay. Gọi mỗi vòng lặp trong Sdcard_LoadSong() - cùng khuôn mẫu với
 * Mp3_ServicePendingCommand() (mp3.c) và Oled_ServicePendingCommand() (oled.c).
 * @param
 * @return
 */
static void Sdcard_ServicePendingCommand(void)
{
    Srm_Message_s lRequest;

    if (xQueueReceive(xSdCommandQueue, &lRequest, 0) == pdTRUE)
    {
        Sdcard_HandleCommand(&lRequest);
    }
}

/**
 * @brief Sdcard_FillMp3RingBuffer
 * Đọc thêm dữ liệu từ gpMp3File (nếu đang mở và chưa hết file) nạp vào xMp3RingBuffer cho
 * tới khi KHÔNG còn đủ chỗ trống cho 1 khối SDCARD_MP3_READ_CHUNK_SIZE nữa, hoặc gặp EOF.
 * Gọi mỗi vòng lặp trong Sdcard_LoadSong() để xMp3RingBuffer luôn được nạp gần đầy -
 * khác với frame animation (Oled_Task chủ động xin từng frame qua SDCARD_CMD_GET_SINGLE_FRAME,
 * xem DoubleBuffer_GetFrame), ring buffer không cần Mp3_Task báo hiệu mới nạp: cứ còn chỗ
 * trống là nạp tiếp, đơn giản hơn vì đây là luồng dữ liệu tuần tự (không cần truy cập ngẫu
 * nhiên theo chỉ số như frame animation).
 * @param
 * @return
 */
static void Sdcard_FillMp3RingBuffer(void)
{
    if ((gpMp3File == NULL) || (gbMp3StreamEof == true))
    {
        return;
    }

    uint8_t lau8Chunk[SDCARD_MP3_READ_CHUNK_SIZE];

    /* Kiểm tra chỗ trống TRƯỚC khi đọc file - đảm bảo RingBuffer_Write() bên dưới luôn có
       đủ chỗ để ghi hết số byte vừa đọc được, không bao giờ phải huỷ dữ liệu đã đọc dở */
    while (RingBuffer_GetFreeSize(xMp3RingBuffer) >= SDCARD_MP3_READ_CHUNK_SIZE)
    {
        size_t lReadBytes = fread(lau8Chunk, 1, SDCARD_MP3_READ_CHUNK_SIZE, gpMp3File);
        if (lReadBytes == 0)
        {
            /* fread() trả 0 có thể là HẾT FILE THẬT (feof()) hoặc LỖI ĐỌC THẬT SỰ (ferror() -
               vd rút thẻ SD giữa chừng) - 2 tình huống khác nhau nhưng trước đây bị xử lý y hệt
               nhau (chỉ set gbMp3StreamEof = true), khiến người dùng không hề biết thẻ đã bị
               rút, chỉ thấy nhạc dừng êm như hết bài bình thường. Phân biệt bằng ferror() - nếu
               đúng là lỗi đọc, báo cho Oled_Task hiển thị lỗi qua kênh SRM sẵn có (dùng lại
               đúng OLED_BOOT_STATUS_SD_ERROR/Srm_OledNotifyBootStatus() vốn đã dùng lúc boot -
               không tạo thêm command/status code mới, không phá vỡ kiến trúc Owner Task/SRM).
               gbMp3StreamEof vẫn phải set true trong CẢ 2 trường hợp - dù EOF thật hay lỗi đọc,
               Sdcard_Task đều không còn gì để nạp thêm cho bài này nữa */
            if (ferror(gpMp3File))
            {
                ESP_LOGE(TAG, "Read error on mp3 stream (SD card removed/faulty?)");
                Srm_OledNotifyBootStatus(OLED_BOOT_STATUS_SD_ERROR);
            }

            gbMp3StreamEof = true;
            break;
        }

        /* Free size đã được đảm bảo đủ chỗ ngay trước vòng lặp này (chỉ Sdcard_Task tự ghi,
           không ai khác giành chỗ ghi cùng lúc - xem hợp đồng 1-gửi-1-nhận trong
           ring_buffer.h) nên về lý thuyết write() luôn thành công; vẫn log cảnh báo nếu
           không, thay vì âm thầm mất dữ liệu mp3 không rõ lý do */
        if (RingBuffer_Write(xMp3RingBuffer, lau8Chunk, lReadBytes, 0) == E_NOT_OK)
        {
            ESP_LOGW(TAG, "RingBuffer_Write failed despite free size check (%u bytes)", (unsigned int)lReadBytes);
        }
    }
}

/**
 * @brief Sdcard_LoadSong
 * Mở double buffer + file mp3 cho bài hát ĐANG THỰC SỰ PHÁT (gsPlayerContext.currentSong)
 * rồi liên tục: (1) xử lý lệnh SDCARD_CMD_GET_SINGLE_FRAME gửi tới qua xSdCommandQueue mỗi khi
 * Oled_Task cần 1 frame animation (xem Sdcard_HandleCommand), và
 * (2) nạp thêm dữ liệu mp3 vào xMp3RingBuffer cho Mp3_Task rút ra phát (xem
 * Sdcard_FillMp3RingBuffer) - cho tới khi có bài mới cần chuyển sang. Cùng khuôn mẫu với
 * Oled_PlayAnimation() (oled.c): mỗi vòng lặp đều check non-blocking notification để không
 * trễ khi cần đổi bài, kết quả trả về qua tham số ra để vòng lặp ngoài không bị mất giá trị
 * notification vừa nhận.
 * @param pu32NotifyValue: [out] giá trị notification mới nhận được, chỉ có ý nghĩa khi
 *        hàm trả về true
 * @return true nếu thoát do có notification mới cần xử lý tiếp, false nếu thoát do lỗi
 *         đọc thông tin bài hát (Sdcard_GetSongByIndex thất bại, vd DB lỗi)
 */
static bool Sdcard_LoadSong(uint32_t *pu32NotifyValue)
{
    Sdcard_SongDbType_s lSong;

    /* Không dùng gsPlayerContext.cursor - vì cursor có thể đã di chuyển sang bài khác nếu
       người dùng đang duyệt MENU trong lúc nhạc vẫn phát nền (xem tính năng auto-return) */
    if (Sdcard_GetSongByIndex((uint16_t)gsPlayerContext.currentSong, &lSong) == E_NOT_OK)
    {
        return false;
    }

    /* Đóng buffer của bài cũ (nếu có), nạp đầy lại cho bài mới */
    DoubleBuffer_UnloadAll();
    DoubleBuffer_LoadAll(lSong.framePath);

    /* Đóng file mp3 của bài cũ (nếu có) trước khi mở file mới. KHÔNG tự RingBuffer_Reset()
       xMp3RingBuffer ở đây dù có vẻ hợp lý (Sdcard_Task là bên chủ động biết "dữ liệu cũ cần
       xoá") - Sdcard_Task là bên GHI của ring buffer này, còn RingBuffer_Reset() bản chất là
       thao tác NHẬN; gọi nhầm phía sẽ đụng độ với Mp3_Task (bên nhận thật sự) nếu nó đang
       mid-RingBuffer_Read() cùng lúc - xem cảnh báo chi tiết trong ring_buffer.h. Việc dọn
       sạch dữ liệu cũ trong xMp3RingBuffer do chính Mp3_Task tự làm khi nhận CÙNG notification
       này (xem case BTN_STATE_NEXT/PREV/PLAY_NEW trong Mp3_Task, mp3.c) */
    if (gpMp3File != NULL)
    {
        fclose(gpMp3File);
        gpMp3File = NULL;
    }
    gbMp3StreamEof = false;

    gpMp3File = fopen(lSong.songPath, "rb");
    if (gpMp3File == NULL)
    {
        /* Không mở được file mp3 -> coi như hết dữ liệu ngay, để Mp3_Task không chờ vô ích
           (vẫn tiếp tục chạy vòng lặp bên dưới bình thường cho phần frame animation) */
        ESP_LOGE(TAG, "Cannot open mp3 %s", lSong.songPath);
        gbMp3StreamEof = true;
    }

    /* Vòng lặp nạp dữ liệu cho bài đang phát, chạy tới khi có notification mới (đổi bài) -
       giống hệt Oled_PlayAnimation()/Mp3_StreamSong(): check non-blocking notify trước, tranh
       thủ trả lời lệnh + làm việc, rồi mới delay nhường CPU ở cuối vòng lặp */
    while (1)
    {
        /* Check non-blocking (timeout = 0): có notification mới thì thoát ngay, không chờ */
        if (xTaskNotifyWait(0, UINT32_MAX, pu32NotifyValue, 0) == pdTRUE)
        {
            return true;
        }

        /* Tranh thủ trả lời các request đang chờ (vd Oled_Task xin frame animation) trong
           lúc đang chạy vòng lặp này, xem Sdcard_ServicePendingCommand() */
        Sdcard_ServicePendingCommand();

        /* Tranh thủ nạp thêm dữ liệu mp3 cho Mp3_Task mỗi vòng lặp - không cần chờ yêu cầu
           gì từ Mp3_Task, cứ còn chỗ trống trong xMp3RingBuffer là nạp tiếp (xem
           Sdcard_FillMp3RingBuffer) */
        Sdcard_FillMp3RingBuffer();

        /* Không còn tác vụ nào tự chờ (queue receive đã chuyển non-blocking ở trên) -> phải
           tự delay để nhường CPU cho task khác (Mp3_Task, Oled_Task...) thay vì busy-poll
           liên tục, giống hệt Oled_PlayAnimation() */
        vTaskDelay(pdMS_TO_TICKS(SDCARD_LOAD_WAIT_MS));
    }
}

/**
 * @brief Sdcard_HasExtension
 * Kiểm tra tên file có đúng phần mở rộng cần tìm không
 * @param name: tên file bao gồm extension (vd "song.mp3")
 * @param ext: phần mở rộng cần so khớp, bao gồm dấu chấm (vd ".mp3")
 * @return true nếu đúng, false nếu sai hoặc tên file không có phần mở rộng
 */
static bool Sdcard_HasExtension(const char *name, const char *ext)
{
    const char *lDot = strrchr(name, '.');
    if (lDot == NULL)
    {
        return false;
    }

    return (strcasecmp(lDot, ext) == 0);
}

/**
 * @brief Sdcard_RemoveExtension
 * Copy tên file sang dest, bỏ phần mở rộng (phần từ dấu chấm cuối cùng trở đi)
 * @param pDest: buffer đích để lưu kết quả, kích thước destSize byte
 * @param destSize: kích thước pDest, tránh tràn bộ nhớ khi tên file dài bất thường
 * @param pSrc: tên file nguồn bao gồm extension (vd "song.mp3")
 * @return
 */
static void Sdcard_RemoveExtension(char *pDest, size_t destSize, const char *pSrc)
{
    char *lDot;

    /* Copy có giới hạn kích thước, luôn đảm bảo kết thúc bằng '\0' */
    snprintf(pDest, destSize, "%s", pSrc);

    lDot = strrchr(pDest, '.');
    if (lDot != NULL)
    {
        *lDot = '\0';
    }
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Sdcard_Init
 * Khởi tạo module Sdcard: tạo xSdCommandQueue để các module khác (double_buffer.c) gửi
 * lệnh vào. Gọi trước khi tạo Sdcard_Task.
 * @param
 * @return
 */
void Sdcard_Init(void)
{
    xSdCommandQueue = xQueueCreate(SDCARD_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
}

/**
 * @brief Sdcard_Mount
 * Mount thẻ nhớ SD - chỉ lo phần CHUNG (đường dẫn mount, cấu hình FAT qua gSdcardMountConfig -
 * định nghĩa sẵn trong sdcard_config.c, xem sdcard_config.h), KHÔNG biết chi tiết chân/bus
 * phần cứng thật sự dùng để giao tiếp với
 * thẻ - phần đó nằm trong driver tương ứng, CHỌN ĐÚNG 1 NHÁNH THEO SDCARD_INTERFACE
 * (sdcard_config.h) NGAY LÚC BIÊN DỊCH (tiền xử lý #if, không phải if runtime) - nhánh không
 * được chọn KHÔNG được biên dịch vào firmware (không sinh code thừa, không tăng dung lượng
 * .bin cho driver không dùng tới). Nếu SDCARD_INTERFACE mang giá trị chưa có driver tương
 * ứng, #error chặn biên dịch ngay (in thẳng ra log build) thay vì để lỗi trôi tới runtime.
 * Thêm giao diện mới (vd SPI/QSPI): viết driver cùng khuôn mẫu Sdmmc_Init()/Sdmmc_Mount()
 * (vd driver/sdspi/sdspi.c với Sdspi_Init()/Sdspi_Mount()), rồi thêm 1 nhánh #elif tương ứng
 * bên dưới. Nhánh SDMMC hiện tại YÊU CẦU Sdmmc_Init() (driver/sdmmc/sdmmc.c) đã được gọi
 * thành công từ trước trong app_main(), TRƯỚC KHI tạo Sdcard_Task.
 * @param
 * @return ESP_OK nếu mount thành công
 */
esp_err_t Sdcard_Mount(void)
{
    sdmmc_card_t *pCard;

#if (SDCARD_INTERFACE == SDCARD_INTERFACE_SDMMC)
    esp_err_t lRet = Sdmmc_Mount(SDCARD_MOUNT_POINT, &gSdcardMountConfig, &pCard);
#elif (SDCARD_INTERFACE == SDCARD_INTERFACE_SPI)
    #error "SDCARD_INTERFACE_SPI chua duoc trien khai - viet driver/sdspi/sdspi.c (Sdspi_Mount cung chu ky Sdmmc_Mount), roi doi nhanh #elif nay thanh loi goi Sdspi_Mount()"
#else
    #error "SDCARD_INTERFACE (config/hardware/sdcard_config.h) dang mang gia tri khong hop le/chua duoc ho tro - chi chap nhan SDCARD_INTERFACE_SDMMC hoac SDCARD_INTERFACE_SPI (sau khi da trien khai)"
#endif

    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed");
        return lRet;
    }

    ESP_LOGI(TAG, "SD card mounted");

    /* In thông tin thẻ nhớ ra console */
    sdmmc_card_print_info(stdout, pCard);

    return ESP_OK;
}

/**
 * @brief Sdcard_ScanAndCreateDb
 * Quét toàn bộ thư mục gốc thẻ nhớ, tìm các file .mp3 có file .bin (frame animation)
 * đi kèm, ghi thành file database "/sdcard/songs.db" và nạp tên bài hát vào gaSongNameList
 * @param basePath: thư mục gốc cần quét (vd "/sdcard")
 * @return
 */
void Sdcard_ScanAndCreateDb(const char *basePath)
{
    DIR *pDir;
    FILE *pDb;
    struct dirent *pEntry;

    pDir = opendir(basePath);
    if (pDir == NULL)
    {
        printf("Cannot open dir\n");
        return;
    }

    pDb = fopen(SDCARD_DB_PATH, "wb");
    if (pDb == NULL)
    {
        printf("Cannot open db file\n");
        closedir(pDir);
        return;
    }

    /* Duyệt từng entry trong thư mục cho tới khi hết file hoặc đầy gaSongNameList */
    while (((pEntry = readdir(pDir)) != NULL) && (gsPlayerContext.totalSong < SDCARD_MAX_SONGS))
    {
        char lName[64];
        /* Tên file KHÔNG bao gồm extension, dùng để dựng lMp3Path/lBinPath - kích thước bằng
           đúng songPath/framePath (Sdcard_SongDbType_s, sdcard.h) thay vì dùng chung lName[64]
           (chỉ đủ cho HIỂN THỊ trên menu OLED, xem Menu_Draw trong menu.c chỉ hiện tối đa 27
           ký tự/dòng). Nếu dùng lName (đã bị cắt còn 63 ký tự) để dựng lại đường dẫn, file có
           tên gốc dài hơn 63 ký tự sẽ bị dựng SAI đường dẫn (không khớp file thật trên thẻ),
           khiến fopen(lBinPath) thất bại và bài hát bị loại âm thầm dù có đủ cặp .mp3/.bin
           hợp lệ - lBaseName giữ đủ độ dài gốc (trừ phần snprintf tự cắt an toàn nếu tên thật
           sự vượt quá 128 ký tự, cực hiếm) để tránh đúng lỗi đó */
        char lBaseName[128];
        char lMp3Path[128];
        char lBinPath[128];
        FILE *pBinFile;
        Sdcard_SongDbType_s lRecord;

        printf("Found: %s\n", pEntry->d_name);

        /* Chỉ lấy file .mp3, bỏ qua file khác */
        if (!Sdcard_HasExtension(pEntry->d_name, ".mp3"))
        {
            continue;
        }

        Sdcard_RemoveExtension(lBaseName, sizeof(lBaseName), pEntry->d_name);
        /* lName riêng cho hiển thị (có thể cắt ngắn hơn lBaseName) - không ảnh hưởng đường dẫn
           thật đã dựng từ lBaseName ở trên/dưới */
        Sdcard_RemoveExtension(lName, sizeof(lName), pEntry->d_name);

        snprintf(lMp3Path, sizeof(lMp3Path), "%s/%s.mp3", basePath, lBaseName);
        snprintf(lBinPath, sizeof(lBinPath), "%s/%s.bin", basePath, lBaseName);

        /* Bài hát chỉ hợp lệ nếu có đủ file .bin (frame animation) đi kèm */
        pBinFile = fopen(lBinPath, "rb");
        if (pBinFile == NULL)
        {
            printf("Missing bin for %s\n", lName);
            continue;
        }
        fclose(pBinFile);

        /* Ghi 1 bản ghi vào database */
        memset(&lRecord, 0, sizeof(lRecord));
        snprintf(lRecord.songPath, sizeof(lRecord.songPath), "%s", lMp3Path);
        snprintf(lRecord.framePath, sizeof(lRecord.framePath), "%s", lBinPath);
        fwrite(&lRecord, sizeof(Sdcard_SongDbType_s), 1, pDb);

        /* Lưu song song vào RAM để menu hiển thị, không cần đọc lại file mỗi lần vẽ */
        snprintf(gaSongNameList[gsPlayerContext.totalSong].songName,
                 sizeof(gaSongNameList[gsPlayerContext.totalSong].songName), "%s", lName);

        #if defined DEBUG_PRINTF_ENABLED
            printf("Added: %s\n", lName);
        #endif

        gsPlayerContext.totalSong++;
    }

    fclose(pDb);
    closedir(pDir);

    printf("Total songs: %u\n", (unsigned int)gsPlayerContext.totalSong);
}

/**
 * @brief Sdcard_ReadDbFile
 * In toàn bộ nội dung file database "/sdcard/songs.db" ra console (debug)
 * @param
 * @return
 */
void Sdcard_ReadDbFile(void)
{
    FILE *pDb;
    Sdcard_SongDbType_s lRecord;

    /* Hàm này luôn được biên dịch (dù chỉ hữu ích lúc debug) vì sdcard.h khai báo và
       audio.c gọi không điều kiện - nếu bọc cả hàm trong #if DEBUG_PRINTF_ENABLED như
       bản gốc, build release (không định nghĩa DEBUG_PRINTF_ENABLED) sẽ lỗi linker vì
       gọi tới hàm không tồn tại. Chỉ nội dung in ra mới cần thiết cho debug, nên chỉ bọc
       phần đó nếu muốn, còn bản thân hàm luôn tồn tại. */
    pDb = fopen(SDCARD_DB_PATH, "rb");
    if (pDb == NULL)
    {
        printf("Cannot open db\n");
        return;
    }

    while (fread(&lRecord, sizeof(Sdcard_SongDbType_s), 1, pDb) == 1)
    {
        printf("MP3: %s\n", lRecord.songPath);
        printf("BIN: %s\n", lRecord.framePath);
    }

    fclose(pDb);
}

/**
 * @brief Sdcard_GetSongByIndex
 * Đọc 1 bản ghi trong file database "/sdcard/songs.db" theo chỉ số
 * @param index: chỉ số bài hát cần lấy (0..gsPlayerContext.totalSong-1)
 * @param pOut: struct nhận thông tin bài hát
 * @return E_OK nếu lấy thành công, E_NOT_OK nếu lỗi (index ngoài phạm vi, không mở được file...)
 */
Std_ReturnType Sdcard_GetSongByIndex(uint16_t index, Sdcard_SongDbType_s *pOut)
{
    FILE *pDb;
    long lOffset;

    if ((index >= gsPlayerContext.totalSong) || (pOut == NULL))
    {
        return E_NOT_OK;
    }

    pDb = fopen(SDCARD_DB_PATH, "rb");
    if (pDb == NULL)
    {
        printf("Open DB failed\n");
        return E_NOT_OK;
    }

    /* Nhảy thẳng tới bản ghi thứ index, không cần đọc tuần tự từ đầu */
    lOffset = (long)index * (long)sizeof(Sdcard_SongDbType_s);
    if (fseek(pDb, lOffset, SEEK_SET) != 0)
    {
        printf("fseek failed\n");
        fclose(pDb);
        return E_NOT_OK;
    }

    if (fread(pOut, sizeof(Sdcard_SongDbType_s), 1, pDb) != 1)
    {
        printf("fread failed\n");
        fclose(pDb);
        return E_NOT_OK;
    }

    fclose(pDb);
    return E_OK;
}

/**
 * @brief Sdcard_Task
 * Task owner duy nhất của thẻ nhớ SD (kiến trúc Owner Task, xem srm.h). Nạp dữ liệu frame
 * animation từ thẻ nhớ vào double buffer, VÀ dữ liệu mp3 thô vào xMp3RingBuffer, đều cho
 * bài đang phát - Oled_Task/Mp3_Task chỉ đọc dữ liệu qua DoubleBuffer_GetFrame()/
 * xMp3RingBuffer, không tự đụng vào thẻ SD. Cùng khuôn thuật toán với Oled_Task (oled.c) để
 * các task trong hệ thống đọc thống nhất, dễ theo dõi:
 *
 * Cơ chế nhận sự kiện:
 * - Không tự poll trạng thái theo chu kỳ, mà "ngủ" (xTaskNotifyWait, portMAX_DELAY) chờ
 *   PlayerManager_Task gửi task notification, tiết kiệm CPU khi không có gì thay đổi và
 *   phản ứng ngay lập tức khi có sự kiện.
 * - Giá trị notification nhận được chính là gsPlayerContext.buttonState (kiểu
 *   PlayerManager_ButtonStateType_e) tại thời điểm PlayerManager_Task gửi đi -> không cần
 *   thêm 1 enum sự kiện riêng cho kênh notification giữa 2 task.
 * - Switch bên dưới xử lý trực tiếp trên buttonState, không qua enum trung gian.
 *
 * 2 nhánh xử lý theo buttonState:
 * - BTN_STATE_NEXT/PREV/PLAY_NEW: đổi bài (Next/Prev khi đang phát, hoặc chọn bài mới từ MENU)
 *   -> chạy Sdcard_LoadSong() mở lại double buffer + file mp3, nạp dữ liệu cho bài mới.
 * - Các buttonState còn lại: sự kiện không liên quan tới việc đổi bài (di chuyển cursor MENU,
 *   play/pause, peek MENU...) -> bỏ qua, không làm gì.
 *
 * Cơ chế lbHasPendingNotify: Sdcard_LoadSong() có thể thoát sớm vì vừa nhận 1
 * notification mới (thay vì thoát vì lỗi đọc DB) và đã đọc giá trị đó vào lu32button_evt.
 * Nếu vòng lặp ngoài gọi xTaskNotifyWait() lần nữa ngay lúc đó thì giá trị vừa nhận sẽ bị
 * mất (thay bằng giá trị chờ tiếp theo). lbHasPendingNotify = true báo cho vòng lặp biết:
 * bỏ qua bước chờ, xử lý ngay lu32button_evt đang có sẵn ở vòng lặp kế tiếp.
 *
 * Mount + quét thẻ SD (Sdcard_Mount/Sdcard_ScanAndCreateDb/Sdcard_ReadDbFile) cũng chạy ngay
 * lúc khởi động task này - trước đây app_main làm việc này TRƯỚC KHI tạo task nào (đồng bộ,
 * chặn cả hệ thống), giờ chuyển vào đây cùng khuôn mẫu Mp3_Task tự gọi vs1053_init() lúc
 * khởi động (xem mp3.c). Kết quả báo cho Oled_Task qua Srm_OledNotifyBootStatus() (xem srm.c)
 * để hiển thị lỗi lên màn hình nếu cần - Sdcard_Task không được phép tự vẽ lên SSD1306
 * (Oled_Task mới là owner, xem kiến trúc Owner Task trong srm.h).
 *
 * @param pvParameters
 * @return không bao giờ return (vòng lặp vô hạn, đúng chuẩn 1 FreeRTOS task)
 */
void Sdcard_Task(void *pvParameters)
{
    (void)pvParameters;

    /* Giá trị notification nhận từ PlayerManager_Task, thực chất là gsPlayerContext.buttonState
       (kiểu PlayerManager_ButtonStateType_e) tại thời điểm gửi, ép kiểu lại khi cần dùng */
    uint32_t lu32button_evt;
    /* true = bỏ qua bước chờ notify ở vòng lặp kế tiếp, vì lu32button_evt đã có sẵn
       1 giá trị mới cần xử lý ngay (do Sdcard_LoadSong vừa trả về) */
    bool lbHasPendingNotify = false;

    /* Tạo xSdCommandQueue TRƯỚC Sdcard_Mount() - đây là phần nhanh (chỉ cấp phát RAM), làm
       trước để sẵn sàng càng sớm càng tốt, không phải chờ Sdcard_Mount() (chậm - I/O phần
       cứng) chạy xong mới có. Được đụng tới lần đầu qua Srm_SdcardGetSingleFrame(), CHỈ gọi
       trong Oled_PlayAnimation() - luôn cần người dùng bấm Play/Next/Prev trước, độ trễ phản
       xạ người dùng dư sức lớn hơn thời gian dòng lệnh dưới đây chạy xong (cùng lý do
       Mp3_Init() an toàn khi gọi trong Mp3_Task, xem mp3.c) */
    Sdcard_Init();

    /* Mount + quét thẻ SD lúc khởi động - chỉ scan/đọc DB khi mount thành công, mount fail
       thì "/sdcard" không truy cập được, gọi tiếp cũng chỉ tự thất bại vô ích
       (Sdcard_ScanAndCreateDb tự log "Cannot open dir" rồi return, không crash) */
    esp_err_t lMountRet = Sdcard_Mount();
    if (lMountRet == ESP_OK)
    {
        Sdcard_ScanAndCreateDb(SDCARD_MOUNT_POINT);
        Sdcard_ReadDbFile();
    }

    /* Báo Oled_Task trạng thái mount/quét (OK/lỗi mount/không có bài hát) để hiển thị lỗi lên
       màn hình nếu cần, trước khi Oled_Task vẽ menu lần đầu (xem Oled_Task, oled.c) */
    if (lMountRet != ESP_OK)
    {
        Srm_OledNotifyBootStatus(OLED_BOOT_STATUS_SD_ERROR);
    }
    else if (gsPlayerContext.totalSong == 0)
    {
        Srm_OledNotifyBootStatus(OLED_BOOT_STATUS_NO_SONGS);
    }
    else
    {
        Srm_OledNotifyBootStatus(OLED_BOOT_STATUS_OK);
    }

    /* Khởi tạo double buffer trước khi dùng tới DoubleBuffer_LoadAll()/DoubleBuffer_GetFrame() */
    DoubleBuffer_Init();

    /* Vòng lặp chính của task, chạy vô hạn cho tới khi thiết bị tắt nguồn */
    while (1)
    {
        /* Check xem có notify nào đang pending không? nếu không thì chờ notify mới,
           chờ vô thời hạn (portMAX_DELAY) vì task không có việc gì khác để làm */
        if (lbHasPendingNotify == false)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32button_evt, portMAX_DELAY);
        }
        /* Dùng xong cờ pending cho vòng lặp này -> reset về false, chỉ set lại true
           bên dưới nếu Sdcard_LoadSong() báo còn notify chưa xử lý */
        lbHasPendingNotify = false;

        /* Sdcard_Task chỉ thực sự quan tâm sự kiện ĐỔI BÀI (Next/Prev lúc đang phát, hoặc
           chọn bài mới từ MENU); các buttonState khác (di chuyển cursor MENU, play/pause...)
           không làm thay đổi bài đang cần nạp dữ liệu nên bỏ qua */
        switch ((PlayerManager_ButtonStateType_e)lu32button_evt)
        {
            case BTN_STATE_NEXT:
            case BTN_STATE_PREV:
            case BTN_STATE_PLAY_NEW:
                /* Chạy vòng nạp dữ liệu cho bài mới; nếu bị ngắt giữa chừng do có notify
                   khác tới thì lbHasPendingNotify sẽ được set true để xử lý tiếp ở vòng
                   lặp kế, không mất giá trị notification vừa nhận */
                lbHasPendingNotify = Sdcard_LoadSong(&lu32button_evt);
                break;

            case BTN_STATE_UP:
            case BTN_STATE_DOWN:
            case BTN_STATE_BACK_MENU:
            case BTN_STATE_PLAY:
            case BTN_STATE_PAUSE:
            case BTN_STATE_IDLE:
            default:
                /* Sự kiện không liên quan tới đổi bài -> bỏ qua */
                break;
        }
    }
}
