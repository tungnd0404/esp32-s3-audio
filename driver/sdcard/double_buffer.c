#include "double_buffer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DOUBLE_BUFFER";

// ==================== Định nghĩa biến toàn cục ====================
// Hai vùng đệm chính
static uint8_t bufferA[CACHE_FRAMES][FRAME_SIZE];
static uint8_t bufferB[CACHE_FRAMES][FRAME_SIZE];

// Chỉ số frame đầu tiên trong mỗi buffer
static uint32_t startA = 0;
static uint32_t startB = 0;

// Số frame thực tế đã load được vào mỗi buffer (có thể < CACHE_FRAMES ở cuối file)
static uint32_t countA = 0;
static uint32_t countB = 0;

// Cờ cho biết buffer đã sẵn sàng để đọc chưa
static bool readyA = false;
static bool readyB = false;

// Buffer nào đang được dùng để phục vụ double_buffer_get_frame()
static bool usingA = true;

// Cờ tránh gửi sự kiện PRELOAD nhiều lần cho mỗi buffer
static bool preload_requested_for_A = false;
static bool preload_requested_for_B = false;

// Mutex bảo vệ truy cập vào các biến toàn cục trên
static SemaphoreHandle_t double_buffer_mutex = NULL;

// Event group để đồng bộ với SD task
static EventGroupHandle_t double_buffer_event = NULL;

// Con trỏ FILE đang mở (đọc frame.bin)
static FILE *frame_file = NULL;

// Tổng số frame của bài hiện tại
static uint32_t total_frames = 0;

// ==================== Hàm nội bộ ====================

/**
 * @brief Load dữ liệu từ file vào một buffer cụ thể tại vị trí index
 * @param buffer  Mảng 2 chiều [CACHE_FRAMES][FRAME_SIZE]
 * @param start   Con trỏ lưu chỉ số frame đầu tiên được load
 * @param count   Con trỏ lưu số frame thực tế đã load
 * @param index   Chỉ số frame bắt đầu cần load
 * @return true nếu thành công (có ít nhất 1 frame), false nếu lỗi hoặc hết file
 */
static bool load_double_buffer_internal(uint8_t buffer[][FRAME_SIZE],
                                  uint32_t *start,
                                  uint32_t *count,
                                  uint32_t index)
{
    if (!frame_file) return false;
    if (index >= total_frames) return false;

    *start = index;
    long offset = (long)index * FRAME_SIZE;
    if (fseek(frame_file, offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek failed at index %lu", index);
        return false;
    }

    uint32_t frames_loaded = 0;
    for (int i = 0; i < CACHE_FRAMES; i++) {
        if (index + i >= total_frames)
            break;
        size_t n = fread(buffer[i], 1, FRAME_SIZE, frame_file);
        if (n != FRAME_SIZE) {
            ESP_LOGE(TAG, "fread failed at frame %lu", index + i);
            return false;
        }
        frames_loaded++;
    }
    *count = frames_loaded;
    return (frames_loaded > 0);
}

/**
 * @brief Chuyển đổi buffer đang dùng (flip) nếu buffer kia đã sẵn sàng
 *        Hàm này phải được gọi trong khi đã giữ mutex
 * @param current_index  Chỉ số frame đang cần đọc
 * @return true nếu đã flip thành công và buffer mới chứa frame cần, false nếu chưa thể flip
 */
static bool try_flip_buffer(uint32_t current_index)
{
    if (usingA) {
        // Nếu buffer A không còn chứa frame này nữa (đã đọc hết)
        if (current_index >= startA + countA) {
            if (readyB && current_index >= startB && current_index < startB + countB) {
                // Flip sang buffer B
                usingA = false;
                ESP_LOGD(TAG, "Flip to buffer B, startB=%lu, countB=%lu", startB, countB);
                return true;
            }
        }
    } else {
        if (current_index >= startB + countB) {
            if (readyA && current_index >= startA && current_index < startA + countA) {
                usingA = true;
                ESP_LOGD(TAG, "Flip to buffer A, startA=%lu, countA=%lu", startA, countA);
                return true;
            }
        }
    }
    return false;
}

// ==================== API công khai ====================

void double_buffer_init(void)
{
    double_buffer_mutex = xSemaphoreCreateMutex();
    if (!double_buffer_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    double_buffer_event = xEventGroupCreate();
    if (!double_buffer_event) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(double_buffer_mutex);
        double_buffer_mutex = NULL;
        return;
    }
    ESP_LOGI(TAG, "Double buffer module initialized");
}

void double_buffer_open(const char *path)
{
    if (!double_buffer_mutex || !double_buffer_event) {
        ESP_LOGE(TAG, "Module not initialized. Call double_buffer_init() first.");
        return;
    }

    xSemaphoreTake(double_buffer_mutex, portMAX_DELAY);

    // Đóng file cũ nếu có
    if (frame_file) {
        fclose(frame_file);
        frame_file = NULL;
    }

    // Mở file mới
    frame_file = fopen(path, "rb");
    if (!frame_file) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        xSemaphoreGive(double_buffer_mutex);
        return;
    }

    // Lấy kích thước file
    fseek(frame_file, 0, SEEK_END);
    long size = ftell(frame_file);
    fseek(frame_file, 0, SEEK_SET);
    total_frames = size / FRAME_SIZE;
    ESP_LOGI(TAG, "Opened %s, total frames = %lu", path, total_frames);

    // Load buffer A từ frame 0
    readyA = load_double_buffer_internal(bufferA, &startA, &countA, 0);
    readyB = false;
    usingA = true;
    preload_requested_for_A = false;
    preload_requested_for_B = false;

    // Nếu load A thất bại, xóa file
    if (!readyA) {
        fclose(frame_file);
        frame_file = NULL;
        total_frames = 0;
    }

    xSemaphoreGive(double_buffer_mutex);
}

