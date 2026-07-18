#ifndef SRM_H
#define SRM_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "std_types.h"

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* SRM (Shared Resource Manager): bộ API dùng chung cho kiến trúc Owner Task - 1 task sở
   hữu 1 tài nguyên dùng chung (vd Pcm_Task sở hữu kênh I2S), các task khác không được đụng
   thẳng vào tài nguyên đó mà phải gửi lệnh qua command queue của owner. SRM cũng là nơi
   quản lý tập trung mã lệnh (cmdId) của TẤT CẢ tài nguyên dùng chung trong hệ thống - mỗi
   tài nguyên mới thêm vào sau này đều khai báo enum lệnh riêng của nó ngay tại đây.

   Mỗi lệnh có 1 hàm gửi riêng (Srm_<Owner><Lệnh>, xem GLOBAL FUNCTION bên dưới) thay vì 1
   hàm gửi chung nhận cmdId + con trỏ payload kiểu uint32_t* - vì không phải lệnh nào cũng
   chỉ cần trao đổi đúng 4 byte (vd SDCARD_CMD_GET_SINGLE_FRAME cần nhận về nguyên 1 frame 1024
   byte). Tham số/kiểu trả về của mỗi hàm khớp đúng bản chất dữ liệu của lệnh đó, không phải
   ép hết về uint32_t/void* chung chung ở nơi gọi. Cơ chế gửi/nhận generic (queue + registry
   response queue theo từng task) vẫn dùng chung, chỉ ẩn sau các hàm riêng này. */

/* --- Lệnh cho tài nguyên I2S/PCM (owner: Pcm_Task, xem task/pcm_player.h/pcm_player.c) --- */
/* --- Lệnh cho tài nguyên double buffer animation (owner: Sdcard_Task, xem task/sdcard.c) --- */
/* --- Lệnh cho tài nguyên SSD1306 (owner: Oled_Task, xem task/oled.c) --- */
typedef enum {
    PCM_CMD_GET_PLAYED_SAMPLES,
    SDCARD_CMD_GET_SINGLE_FRAME,
    OLED_CMD_SHOW_STATUS,

    /* Luôn đặt cuối cùng - không phải lệnh thật, dùng để validate cmdId nhận được ở phía
       owner (Mp3_HandleCommand/Sdcard_HandleCommand) có hợp lệ hay không */
    SRM_CMD_INVALID
} Srm_CommandType_e;

/* Phân loại nội dung message */
typedef enum {
    SRM_MSG_COMMAND,   /* Yêu cầu owner task thực hiện hành động / trả dữ liệu */
    SRM_MSG_DATA       /* Dữ liệu thuần, dùng cho response */
} Srm_MsgKindType_e;

/* Message chung cho MỌI command queue theo kiến trúc Owner Task, không riêng gì 1 module -
   chỉ dùng NỘI BỘ bởi các hàm Srm_<Owner><Lệnh> (bên gửi) và Srm_Reply (bên owner trả lời),
   module ngoài SRM không tự dựng struct này. 2 kênh mang dữ liệu:
   - payload: giá trị vô hướng nhỏ (4 byte) - đủ cho phần lớn lệnh (chỉ số frame lúc gửi,
     kết quả thành công/thất bại hoặc thời gian giải mã lúc trả lời...)
   - pData: con trỏ dữ liệu LỚN HƠN 4 byte (vd buffer 1024 byte của 1 frame animation), NULL
     nếu lệnh không cần. Do bên gửi sở hữu (biến local/tham số của bên gửi) - an toàn vì
     Srm_<Owner><Lệnh> block bên gửi cho tới khi nhận phản hồi, con trỏ luôn còn sống suốt
     thời gian owner xử lý, owner ghi thẳng vào đó thay vì phải copy qua lại qua queue */
