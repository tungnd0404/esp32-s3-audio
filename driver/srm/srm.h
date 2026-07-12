#ifndef SRM_H
#define SRM_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* SRM (Shared Resource Manager): bộ API dùng chung cho kiến trúc Owner Task - 1 task sở
   hữu 1 tài nguyên dùng chung (vd Mp3_Task sở hữu VS1053), các task khác không được đụng
   thẳng vào tài nguyên đó mà phải gửi lệnh qua command queue của owner, dùng API ở đây.
   SRM cũng là nơi quản lý tập trung mã lệnh (cmdId) của TẤT CẢ tài nguyên dùng chung trong
   hệ thống - mỗi tài nguyên mới thêm vào sau này (không riêng gì Mp3) đều khai báo enum
   lệnh riêng của nó ngay tại đây, không khai báo rải rác trong header của từng task owner. */

/* --- Lệnh cho tài nguyên VS1053 (owner: Mp3_Task, xem mp3.h/mp3.c) --- */
/* Mã lệnh (Srm_Message_s.cmdId) gửi tới Mp3_Task qua xMp3CommandQueue. Mp3_Task là owner
   duy nhất của thiết bị VS1053 - mọi module khác (sync_frame, oled...) không được gọi
   thẳng vs1053_read_sci/vs1053_write_sci/vs1053_send_buffer, mà phải gửi lệnh qua đây. */
typedef enum {
    MP3_CMD_GET_DECODE_TIME,
    /* Mở rộng sau: MP3_CMD_GET_VOLUME, MP3_CMD_SET_VOLUME, MP3_CMD_PLAY, MP3_CMD_PAUSE, ... */

    /* --- Lệnh cho tài nguyên double buffer animation (owner: Sdcard_Task, xem
       driver/buffer/double_buffer.c/h) --- */
    /* Yêu cầu nạp trước buffer còn lại - gửi qua Srm_SendCommand nhưng owner (Sdcard_Task)
       trả lời NGAY khi nhận được (ack "đã nhận", payload trả về không có ý nghĩa), TRƯỚC
       KHI thực sự nạp - nhờ vậy bên gửi không phải chờ tới lúc nạp xong thật mới được trả
       lời, gần như không chặn dù cùng đi qua kênh blocking như SDCARD_CMD_LOAD_MISSING_FRAME
       (xem Sdcard_HandleCommand trong sdcard.c) */
    SDCARD_CMD_PRELOAD_BUFFER,
    /* Yêu cầu nạp gấp đúng frame đang thiếu (payload = chỉ số frame cần), gửi qua
       Srm_SendCommand - owner chỉ trả lời SAU KHI đã nạp xong thật (payload trả về: 1 =
       thành công, 0 = thất bại) vì bên gửi cần biết kết quả trước khi đọc tiếp frame đó */
    SDCARD_CMD_LOAD_MISSING_FRAME,

    /* Luôn đặt cuối cùng - không phải lệnh thật, dùng để Srm_SendCommand validate cmdId
       truyền vào có hợp lệ hay không (cmdId >= SRM_CMD_INVALID -> từ chối, không gửi) */
    SRM_CMD_INVALID
} Srm_CommandType_e;

/* Phân loại nội dung message */
typedef enum {
    SRM_MSG_KIND_COMMAND,   /* Yêu cầu owner task thực hiện hành động / trả dữ liệu */
    SRM_MSG_KIND_DATA       /* Dữ liệu thuần, dùng cho response */
} Srm_MsgKindType_e;

/* Message chung cho MỌI command queue theo kiến trúc Owner Task, không riêng gì 1 module.
   payload là uint32_t thô - task nhận tự ép kiểu/diễn giải theo đúng cmdId của mình
   (vd (uint16_t)payload nếu cmdId trả về giá trị 16-bit) */