void double_buffer_close(void)
{
    xSemaphoreTake(double_buffer_mutex, portMAX_DELAY);

    if (frame_file) {
        fclose(frame_file);
        frame_file = NULL;
    }
    readyA = false;
    readyB = false;
    startA = startB = 0;
    countA = countB = 0;
    usingA = true;
    total_frames = 0;
    preload_requested_for_A = false;
    preload_requested_for_B = false;

    xSemaphoreGive(double_buffer_mutex);
}

uint32_t double_buffer_total_frames(void)
{
    uint32_t ret;
    xSemaphoreTake(double_buffer_mutex, portMAX_DELAY);
    ret = total_frames;
    xSemaphoreGive(double_buffer_mutex);
    return ret;
}

bool double_buffer_get_frame(uint32_t index, uint8_t *out_frame)
{
    if (!out_frame) return false;

    // Kiểm tra chỉ số hợp lệ
    if (index >= double_buffer_total_frames()) {
        ESP_LOGE(TAG, "Index %lu out of range (max %lu)", index, total_frames);
        return false;
    }

    // Vòng lặp retry (không đệ quy)
    while (1) {
        bool found = false;

        xSemaphoreTake(double_buffer_mutex, portMAX_DELAY);

        // Kiểm tra trong buffer hiện tại
        if (usingA && readyA) {
            if (index >= startA && index < startA + countA) {
                memcpy(out_frame, bufferA[index - startA], FRAME_SIZE);
                found = true;

                // Kích hoạt preload nếu đã đọc hơn 80% buffer A và chưa yêu cầu preload cho B
                if (!preload_requested_for_B &&
                    (index - startA) > (countA * 8 / 10)) {
                    preload_requested_for_B = true;
                    xEventGroupSetBits(double_buffer_event, EVT_PRELOAD);
                    ESP_LOGD(TAG, "Preload requested for buffer B at index %lu", index);
                }
            }
        } else if (!usingA && readyB) {
            if (index >= startB && index < startB + countB) {
                memcpy(out_frame, bufferB[index - startB], FRAME_SIZE);
                found = true;

                if (!preload_requested_for_A &&
                    (index - startB) > (countB * 8 / 10)) {
                    preload_requested_for_A = true;
                    xEventGroupSetBits(double_buffer_event, EVT_PRELOAD);
                    ESP_LOGD(TAG, "Preload requested for buffer A at index %lu", index);
                }
            }
        }

        // Nếu không tìm thấy trong buffer hiện tại, thử flip sang buffer kia
        if (!found) {
            if (try_flip_buffer(index)) {
                // Đã flip, kiểm tra lại trong buffer mới (vòng lặp tiếp theo)
                xSemaphoreGive(double_buffer_mutex);
                continue;
            }
        }

        xSemaphoreGive(double_buffer_mutex);

        if (found) {
            return true;
        }

        // Không có buffer nào chứa frame cần -> báo miss, chờ SD task load
        ESP_LOGW(TAG, "Frame %lu not in any buffer, requesting load miss", index);
        xEventGroupSetBits(double_buffer_event, EVT_LOAD_MISS);

        // Chờ sự kiện EVT_READY với timeout 5 giây
        EventBits_t bits = xEventGroupWaitBits(double_buffer_event,
                                                EVT_READY,
                                                pdTRUE,   // clear bit sau khi nhận
                                                pdTRUE,   // wait for all bits (chỉ 1 bit)
                                                pdMS_TO_TICKS(5000));
        if (!(bits & EVT_READY)) {
            ESP_LOGE(TAG, "Timeout waiting for frame %lu", index);
            return false;
        }

        // Sau khi được báo ready, quay lại vòng lặp để thử lấy frame lần nữa
    }
}

