/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "srm.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* Số task tối đa SRM có thể đăng ký response queue riêng cùng lúc. Tăng lên nếu sau này
   có nhiều task hơn cùng gọi 1 hàm Srm_<Owner><Lệnh> (mỗi task chỉ chiếm 1 chỗ, không phụ
   thuộc số lần gọi). */
#define SRM_MAX_REGISTERED_TASKS   8U

/* ===================================================
 *  LOCAL TYPE DEFINITIONS
 * =================================================== */

/* 1 dòng trong registry: ánh xạ 1 task -> response queue riêng của task đó */
typedef struct {
    /* Task đang giữ chỗ này, NULL nghĩa là chỗ còn trống */
    TaskHandle_t taskHandle;
    /* Response queue riêng của taskHandle, tạo đúng 1 lần và dùng lại mãi mãi */
    QueueHandle_t responseQueue;
} Srm_TaskEntry_s;

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Registry giữ response queue riêng của từng task đã từng gọi 1 hàm Srm_<Owner><Lệnh>.
   Mảng tĩnh, không cấp phát động - đủ dùng vì số lượng task trong hệ thống cố định và nhỏ. */
static Srm_TaskEntry_s gaTaskResponseQueueList[SRM_MAX_REGISTERED_TASKS];

/* Số chỗ trong gaTaskResponseQueueList đã được dùng (đăng ký từ đầu mảng, không có lỗ trống ở giữa) */
static uint32_t gu32RegisteredCount = 0;

/* Mutex bảo vệ gaTaskResponseQueueList/gu32RegisteredCount khỏi bị 2 task cùng đọc-sửa-ghi 1 lúc
   (vd 2 task cùng lần đầu gọi 1 hàm Srm_<Owner><Lệnh> cùng lúc -> cùng không thấy mình trong
   registry -> cùng ghi đè lên cùng 1 chỗ trống). Tạo 1 lần trong Srm_Init(), KHÔNG tạo
   lazy trong Srm_GetOwnResponseQueue() vì việc tạo mutex kiểu lazy cũng bị race y hệt. */
static SemaphoreHandle_t gxMutexTaskResponseQueueList = NULL;

/* Tag dùng cho ESP_LOGx trong module này */
static const char *TAG = "SRM";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief Srm_GetOwnResponseQueue
 * Trả về response queue riêng của TASK ĐANG GỌI hàm này (xác định qua
 * xTaskGetCurrentTaskHandle()). Nếu đây là lần đầu task này gọi, tự tạo 1 queue mới và
 * đăng ký vào gaTaskResponseQueueList để dùng lại cho các lần gọi sau, kể cả khi được gọi lại từ
 * module khác (miễn cùng chạy trong task đó). Toàn bộ thao tác tìm + đăng ký được khoá
 * bằng gxMutexTaskResponseQueueList để an toàn khi nhiều task gọi đồng thời.
 * @param
 * @return response queue riêng của task đang gọi, hoặc NULL nếu Srm_Init() chưa được gọi,
 *         registry đã đầy (SRM_MAX_REGISTERED_TASKS), hoặc tạo queue thất bại (hết heap)
 */