typedef struct {
    /* COMMAND hay DATA */
    Srm_MsgKindType_e kind;
    /* Mã lệnh, định nghĩa riêng theo từng owner task (Srm_CommandType_e) */
    uint32_t cmdId;
    /* Dữ liệu vô hướng nhỏ đi kèm: tham số của lệnh (khi kind == COMMAND) hoặc kết quả trả
       về - E_OK/E_NOT_OK (khi kind == DATA) */
    uint32_t payload;
    /* Con trỏ dữ liệu lớn hơn đi kèm (khi lệnh cần), NULL nếu không cần - xem giải thích ở
       trên. Chỉ có ý nghĩa khi kind == COMMAND (bên gửi truyền vào); owner ghi thẳng dữ liệu
       trả về vào đây (nếu có) trước khi gọi Srm_Reply(), không dùng field này ở chiều
       response (payload đã đủ cho kết quả 4 byte) */
    void *pData;
    /* Queue trả lời, chỉ dùng khi kind == SRM_MSG_COMMAND và cần response.
       NULL nếu không cần phản hồi. Do bên gửi tự tạo và sở hữu - mỗi bên gửi cần 1 kênh
       response riêng, không dùng chung giữa nhiều bên gửi khác nhau. */
    QueueHandle_t responseQueue;
} Srm_Message_s;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Srm_Init
 * Khởi tạo module SRM: tạo mutex bảo vệ registry (ánh xạ task -> response queue riêng,
 * xem srm.c). PHẢI gọi đúng 1 lần trong app_main(), TRƯỚC khi bất kỳ task nào được tạo
 * (xTaskCreatePinnedToCore/xTaskCreate) - lúc đó chỉ có 1 luồng thực thi duy nhất nên tạo
 * mutex không cần đồng bộ hoá gì thêm. Nếu tạo mutex kiểu lazy (tạo khi cần bên trong hàm
 * gửi lệnh) thì 2 task gọi lần đầu cùng lúc có thể cùng thấy mutex chưa tồn tại và cùng tạo,
 * ra 2 mutex khác nhau - không bảo vệ được gì cả.
 * @param
 * @return
 */
void Srm_Init(void);

/**
 * @brief Srm_Reply
 * Owner task dùng để trả lời 1 request đã nhận được từ command queue của chính mình. Nếu
 * request có mang pData (vd SDCARD_CMD_GET_SINGLE_FRAME), owner phải ghi dữ liệu trả về thẳng vào
 * đó (qua con trỏ nhận được trong Srm_Message_s.pData) TRƯỚC KHI gọi hàm này.
 * @param pRequest: request đã nhận (dùng pRequest->responseQueue để gửi trả lời)
 * @param payload: dữ liệu vô hướng trả về (kết quả thành công/thất bại, giá trị đọc được...)
 * @return E_OK nếu gửi trả lời thành công, E_NOT_OK nếu pRequest không có responseQueue hoặc
 *         gửi thất bại (hàng đợi đầy)
 */
Std_ReturnType Srm_Reply(const Srm_Message_s *pRequest, uint32_t payload);

/* --- API riêng cho từng lệnh, 1 lệnh 1 hàm - xem Srm_CommandType_e để biết đủ danh sách --- */

/**
 * @brief Srm_PcmGetPlayedSamples
 * Hỏi Pcm_Task số sample-frame ĐÃ THỰC SỰ PHÁT RA DAC của bài đang phát (owner: Pcm_Task, xem
 * task/pcm_player.h/pcm_player.c), lấy từ Max98357a_GetPlayedSamples() (driver/max98357a) -
 * đếm bằng callback on_sent xác nhận DMA thật, KHÔNG phải nội suy theo thời gian như
 * Srm_Mp3GetDecodeTime()/VS1053 trước đây. Pcm_Task ghi thẳng giá trị đọc được vào pData
 * (giống cơ chế pOutFrame của Srm_SdcardGetSingleFrame) rồi mới trả lời, payload lúc trả lời
 * chỉ còn mang E_OK/E_NOT_OK. Không nhận ownerQueue làm tham số - lệnh này luôn gửi tới đúng 1
 * owner cố định (Pcm_Task/xPcmCommandQueue), tự biết bên trong (xem srm.c).
 * @param pPlayedSamples: [out] số sample-frame đã phát, chỉ hợp lệ khi hàm trả về E_OK
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return E_OK nếu nhận được phản hồi E_OK trong thời gian chờ, E_NOT_OK nếu xPcmCommandQueue
 *         chưa tồn tại, SRM hết chỗ đăng ký task mới, command queue đầy, hết thời gian chờ,
 *         hoặc Pcm_Task trả lời E_NOT_OK
 */