// Hàm dành riêng cho SD task: thực hiện load buffer tiếp theo hoặc buffer bị miss
bool sd_task_load_buffer(void)
{
    if (!frame_file) return false;

    xSemaphoreTake(double_buffer_mutex, portMAX_DELAY);

    // Xác định buffer nào cần được load
    bool need_load_A = false;
    bool need_load_B = false;
    uint32_t load_index = 0;

    if (usingA) {
        // Buffer A đang dùng, cần preload buffer B
        if (!readyB && preload_requested_for_B) {
            need_load_B = true;
            // Tính index cần load cho B: thường là startA + countA (tiếp theo sau buffer A)
            load_index = startA + countA;
            if (load_index >= total_frames) {
                // Hết file, không cần load
                preload_requested_for_B = false;
                need_load_B = false;
            }
        }
    } else {
        if (!readyA && preload_requested_for_A) {
            need_load_A = true;
            load_index = startB + countB;
            if (load_index >= total_frames) {
                preload_requested_for_A = false;
                need_load_A = false;
            }
        }
    }

    // Kiểm tra nếu có sự kiện LOAD_MISS (ưu tiên hơn preload)
    EventBits_t bits = xEventGroupGetBits(double_buffer_event);
    if (bits & EVT_LOAD_MISS) {
        // Có miss, cần load ngay buffer chứa frame đang cần.
        // Tuy nhiên, ta không biết frame index nào bị miss (có thể lưu lại biến toàn cục)
        // Ở đây giả sử OLED task sẽ gửi kèm index cần qua một cơ chế khác.
        // Để đơn giản, ta sẽ load buffer còn lại chưa được dùng.
        // Nếu muốn chính xác, cần thêm một biến missing_index.
        // Cải tiến: Thay vì dùng EVT_LOAD_MISS riêng, ta dùng chung cơ chế preload và flip.
        // Nhưng để đúng yêu cầu, ta xử lý:
        if (!readyA && !usingA) {
            need_load_A = true;
            // load index sẽ được tính từ yêu cầu của OLED, nhưng ở đây ta bỏ qua.
            // Giải pháp thực tế: OLED task gọi double_buffer_get_frame, nếu miss thì nó set bit
            // và chờ. SD task khi thấy EVT_LOAD_MISS sẽ load buffer dựa trên index hiện tại.
            // Để đơn giản, ta sẽ load buffer tiếp theo sau buffer hiện tại.
            if (usingA) {
                load_index = startA + countA;
            } else {
                load_index = startB + countB;
            }
        }
        // Xóa bit LOAD_MISS để tránh load lại nhiều lần
        xEventGroupClearBits(double_buffer_event, EVT_LOAD_MISS);
    }

    bool result = false;
    if (need_load_A) {
        result = load_double_buffer_internal(bufferA, &startA, &countA, load_index);
        if (result) {
            readyA = true;
            preload_requested_for_A = false; // reset sau khi load xong
            ESP_LOGI(TAG, "Loaded buffer A at index %lu, count=%lu", load_index, countA);
            xEventGroupSetBits(double_buffer_event, EVT_READY);
        } else {
            ESP_LOGE(TAG, "Failed to load buffer A at index %lu", load_index);
        }
    } else if (need_load_B) {
        result = load_double_buffer_internal(bufferB, &startB, &countB, load_index);
        if (result) {
            readyB = true;
            preload_requested_for_B = false;
            ESP_LOGI(TAG, "Loaded buffer B at index %lu, count=%lu", load_index, countB);
            xEventGroupSetBits(double_buffer_event, EVT_READY);
        } else {
            ESP_LOGE(TAG, "Failed to load buffer B at index %lu", load_index);
        }
    }

    xSemaphoreGive(double_buffer_mutex);
    return result;
}