static QueueHandle_t Srm_GetOwnResponseQueue(void)
{
    /* Danh tính của task đang gọi - dùng làm khoá tra cứu/đăng ký trong registry */
    TaskHandle_t lCallerTask = xTaskGetCurrentTaskHandle();
    QueueHandle_t lResult = NULL;

    /* Chưa gọi Srm_Init() -> không có mutex để bảo vệ registry, từ chối luôn cho an toàn */
    if (gxMutexTaskResponseQueueList == NULL)
    {
        return NULL;
    }

    /* Khoá registry trong lúc tìm + (có thể) đăng ký, tránh 2 task cùng sửa 1 lúc */
    if (xSemaphoreTake(gxMutexTaskResponseQueueList, portMAX_DELAY) != pdTRUE)
    {
        return NULL;
    }

    /* Tìm xem task này đã đăng ký trước đó chưa, có thì dùng lại luôn */
    for (uint32_t i = 0; i < gu32RegisteredCount; i++)
    {
        if (gaTaskResponseQueueList[i].taskHandle == lCallerTask)
        {
            lResult = gaTaskResponseQueueList[i].responseQueue;
            break;
        }
    }

    /* Chưa từng đăng ký -> thử tạo mới, nếu còn chỗ trống */
    if ((lResult == NULL) && (gu32RegisteredCount < SRM_MAX_REGISTERED_TASKS))
    {
        /* Lần đầu task này gọi SRM -> tạo response queue riêng cho nó (độ sâu 1, đủ dùng
           vì bên gửi luôn gửi xong 1 request rồi mới chờ response, không gửi dồn) */
        QueueHandle_t lNewQueue = xQueueCreate(1, sizeof(Srm_Message_s));
        if (lNewQueue != NULL)
        {
            /* Ghi nhận vào registry để lần gọi sau (của chính task này) tìm thấy ngay */
            gaTaskResponseQueueList[gu32RegisteredCount].taskHandle = lCallerTask;
            gaTaskResponseQueueList[gu32RegisteredCount].responseQueue = lNewQueue;
            gu32RegisteredCount++;
            lResult = lNewQueue;
        }
        else
        {
            /* Hết heap lúc tạo response queue mới - hiếm nhưng cần log rõ lý do thất bại,
               tránh hàm gửi lệnh trả về false không rõ nguyên nhân (registry đầy vs timeout thật) */
            ESP_LOGE(TAG, "Failed to create response queue for task 0x%p (out of heap)", (void *)lCallerTask);
        }
    }
    else if (lResult == NULL)
    {
        /* Task chưa từng đăng ký nhưng registry đã đầy (SRM_MAX_REGISTERED_TASKS) -> không
           còn chỗ tạo mới. Log rõ để phân biệt với trường hợp timeout thật khi debug */
        ESP_LOGE(TAG, "Registry full (SRM_MAX_REGISTERED_TASKS=%u), cannot register task 0x%p",
                 (unsigned)SRM_MAX_REGISTERED_TASKS, (void *)lCallerTask);
    }

    xSemaphoreGive(gxMutexTaskResponseQueueList);

    return lResult;
}

/**
 * @brief Srm_SendCommand
 * Gửi 1 lệnh vào command queue của owner task, chờ phản hồi - cơ chế gửi/nhận generic dùng
 * chung cho MỌI lệnh, ẩn phía sau các hàm Srm_<Owner><Lệnh> công khai (xem srm.h) nên không
 * lộ ra ngoài module này - callers phải dùng đúng hàm dành riêng cho lệnh mình cần, không
 * tự dựng Srm_Message_s hay gọi hàm generic trực tiếp.
 *
 * SRM tự động quản lý response queue theo TỪNG TASK GỌI (không phải theo module gọi):
 * ngay lần đầu tiên 1 task gọi tới đây (qua 1 hàm Srm_<Owner><Lệnh> bất kỳ), SRM tạo 1 queue
 * riêng dành cho đúng task đó (khoá theo TaskHandle_t lấy từ xTaskGetCurrentTaskHandle()) và
 * ghi nhớ lại (registry nội bộ, xem Srm_GetOwnResponseQueue()) để dùng lại cho mọi lần gọi
 * sau, kể cả khi được gọi từ nhiều module khác nhau nhưng cùng chạy trong 1 task. Vì mỗi
 * task có queue response RIÊNG, 2 task khác nhau cùng gửi lệnh gần nhau không bao giờ nhận
 * nhầm response của nhau - kể cả khi cùng gửi 1 loại cmdId.
 *
 * Lưu ý: 1 task chỉ nên đóng 1 vai tại 1 thời điểm khi dùng SRM - hoặc là owner (nhận
 * command hướng vào mình qua ownerQueue riêng của nó, như Mp3_Task), hoặc là bên gửi. Nếu
 * 1 task vừa là owner vừa tự gọi hàm gửi lệnh đồng thời, command người khác gửi tới và
 * response nó đang chờ sẽ lẫn vào chung 1 queue của chính nó - trường hợp này chưa được hỗ trợ.
 *
 * @param ownerQueue: command queue của owner task cần gửi tới (vd xMp3CommandQueue)
 * @param cmdId: mã lệnh (Srm_CommandType_e)
 * @param pPayload: [in/out] vào: dữ liệu vô hướng kèm theo lệnh, 0 nếu không cần; ra:
 *        payload nhận được từ owner task (chỉ khi hàm trả về true)
 * @param pData: con trỏ dữ liệu lớn hơn kèm theo lệnh (owner ghi thẳng dữ liệu trả về vào
 *        đây nếu lệnh cần, xem Srm_Message_s), NULL nếu lệnh không cần
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return true nếu gửi và nhận được phản hồi thành công, false nếu ownerQueue chưa tồn
 *         tại, cmdId không hợp lệ (>= SRM_CMD_INVALID), SRM hết chỗ đăng ký task mới,
 *         command queue đầy, hoặc hết thời gian chờ
 */
