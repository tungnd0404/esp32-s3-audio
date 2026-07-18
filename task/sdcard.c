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
#include "pcm_player.h"
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
   CPU cho task khác (Pcm_Task, Oled_Task...) thay vì busy-poll liên tục, vì xQueueReceive
   trên xSdCommandQueue giờ non-blocking (xem Sdcard_ServicePendingCommand) không còn tự
   chờ hộ vòng lặp nữa - giống hệt OLED_ANIMATION_DELAY_MS bên oled.c */
#define SDCARD_LOAD_WAIT_MS         50U

/* Số request tối đa có thể chờ xử lý trong xSdCommandQueue cùng lúc */
#define SDCARD_COMMAND_QUEUE_LENGTH 4U

/* Kích thước mỗi lần đọc file .pcm để nạp vào xPcmRingBuffer (byte). PCM 16-bit stereo
   44.1kHz cần thông lượng ~172KB/s (gấp ~10 lần MP3 128kbps trước đây), nên tăng hẳn kích
   thước đọc mỗi lần (512 byte cũ -> 4096 byte) để giảm số lần gọi fread() trên thẻ SD tương
   ứng - việc tách "đọc thẻ SD theo khối lớn" khỏi "ghi cho I2S theo khối vừa" chính là lý do
   cần xPcmRingBuffer làm lớp đệm trung gian */
#define SDCARD_PCM_READ_CHUNK_SIZE  4096U

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

/* File .pcm của bài ĐANG THỰC SỰ PHÁT, chỉ Sdcard_Task được đụng vào (kiến trúc Owner Task -
   Pcm_Task không tự fopen/fread thẳng trên thẻ SD, xem Pcm_StreamSong trong pcm_player.c).
   Đóng và mở lại mỗi khi đổi bài (xem Sdcard_LoadSong), KHÔNG đóng lúc Pause - giữ nguyên vị
   trí đọc dở để Resume tiếp tục đúng chỗ. NULL khi chưa có bài nào đang mở. */
static FILE *gpPcmFile = NULL;

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
 * Pcm_ServicePendingCommand() (pcm_player.c) và Oled_ServicePendingCommand() (oled.c).
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
 * @brief Sdcard_FillPcmRingBuffer
 * Đọc thêm dữ liệu từ gpPcmFile (nếu đang mở và chưa hết file) nạp vào xPcmRingBuffer cho
 * tới khi KHÔNG còn đủ chỗ trống cho 1 khối SDCARD_PCM_READ_CHUNK_SIZE nữa, hoặc gặp EOF.
 * Gọi mỗi vòng lặp trong Sdcard_LoadSong() để xPcmRingBuffer luôn được nạp gần đầy -
 * khác với frame animation (Oled_Task chủ động xin từng frame qua SDCARD_CMD_GET_SINGLE_FRAME,
 * xem DoubleBuffer_GetFrame), ring buffer không cần Pcm_Task báo hiệu mới nạp: cứ còn chỗ
 * trống là nạp tiếp, đơn giản hơn vì đây là luồng dữ liệu tuần tự (không cần truy cập ngẫu
 * nhiên theo chỉ số như frame animation).
 * @param
 * @return
 */