Std_ReturnType Srm_PcmGetPlayedSamples(uint32_t *pPlayedSamples, TickType_t timeoutTicks);

/**
 * @brief Srm_SdcardGetSingleFrame
 * Xin Sdcard_Task 1 frame animation theo chỉ số (owner: Sdcard_Task, xem task/sdcard.c,
 * driver/buffer/double_buffer.c). Sdcard_Task tự nạp trước/nạp gấp nếu thiếu dữ liệu trong
 * buffer, ghi thẳng dữ liệu frame vào pOutFrame rồi mới trả lời - nhận được E_OK nghĩa là
 * pOutFrame đã có dữ liệu sẵn sàng để vẽ ngay, không cần xử lý gì thêm. Không nhận ownerQueue
 * làm tham số - lệnh này luôn gửi tới đúng 1 owner cố định (Sdcard_Task/xSdCommandQueue), tự
 * biết bên trong (xem srm.c).
 * @param frameIndex: chỉ số frame cần lấy
 * @param pOutFrame: buffer đích nhận dữ liệu, kích thước tối thiểu FRAME_SIZE byte (xem
 *        driver/buffer/double_buffer.h), do bên gọi sở hữu và còn sống tới khi hàm return
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return E_OK nếu lấy thành công (pOutFrame đã có dữ liệu mới, payload nội bộ là
 *         E_OK), E_NOT_OK nếu xSdCommandQueue chưa tồn tại, SRM hết chỗ đăng ký task mới,
 *         command queue đầy, hết thời gian chờ, hoặc Sdcard_Task trả lời E_NOT_OK
 *         (không nạp được frame, vd lỗi đọc thẻ SD)
 */
Std_ReturnType Srm_SdcardGetSingleFrame(uint32_t frameIndex, uint8_t *pOutFrame, TickType_t timeoutTicks);

/**
 * @brief Srm_OledNotifyBootStatus
 * Báo Oled_Task trạng thái mount/quét thẻ SD ngay lúc boot (owner: Oled_Task, xem
 * task/oled.c) - giá trị statusCode là OLED_BOOT_STATUS_OK/SD_ERROR/NO_SONGS (xem oled.h).
 * Chỉ Sdcard_Task gọi đúng 1 lần lúc khởi động, ngay sau khi Sdcard_Mount()/
 * Sdcard_ScanAndCreateDb() xong. KHÔNG chờ phản hồi (fire-and-forget, không qua
 * Srm_SendCommand) - Oled_Task tự dừng lại chờ đúng 1 lần ở đầu Oled_Task để nhận thông báo
 * này trước khi vẽ menu lần đầu, xem Oled_Task. Tự đợi (poll, tối đa vài trăm ms) nếu
 * xOledCommandQueue chưa được tạo - Sdcard_Task (Core 1) và Oled_Task (Core 0) khởi động
 * song song, không đảm bảo Oled_Init() đã chạy xong trước khi Sdcard_Task quét xong thẻ SD.
 * @param statusCode: OLED_BOOT_STATUS_OK/SD_ERROR/NO_SONGS (xem oled.h)
 * @return
 */
void Srm_OledNotifyBootStatus(uint32_t statusCode);

#endif /* SRM_H */
