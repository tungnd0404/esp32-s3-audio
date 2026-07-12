/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "mp3.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số request tối đa có thể chờ xử lý trong xMp3CommandQueue cùng lúc */
#define MP3_COMMAND_QUEUE_LENGTH   4U

/* ===================================================
 *  GLOBAL VARIABLES
 * =================================================== */

/* task handler Mp3_Task */
TaskHandle_t xMp3TaskHandle = NULL;

/* Hàng đợi lệnh dùng chung tới Mp3_Task, tạo trong Mp3_Init() vì Mp3_Task là owner */
QueueHandle_t xMp3CommandQueue = NULL;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Mp3_Init
 * Khởi tạo module Mp3: tạo xMp3CommandQueue để các module khác gửi lệnh vào.
 * Gọi trước khi tạo Mp3_Task.
 * @param
 * @return
 */
void Mp3_Init(void)
{
    xMp3CommandQueue = xQueueCreate(MP3_COMMAND_QUEUE_LENGTH, sizeof(Srm_Message_s));
}

/**
 * @brief Mp3_Task
 * Task owner duy nhất của thiết bị VS1053. Nhận Srm_Message_s từ xMp3CommandQueue, dựa
 * vào cmdId (Srm_CommandType_e) để gọi API vs1053_* tương ứng, trả kết quả về bằng
 * Srm_Reply().
 * Thân hàm hiện để trống, sẽ triển khai sau (stream mp3 + xử lý command).
 * @param pvParameters
 * @return
 */
void Mp3_Task(void *pvParameters)
{
    while (1)
    {
        /* TODO: stream dữ liệu mp3 cho VS1053 */

        /* TODO: xQueueReceive(xMp3CommandQueue, &lRequest, 0) không chờ, switch theo
           lRequest.cmdId (Srm_CommandType_e) để xử lý (vd đọc SCI_DECODE_TIME), rồi
           Srm_Reply(&lRequest, <kết quả>) trả về cho bên gửi */

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