static void Sdcard_FillPcmRingBuffer(void)
{
    if ((gpPcmFile == NULL) || (gbPcmStreamEof == true))
    {
        return;
    }

    /* static (KHÔNG phải local trên stack) - 4096 byte gần như chiếm hết sạch
       AUDIO_TASK_STACK_SIZE (4096 byte, main/audio.c) nếu để trên stack của Sdcard_Task,
       không còn chỗ cho phần còn lại của call chain (fread(), các local khác...) -> tràn stack
       thật sự (đã quan sát được trên board thật khi tăng SDCARD_PCM_READ_CHUNK_SIZE từ 512
       lên 4096 mà quên đổi chỗ cấp phát). An toàn khi để static vì hàm này CHỈ được gọi từ
       chính Sdcard_Task (không có 2 lời gọi chồng nhau cùng lúc) - cùng nguyên tắc buffer lớn
       của double_buffer.c/ring_buffer.c/oled.c (gau8Frame) */
    static uint8_t lau8Chunk[SDCARD_PCM_READ_CHUNK_SIZE];

    /* Kiểm tra chỗ trống TRƯỚC khi đọc file - đảm bảo RingBuffer_Write() bên dưới luôn có
       đủ chỗ để ghi hết số byte vừa đọc được, không bao giờ phải huỷ dữ liệu đã đọc dở */
    while (RingBuffer_GetFreeSize(xPcmRingBuffer) >= SDCARD_PCM_READ_CHUNK_SIZE)
    {
        size_t lReadBytes = fread(lau8Chunk, 1, SDCARD_PCM_READ_CHUNK_SIZE, gpPcmFile);
        if (lReadBytes == 0)
        {
            /* fread() trả 0 có thể là HẾT FILE THẬT (feof()) hoặc LỖI ĐỌC THẬT SỰ (ferror() -
               vd rút thẻ SD giữa chừng) - 2 tình huống khác nhau nhưng trước đây bị xử lý y hệt
               nhau (chỉ set gbPcmStreamEof = true), khiến người dùng không hề biết thẻ đã bị
               rút, chỉ thấy nhạc dừng êm như hết bài bình thường. Phân biệt bằng ferror() - nếu
               đúng là lỗi đọc, báo cho Oled_Task hiển thị lỗi qua kênh SRM sẵn có (dùng lại
               đúng OLED_BOOT_STATUS_SD_ERROR/Srm_OledNotifyBootStatus() vốn đã dùng lúc boot -
               không tạo thêm command/status code mới, không phá vỡ kiến trúc Owner Task/SRM).
               gbPcmStreamEof vẫn phải set true trong CẢ 2 trường hợp - dù EOF thật hay lỗi đọc,
               Sdcard_Task đều không còn gì để nạp thêm cho bài này nữa */
            if (ferror(gpPcmFile))
            {
                ESP_LOGE(TAG, "Read error on pcm stream (SD card removed/faulty?)");
                Srm_OledNotifyBootStatus(OLED_BOOT_STATUS_SD_ERROR);
            }

            gbPcmStreamEof = true;
            break;
        }

        /* Free size đã được đảm bảo đủ chỗ ngay trước vòng lặp này (chỉ Sdcard_Task tự ghi,
           không ai khác giành chỗ ghi cùng lúc - xem hợp đồng 1-gửi-1-nhận trong
           ring_buffer.h) nên về lý thuyết write() luôn thành công; vẫn log cảnh báo nếu
           không, thay vì âm thầm mất dữ liệu PCM không rõ lý do */
        if (RingBuffer_Write(xPcmRingBuffer, lau8Chunk, lReadBytes, 0) == E_NOT_OK)
        {
            ESP_LOGW(TAG, "RingBuffer_Write failed despite free size check (%u bytes)", (unsigned int)lReadBytes);
        }
    }
}

/**
 * @brief Sdcard_LoadSong
 * Mở double buffer + file .pcm cho bài hát ĐANG THỰC SỰ PHÁT (gsPlayerContext.currentSong)
 * rồi liên tục: (1) xử lý lệnh SDCARD_CMD_GET_SINGLE_FRAME gửi tới qua xSdCommandQueue mỗi khi
 * Oled_Task cần 1 frame animation (xem Sdcard_HandleCommand), và
 * (2) nạp thêm dữ liệu PCM vào xPcmRingBuffer cho Pcm_Task rút ra phát (xem
 * Sdcard_FillPcmRingBuffer) - cho tới khi có bài mới cần chuyển sang. Cùng khuôn mẫu với
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

    /* Đóng file .pcm của bài cũ (nếu có) trước khi mở file mới. KHÔNG tự RingBuffer_Reset()
       xPcmRingBuffer ở đây dù có vẻ hợp lý (Sdcard_Task là bên chủ động biết "dữ liệu cũ cần
       xoá") - Sdcard_Task là bên GHI của ring buffer này, còn RingBuffer_Reset() bản chất là
       thao tác NHẬN; gọi nhầm phía sẽ đụng độ với Pcm_Task (bên nhận thật sự) nếu nó đang
       mid-RingBuffer_Read() cùng lúc - xem cảnh báo chi tiết trong ring_buffer.h. Việc dọn
       sạch dữ liệu cũ trong xPcmRingBuffer do chính Pcm_Task tự làm khi nhận CÙNG notification
       này (xem case BTN_STATE_NEXT/PREV/PLAY_NEW trong Pcm_Task, pcm_player.c) */
    if (gpPcmFile != NULL)
    {
        fclose(gpPcmFile);
        gpPcmFile = NULL;
    }
    gbPcmStreamEof = false;

    gpPcmFile = fopen(lSong.songPath, "rb");
    if (gpPcmFile == NULL)
    {
        /* Không mở được file .pcm -> coi như hết dữ liệu ngay, để Pcm_Task không chờ vô ích
           (vẫn tiếp tục chạy vòng lặp bên dưới bình thường cho phần frame animation) */
        ESP_LOGE(TAG, "Cannot open pcm %s", lSong.songPath);
        gbPcmStreamEof = true;
    }
    else
    {
        /* [DEBUG TẠM] in kích thước file .pcm thật ngay lúc mở - nếu ra 0 hoặc rất nhỏ, xác
           nhận đúng nghi vấn file rỗng/gần rỗng (khiến Pcm_StreamSong() thoát ngay do EOF thật,
           không phải lỗi) thay vì đoán. XOÁ khối debug này (4 dòng) sau khi xác định xong
           nguyên nhân. */
        fseek(gpPcmFile, 0, SEEK_END);
        long lFileSize = ftell(gpPcmFile);
        fseek(gpPcmFile, 0, SEEK_SET);
        ESP_LOGW(TAG, "[DEBUG] Opened %s, file size = %ld bytes", lSong.songPath, lFileSize);
    }

    /* Vòng lặp nạp dữ liệu cho bài đang phát, chạy tới khi có notification mới (đổi bài) -
       giống hệt Oled_PlayAnimation()/Pcm_StreamSong(): check non-blocking notify trước, tranh
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

        /* Tranh thủ nạp thêm dữ liệu PCM cho Pcm_Task mỗi vòng lặp - không cần chờ yêu cầu
           gì từ Pcm_Task, cứ còn chỗ trống trong xPcmRingBuffer là nạp tiếp (xem
           Sdcard_FillPcmRingBuffer) */
        Sdcard_FillPcmRingBuffer();

        /* Không còn tác vụ nào tự chờ (queue receive đã chuyển non-blocking ở trên) -> phải
           tự delay để nhường CPU cho task khác (Pcm_Task, Oled_Task...) thay vì busy-poll
           liên tục, giống hệt Oled_PlayAnimation() */
        vTaskDelay(pdMS_TO_TICKS(SDCARD_LOAD_WAIT_MS));
    }
}