typedef struct {
    /* COMMAND hay DATA */
    Srm_MsgKindType_e kind;
    /* Mã lệnh, định nghĩa riêng theo từng owner task (vd Srm_CommandType_e) */
    uint32_t cmdId;
    /* Dữ liệu thô đi kèm: tham số của lệnh (khi kind == COMMAND) hoặc kết quả trả về
       (khi kind == DATA) */
    uint32_t payload;
    /* Queue trả lời, chỉ dùng khi kind == SRM_MSG_KIND_COMMAND và cần response.
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
 * mutex không cần đồng bộ hoá gì thêm. Nếu tạo mutex kiểu lazy (tạo khi cần bên trong
 * Srm_SendCommand) thì 2 task gọi lần đầu cùng lúc có thể cùng thấy mutex chưa tồn tại và
 * cùng tạo, ra 2 mutex khác nhau - không bảo vệ được gì cả.
 * @param
 * @return
 */
void Srm_Init(void);

/**
 * @brief Srm_SendCommand
 * Gửi 1 lệnh vào command queue của owner task, chờ phản hồi.
 *
 * SRM tự động quản lý response queue theo TỪNG TASK GỌI (không phải theo module gọi):
 * ngay lần đầu tiên 1 task gọi Srm_SendCommand, SRM tạo 1 queue riêng dành cho đúng task
 * đó (khoá theo TaskHandle_t lấy từ xTaskGetCurrentTaskHandle()) và ghi nhớ lại (registry
 * nội bộ, xem Srm_GetOwnResponseQueue() trong srm.c) để dùng lại cho mọi lần gọi sau, kể
 * cả khi được gọi từ nhiều module khác nhau nhưng cùng chạy trong 1 task. Vì mỗi task có
 * queue response RIÊNG, 2 task khác nhau cùng gửi lệnh gần nhau không bao giờ nhận nhầm
 * response của nhau - kể cả khi cùng gửi 1 loại cmdId.
 *
 * Lưu ý: 1 task chỉ nên đóng 1 vai tại 1 thời điểm khi dùng SRM - hoặc là owner (nhận
 * command hướng vào mình qua ownerQueue riêng của nó, như Mp3_Task), hoặc là bên gửi (gọi
 * hàm này chờ response). Nếu 1 task vừa là owner vừa tự gọi Srm_SendCommand đồng thời,
 * command người khác gửi tới và response nó đang chờ sẽ lẫn vào chung 1 queue của chính
 * nó - trường hợp này chưa được hỗ trợ.
 *
 * @param ownerQueue: command queue của owner task cần gửi tới (vd xMp3CommandQueue)
 * @param cmdId: mã lệnh (định nghĩa theo owner task, vd Srm_CommandType_e)
 * @param pPayload: [in/out] vào: dữ liệu kèm theo lệnh (vd giá trị volume muốn set), 0 nếu
 *        không cần; ra: payload nhận được từ owner task (chỉ khi hàm trả về true)
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return true nếu gửi và nhận được phản hồi thành công, false nếu ownerQueue chưa tồn
 *         tại, cmdId không hợp lệ (>= SRM_CMD_INVALID), SRM hết chỗ đăng ký task mới,
 *         command queue đầy, hoặc hết thời gian chờ
 */
bool Srm_SendCommand(QueueHandle_t ownerQueue, uint32_t cmdId, uint32_t *pPayload,
                      TickType_t timeoutTicks);

/**
 * @brief Srm_Reply
 * Owner task dùng để trả lời 1 request đã nhận được từ command queue của chính mình.
 * @param pRequest: request đã nhận (dùng pRequest->responseQueue để gửi trả lời)
 * @param payload: dữ liệu trả về
 * @return true nếu gửi trả lời thành công, false nếu pRequest không có responseQueue hoặc
 *         gửi thất bại (hàng đợi đầy)
 */
bool Srm_Reply(const Srm_Message_s *pRequest, uint32_t payload);

#endif /* SRM_H */