static bool Srm_SendCommand(QueueHandle_t ownerQueue, uint32_t cmdId, uint32_t *pPayload,
                             void *pData, TickType_t timeoutTicks)
{
    Srm_Message_s lRequest;
    Srm_Message_s lResponse;
    QueueHandle_t lResponseQueue;

    /* Owner task chưa tạo command queue (vd chưa được triển khai/khởi tạo) -> không có ai xử lý */
    if (ownerQueue == NULL)
    {
        return false;
    }

    /* cmdId nằm ngoài phạm vi lệnh hợp lệ (>= SRM_CMD_INVALID) -> từ chối, không gửi */
    if (cmdId >= SRM_CMD_INVALID)
    {
        return false;
    }

    /* Lấy (hoặc tạo lần đầu) response queue riêng của task đang gọi hàm này */
    lResponseQueue = Srm_GetOwnResponseQueue();
    if (lResponseQueue == NULL)
    {
        return false;
    }

    lRequest.kind = SRM_MSG_COMMAND;
    lRequest.cmdId = cmdId;
    lRequest.payload = *pPayload;
    lRequest.pData = pData;
    lRequest.responseQueue = lResponseQueue;

    /* Gửi request, không chờ nếu command queue của owner đang đầy */
    if (xQueueSend(ownerQueue, &lRequest, 0) != pdTRUE)
    {
        return false;
    }

    /* Chờ owner task trả lời tối đa timeoutTicks, trên đúng queue riêng của task này */
    if (xQueueReceive(lResponseQueue, &lResponse, timeoutTicks) != pdTRUE)
    {
        return false;
    }

    /* Sau khi gửi queue và nhận response, pPayload là output */
    *pPayload = lResponse.payload;

    return true;
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief Srm_Init
 * Khởi tạo module SRM: tạo mutex bảo vệ registry (ánh xạ task -> response queue riêng).
 * PHẢI gọi đúng 1 lần trong app_main(), TRƯỚC khi bất kỳ task nào được tạo - lúc đó chỉ
 * có 1 luồng thực thi duy nhất nên tạo mutex không cần đồng bộ hoá gì thêm.
 * @param
 * @return
 */
void Srm_Init(void)
{
    gxMutexTaskResponseQueueList = xSemaphoreCreateMutex();
}

/**
 * @brief Srm_Reply
 * Owner task dùng để trả lời 1 request đã nhận được từ command queue của chính mình.
 * @param pRequest: request đã nhận (dùng pRequest->responseQueue để gửi trả lời)
 * @param payload: dữ liệu trả về
 * @return true nếu gửi trả lời thành công, false nếu pRequest không có responseQueue hoặc
 *         gửi thất bại (hàng đợi đầy)
 */
bool Srm_Reply(const Srm_Message_s *pRequest, uint32_t payload)
{
    Srm_Message_s lResponse;

    if ((pRequest == NULL) || (pRequest->responseQueue == NULL))
    {
        return false;
    }

    lResponse.kind = SRM_MSG_DATA;
    lResponse.cmdId = pRequest->cmdId;
    lResponse.payload = payload;
    lResponse.pData = NULL;
    lResponse.responseQueue = NULL;

    return (xQueueSend(pRequest->responseQueue, &lResponse, 0) == pdTRUE);
}

/**
 * @brief Srm_Mp3GetDecodeTime
 * Hỏi Mp3_Task thời gian đã giải mã (giây) của bài đang phát (owner: Mp3_Task).
 * @param ownerQueue: command queue của Mp3_Task (xMp3CommandQueue)
 * @param pDecodeTimeSec: [out] thời gian đã giải mã (giây), chỉ hợp lệ khi hàm trả về true
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return true nếu nhận được phản hồi trong thời gian chờ, false nếu thất bại/timeout
 */
bool Srm_Mp3GetDecodeTime(QueueHandle_t ownerQueue, uint16_t *pDecodeTimeSec, TickType_t timeoutTicks)
{
    if (pDecodeTimeSec == NULL)
    {
        return false;
    }

    /* MP3_CMD_GET_DECODE_TIME không cần tham số vào -> payload để 0, không cần pData vì kết
       quả chỉ 4 byte, đủ chứa trong payload */
    uint32_t lu32Payload = 0U;
    if (Srm_SendCommand(ownerQueue, MP3_CMD_GET_DECODE_TIME, &lu32Payload, NULL, timeoutTicks) == false)
    {
        return false;
    }

    /* vs1053_get_decoded_time() trả uint16_t, Mp3_Task ép kiểu mở rộng thành uint32_t để
       trả qua payload -> ép kiểu lại về uint16_t ở đây */
    *pDecodeTimeSec = (uint16_t)lu32Payload;
    return true;
}

/**
 * @brief Srm_SdcardGetSingleFrame
 * Xin Sdcard_Task 1 frame animation theo chỉ số (owner: Sdcard_Task).
 * @param ownerQueue: command queue của Sdcard_Task (xSdCommandQueue)
 * @param frameIndex: chỉ số frame cần lấy
 * @param pOutFrame: buffer đích nhận dữ liệu, kích thước tối thiểu FRAME_SIZE byte
 * @param timeoutTicks: thời gian tối đa chờ phản hồi
 * @return true nếu lấy thành công (pOutFrame đã có dữ liệu mới), false nếu thất bại/timeout/
 *         Sdcard_Task không nạp được frame
 */
bool Srm_SdcardGetSingleFrame(QueueHandle_t ownerQueue, uint32_t frameIndex, uint8_t *pOutFrame, TickType_t timeoutTicks)
{
    if (pOutFrame == NULL)
    {
        return false;
    }

    /* payload mang chỉ số frame lúc gửi đi, pData mang buffer đích để owner ghi thẳng dữ
       liệu 1024 byte vào (không thể truyền qua payload - chỉ 4 byte) */
    uint32_t lu32Payload = frameIndex;
    if (Srm_SendCommand(ownerQueue, SDCARD_CMD_GET_SINGLE_FRAME, &lu32Payload, pOutFrame, timeoutTicks) == false)
    {
        return false;
    }

    /* Sau khi gửi/nhận, lu32Payload là kết quả owner trả về (1 = thành công, 0 = thất bại),
       không còn là frameIndex đã gửi đi nữa (Srm_SendCommand ghi đè qua tham số [in/out]) */
    return (lu32Payload != 0U);
}