/**
 * @brief Sdcard_HasExtension
 * Kiểm tra tên file có đúng phần mở rộng cần tìm không
 * @param name: tên file bao gồm extension (vd "song.pcm")
 * @param ext: phần mở rộng cần so khớp, bao gồm dấu chấm (vd ".pcm")
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
 * @param pSrc: tên file nguồn bao gồm extension (vd "song.pcm")
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
 * Quét toàn bộ thư mục gốc thẻ nhớ, tìm các file .pcm có file .bin (frame animation)
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
        /* Tên file KHÔNG bao gồm extension, dùng để dựng lPcmPath/lBinPath - kích thước bằng
           đúng songPath/framePath (Sdcard_SongDbType_s, sdcard.h) thay vì dùng chung lName[64]
           (chỉ đủ cho HIỂN THỊ trên menu OLED, xem Menu_Draw trong menu.c chỉ hiện tối đa 27
           ký tự/dòng). Nếu dùng lName (đã bị cắt còn 63 ký tự) để dựng lại đường dẫn, file có
           tên gốc dài hơn 63 ký tự sẽ bị dựng SAI đường dẫn (không khớp file thật trên thẻ),
           khiến fopen(lBinPath) thất bại và bài hát bị loại âm thầm dù có đủ cặp .pcm/.bin
           hợp lệ - lBaseName giữ đủ độ dài gốc (trừ phần snprintf tự cắt an toàn nếu tên thật
           sự vượt quá 128 ký tự, cực hiếm) để tránh đúng lỗi đó */
        char lBaseName[128];
        char lPcmPath[128];
        char lBinPath[128];
        FILE *pBinFile;
        Sdcard_SongDbType_s lRecord;

        printf("Found: %s\n", pEntry->d_name);

        /* Chỉ lấy file .pcm, bỏ qua file khác */
        if (!Sdcard_HasExtension(pEntry->d_name, ".pcm"))
        {
            continue;
        }

        Sdcard_RemoveExtension(lBaseName, sizeof(lBaseName), pEntry->d_name);
        /* lName riêng cho hiển thị (có thể cắt ngắn hơn lBaseName) - không ảnh hưởng đường dẫn
           thật đã dựng từ lBaseName ở trên/dưới */
        Sdcard_RemoveExtension(lName, sizeof(lName), pEntry->d_name);

        /* basePath la con tro khong ro do dai voi GCC nen -Wformat-truncation canh bao
           lPcmPath/lBinPath co the bi cat - da chap nhan cat an toan (xem comment lBaseName
           o tren), tat canh bao cuc bo thay vi tang buffer khong can thiet */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(lPcmPath, sizeof(lPcmPath), "%s/%s.pcm", basePath, lBaseName);
        snprintf(lBinPath, sizeof(lBinPath), "%s/%s.bin", basePath, lBaseName);
#pragma GCC diagnostic pop

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
        snprintf(lRecord.songPath, sizeof(lRecord.songPath), "%s", lPcmPath);
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
        printf("PCM: %s\n", lRecord.songPath);
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
 * animation từ thẻ nhớ vào double buffer, VÀ dữ liệu PCM thô vào xPcmRingBuffer, đều cho
 * bài đang phát - Oled_Task/Pcm_Task chỉ đọc dữ liệu qua DoubleBuffer_GetFrame()/
 * xPcmRingBuffer, không tự đụng vào thẻ SD. Cùng khuôn thuật toán với Oled_Task (oled.c) để
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
 *   -> chạy Sdcard_LoadSong() mở lại double buffer + file .pcm, nạp dữ liệu cho bài mới.
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
 * chặn cả hệ thống), giờ chuyển vào đây cùng khuôn mẫu Pcm_Task tự gọi Max98357a_Init() lúc
 * khởi động (xem pcm_player.c). Kết quả báo cho Oled_Task qua Srm_OledNotifyBootStatus() (xem
 * srm.c) để hiển thị lỗi lên màn hình nếu cần - Sdcard_Task không được phép tự vẽ lên SSD1306
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
       xạ người dùng dư sức lớn hơn thời gian dòng lệnh dưới đây chạy xong */
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
