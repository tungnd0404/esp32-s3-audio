#ifndef VS1053_H
#define VS1053_H

/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "std_types.h"
#include "spi.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* --- Chân kết nối vật lý VS1053 - đổi tại đây nếu đấu dây khác board --- */
#define VS1053_CS_PIN       5     /* Chip select cho lệnh SCI (XCS) */
#define VS1053_DCS_PIN      26    /* Chip select cho dữ liệu SDI (XDCS) */
#define VS1053_DREQ_PIN     27    /* Data request - chip kéo cao khi sẵn sàng nhận lệnh/data */
#define VS1053_RESET_PIN    32    /* Reset phần cứng, tích cực mức thấp */
/* Chân MOSI/MISO/SCLK không còn khai báo ở đây - dùng chung cho cả SPI_HOST_ID (không riêng
   VS1053), xem SPI_MOSI_PIN/MISO_PIN/SCLK_PIN trong spi.h */

/* --- Địa chỉ thanh ghi SCI (Serial Control Interface) - theo datasheet VS1053b --- */
#define SCI_MODE            0x00U
#define SCI_STATUS          0x01U
#define SCI_BASS            0x02U
#define SCI_CLOCKF          0x03U
#define SCI_DECODE_TIME     0x04U
#define SCI_AUDATA          0x05U
#define SCI_WRAM            0x06U
#define SCI_WRAMADDR        0x07U
#define SCI_HDAT0           0x08U
#define SCI_HDAT1           0x09U
#define SCI_AIADDR          0x0AU
#define SCI_VOL             0x0BU
#define SCI_AICTRL0         0x0CU
#define SCI_AICTRL1         0x0DU
#define SCI_AICTRL2         0x0EU
#define SCI_AICTRL3         0x0FU

/* --- Các bit trong SCI_MODE --- */
#define SM_SDINEW           (1U << 11)    /* Bắt buộc bật - chuẩn giao tiếp SDI mới */
#define SM_RESET            (1U << 2)     /* Set để trigger soft reset */
#define SM_CANCEL           (1U << 3)     /* Set để huỷ bài đang giải mã dở */
#define SM_TESTS            (1U << 5)     /* Bật chế độ test nội bộ chip */
#define SM_LINE1            (1U << 14)    /* Chọn input LINE1 thay vì MIC */

/* Kích thước tối đa 1 lần gửi dữ liệu SDI (byte) - giới hạn phần cứng của VS1053, KHÔNG phải
   tham số tinh chỉnh được, xem vs1053_send_buffer() */
#define VS1053_CHUNK_SIZE   32U

/* ===================================================
 *  TYPE DEFINITIONS
 * =================================================== */

/* Thông tin 1 thiết bị VS1053 - mỗi field được các hàm vs1053_* đọc/ghi trực tiếp, không có
   getter/setter riêng vì đây là driver cấp thấp, chỉ Mp3_Task (owner phần cứng duy nhất,
   kiến trúc Owner Task - xem srm.h) được phép đụng vào */
typedef struct {
    /* Handle SPI do ESP-IDF cấp, tạo trong vs1053_init() (qua spi_init() nội bộ), dùng lại
       cho mọi giao dịch SCI/SDI sau đó */
    spi_device_handle_t spi_handle;
    gpio_num_t cs_pin;
    gpio_num_t dcs_pin;
    gpio_num_t dreq_pin;
    gpio_num_t reset_pin;
    /* Volume hiện tại (0-100%), lưu lại để vs1053_switch_to_mp3_mode() khôi phục đúng mức
       cũ sau khi soft reset (soft reset làm SCI_VOL về giá trị mặc định của chip) */
    uint8_t volume;
    /* Byte "im lặng" đúng chuẩn của codec/patch đang nạp, đọc từ RAM nội bộ chip trong
       vs1053_start_song() - dùng để flush buffer êm ái lúc vs1053_stop_song(), xem giải
       thích chi tiết ở 2 hàm đó */
    uint8_t end_fill_byte;
    /* Hiện chưa có hàm nào ghi field này (dự phòng cho việc đọc chip version qua SCI_STATUS
       sau này nếu cần phân biệt VS1002/VS1011/VS1053...) */
    uint8_t chip_version;
} vs1053_handle_t;

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

/**
 * @brief vs1053_reset
 * Thực hiện đúng trình tự reset phần cứng qua chân RESET (kéo thấp, đợi ổn định, thả cao,
 * đợi tiếp) rồi chờ DREQ báo chip sẵn sàng nhận lệnh. CHỈ làm reset - không đụng tới SPI bus
 * hay bất kỳ thanh ghi SCI nào, nên có thể gọi lại độc lập (vd sau khi nghi ngờ chip treo)
 * mà không cần khởi tạo lại toàn bộ như vs1053_init(). Yêu cầu GPIO cs/dcs/reset/dreq đã
 * được cấu hình (gpio_config) từ trước - vs1053_init() tự lo việc này trước khi gọi hàm này.
 * @param pDev: con trỏ device VS1053, các field pin đã được gán giá trị thật của board
 * @return ESP_OK luôn (thao tác GPIO thuần tuý, không có đường lỗi)
 */
esp_err_t vs1053_reset(vs1053_handle_t *pDev);

/**
 * @brief vs1053_init
 * Khởi tạo đầy đủ 1 lần: cấu hình GPIO, reset phần cứng (vs1053_reset), add VS1053 làm 1 SPI
 * device trên SPI_HOST_ID ở tốc độ thấp (an toàn cho lúc chip chưa ổn định clock), kiểm tra
 * kết nối (vs1053_test_comm), cấu hình clock/audio/mode/volume mặc định, rồi mới nâng tốc độ
 * SPI lên tốc độ chạy bình thường. YÊU CẦU Spi_Init() (spi.c) đã được gọi thành công từ trước
 * (thường trong app_main(), trước khi tạo Mp3_Task) - hàm này không tự khởi tạo bus, chỉ add
 * device của riêng nó lên bus đã có sẵn (xem spi.h để biết lý do tách ra). CHỈ nên gọi đúng
 * 1 lần cho cùng 1 pDev (gọi lại sẽ add thêm 1 device SPI mới, rò rỉ handle cũ).
 * @param pDev: con trỏ device VS1053, các field pin PHẢI được gán giá trị thật của board
 *        TRƯỚC khi gọi (xem VS1053_CS_PIN/VS1053_DCS_PIN/VS1053_DREQ_PIN/VS1053_RESET_PIN)
 * @return ESP_OK nếu khởi tạo thành công, mã lỗi esp_err_t khác nếu add device lên SPI bus lỗi
 *         (vd Spi_Init() chưa được gọi) hoặc không phát hiện được VS1053 (DREQ không lên
 *         cao / test comm thất bại)
 */
esp_err_t vs1053_init(vs1053_handle_t *pDev);

/**
 * @brief vs1053_soft_reset
 * Reset mềm qua thanh ghi (set bit SM_RESET trong SCI_MODE), rồi tự khôi phục lại SCI_CLOCKF
 * ngay sau đó - soft reset làm chip trả clock multiplier về mặc định (không nhân), nếu không
 * ghi lại ngay thì SPI bus (đang chạy nhanh hơn clock nội bộ mặc định) có thể giao tiếp sai
 * ở lần gọi kế tiếp. Tự khôi phục ở đây để hàm AN TOÀN khi gọi độc lập, không phụ thuộc việc
 * bên gọi có nhớ ghi lại CLOCKF hay không.
 * @param pDev: con trỏ device VS1053 (đã vs1053_init())
 * @return ESP_OK nếu ghi SCI_MODE thành công, mã lỗi esp_err_t khác nếu SPI transaction lỗi
 */
esp_err_t vs1053_soft_reset(vs1053_handle_t *pDev);

/* --- Các hàm giao tiếp cơ bản --- */

/**
 * @brief vs1053_wait_dreq
 * Chờ (poll, không dùng ngắt) tới khi chân DREQ lên mức cao - chip báo hiệu đã sẵn sàng nhận
 * lệnh SCI hoặc dữ liệu SDI tiếp theo. Block vô thời hạn (không timeout) - nếu chip không
 * phản hồi, hàm sẽ treo mãi; đây là hành vi đã biết, chấp nhận được vì toàn bộ codebase chỉ
 * gọi các hàm vs1053_* từ đúng 1 task (Mp3_Task, kiến trúc Owner Task - xem srm.h).
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_wait_dreq(vs1053_handle_t *pDev);

/**
 * @brief vs1053_write_sci
 * Ghi 1 giá trị 16-bit vào 1 thanh ghi SCI qua SPI (lệnh 0x02 theo giao thức VS1053), dùng
 * chân CS. Tự chờ DREQ ngay sau khi gửi xong để đảm bảo lệnh kế tiếp (của bất kỳ hàm vs1053_*
 * nào gọi sau) luôn thấy chip đã sẵn sàng.
 * @param pDev: con trỏ device VS1053
 * @param reg: địa chỉ thanh ghi SCI cần ghi (xem các macro SCI_*)
 * @param value: giá trị 16-bit cần ghi
 * @return ESP_OK nếu SPI transaction thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t vs1053_write_sci(vs1053_handle_t *pDev, uint8_t reg, uint16_t value);

/**
 * @brief vs1053_read_sci
 * Đọc giá trị 16-bit từ 1 thanh ghi SCI qua SPI (lệnh 0x03). Tự chờ DREQ ngay sau khi đọc
 * xong, cùng lý do với vs1053_write_sci().
 * @param pDev: con trỏ device VS1053
 * @param reg: địa chỉ thanh ghi SCI cần đọc (xem các macro SCI_*)
 * @return giá trị 16-bit đọc được. Nếu SPI transaction thất bại, trả về nội dung rx buffer
 *         hiện có (thường là 0, xem ESP_LOGW cảnh báo trong log) - hàm không có cách báo lỗi
 *         qua giá trị trả về vì kiểu trả về là dữ liệu thô, không phải esp_err_t
 */
uint16_t vs1053_read_sci(vs1053_handle_t *pDev, uint8_t reg);

/**
 * @brief vs1053_send_buffer
 * Gửi tối đa VS1053_CHUNK_SIZE byte dữ liệu MP3 thô qua SDI (chân DCS), tự chờ DREQ trước
 * khi gửi để đảm bảo chip còn chỗ trống trong FIFO nội bộ.
 * @param pDev: con trỏ device VS1053
 * @param pData: buffer dữ liệu MP3 cần gửi
 * @param len: số byte cần gửi - nếu > VS1053_CHUNK_SIZE sẽ bị CẮT BỚT còn đúng
 *        VS1053_CHUNK_SIZE (kèm cảnh báo log), KHÔNG tự lặp gửi nhiều lần - bên gọi phải tự
 *        chia nhỏ nếu cần gửi nhiều hơn 1 chunk (xem cách Mp3_StreamSong trong mp3.c dùng)
 * @return ESP_OK nếu SPI transaction thành công, mã lỗi esp_err_t khác nếu thất bại
 */
esp_err_t vs1053_send_buffer(vs1053_handle_t *pDev, const uint8_t *pData, size_t len);

/* --- Các hàm điều khiển phát nhạc --- */

/**
 * @brief vs1053_set_volume
 * Thiết lập âm lượng, áp dụng đều cho cả 2 kênh trái/phải.
 * @param pDev: con trỏ device VS1053
 * @param volume: 0 (im lặng) .. 100 (to nhất) - giá trị > 100 sẽ tự bị kẹp về 100
 * @return ESP_OK nếu ghi thành công, mã lỗi esp_err_t khác nếu SPI transaction lỗi
 */
esp_err_t vs1053_set_volume(vs1053_handle_t *pDev, uint8_t volume);

/**
 * @brief vs1053_set_tone
 * Ghi thẳng giá trị vào thanh ghi SCI_BASS (điều chỉnh bass/treble) - hàm KHÔNG tự đóng gói
 * tham số bass/treble riêng lẻ thành API thân thiện hơn vì hiện chưa có UI/tính năng nào cần
 * chỉnh bass/treble; nếu sau này cần, nên viết thêm 1 hàm bọc ngoài tự đóng gói đúng bit
 * layout dưới đây, không sửa lại hàm này.
 *
 * Bit layout SCI_BASS (datasheet VS1053b):
 *  - bit 15-12: Treble control, bước 1.5dB (signed, -8..+7)
 *  - bit 11-8:  Tần số giới hạn dưới cho treble, bước 1000Hz
 *  - bit 7-4:   Bass Enhancement, bước 1dB (0..15)
 *  - bit 3-0:   Tần số giới hạn dưới cho bass, bước 10Hz
 *
 * @param pDev: con trỏ device VS1053
 * @param tone: giá trị 16-bit ghi thẳng vào SCI_BASS, tự đóng gói theo bit layout ở trên
 * @return ESP_OK nếu ghi thành công, mã lỗi esp_err_t khác nếu SPI transaction lỗi
 */
esp_err_t vs1053_set_tone(vs1053_handle_t *pDev, uint16_t tone);

/**
 * @brief vs1053_switch_to_mp3_mode
 * Soft reset rồi cấu hình lại mode/volume để quay về chế độ phát MP3 - dùng khi cần CHUYỂN
 * LẠI chế độ MP3 sau khi đã đổi sang mode khác (hiện chưa có tính năng nào trong project đổi
 * mode, nên hàm này chưa được gọi ở đâu - dự phòng cho tương lai). KHÔNG cần gọi ngay sau
 * vs1053_init() vì nội dung gần như trùng lặp với những gì vs1053_init() đã tự làm.
 * @param pDev: con trỏ device VS1053 (đã vs1053_init())
 * @return ESP_OK nếu các bước ghi thanh ghi thành công, mã lỗi esp_err_t khác nếu bước soft
 *         reset thất bại
 */
esp_err_t vs1053_switch_to_mp3_mode(vs1053_handle_t *pDev);

/**
 * @brief vs1053_start_song
 * Chuẩn bị trạng thái decode nội bộ cho 1 bài MỚI: đọc "end fill byte" thật từ RAM nội bộ
 * chip (địa chỉ 0x1E06, phụ thuộc codec/patch đang nạp) lưu vào pDev->end_fill_byte, dùng để
 * flush buffer êm ái lúc vs1053_stop_song() sau này (tránh tiếng click nhỏ nếu dùng sai byte
 * im lặng). KHÔNG tự gọi vs1053_send_buffer/vs1053_wait_dreq gì thêm - bên gọi (Mp3_Task) tự
 * bắt đầu bơm dữ liệu MP3 ngay sau khi gọi hàm này.
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_start_song(vs1053_handle_t *pDev);

/**
 * @brief vs1053_stop_song
 * Xả hết dữ liệu còn dở trong FIFO nội bộ chip bằng cách gửi 2052 byte "end fill byte" (con
 * số chuẩn theo tài liệu VS1053), rồi delay để chip xử lý xong. KHÔNG set bit SM_CANCEL và
 * KHÔNG poll xác nhận chip đã dừng hẳn - đủ dùng cho trường hợp hiện tại (chỉ gọi khi hết
 * bài tự nhiên - EOF, xem Mp3_StreamSong trong mp3.c), nhưng nếu sau này cần tính năng "dừng
 * đột ngột giữa bài" (vd nút Stop/Skip khi đang phát dở 1 frame), nên bổ sung SM_CANCEL +
 * poll SCI_MODE xác nhận bit tự clear (xem vs1053_cancel_song), vì gửi filler đơn thuần
 * không đảm bảo huỷ ngay 1 frame đang decode dở.
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_stop_song(vs1053_handle_t *pDev);

/**
 * @brief vs1053_cancel_song
 * Set bit SM_CANCEL trong SCI_MODE để yêu cầu chip huỷ ngay bài đang giải mã dở - chip sẽ tự
 * clear bit này khi đã xử lý xong yêu cầu huỷ (KHÔNG được hàm này tự poll xác nhận, bên gọi
 * tự đọc lại SCI_MODE nếu cần chắc chắn). Hiện chưa có nơi nào trong project gọi hàm này
 * (Mp3_StreamSong dùng vs1053_stop_song() cho trường hợp EOF tự nhiên) - dự phòng cho tính
 * năng "dừng giữa bài" sau này.
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_cancel_song(vs1053_handle_t *pDev);

/**
 * @brief vs1053_get_decoded_time
 * Đọc thanh ghi SCI_DECODE_TIME - thời gian (giây) đã giải mã của bài đang phát, dùng để
 * đồng bộ animation (xem Srm_Mp3GetDecodeTime trong srm.c/driver/buffer/sync_frame.c).
 * @param pDev: con trỏ device VS1053
 * @return thời gian đã giải mã, tính bằng giây
 */
uint16_t vs1053_get_decoded_time(vs1053_handle_t *pDev);

/**
 * @brief vs1053_clear_decoded_time
 * Ghi 0 vào SCI_DECODE_TIME - reset mốc thời gian giải mã về 0 (vd dùng khi cần đồng bộ lại
 * animation từ đầu mà không muốn đổi bài). Hiện chưa có nơi nào trong project gọi hàm này
 * (SyncFrame_Init() trong sync_frame.c tự quản lý mốc đồng bộ ở tầng ứng dụng thay vì reset
 * thẳng thanh ghi chip) - dự phòng cho nhu cầu sau này.
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_clear_decoded_time(vs1053_handle_t *pDev);

/* --- Hàm kiểm tra kết nối / debug --- */

/**
 * @brief vs1053_test_comm
 * Kiểm tra kết nối SPI với VS1053: trước tiên check DREQ đang ở mức cao (chip đã ra khỏi
 * reset), sau đó ghi 1 giá trị test (0x1234) vào SCI_VOL rồi đọc lại - đọc đúng giá trị vừa
 * ghi nghĩa là kênh SPI hoạt động đúng cả 2 chiều. Luôn khôi phục lại giá trị SCI_VOL cũ sau
 * khi test xong, không làm ảnh hưởng tới volume thật đang phát.
 * @param pDev: con trỏ device VS1053
 * @return E_OK nếu giao tiếp SPI hoạt động đúng, E_NOT_OK nếu DREQ không lên cao hoặc giá
 *         trị đọc lại không khớp giá trị test đã ghi
 */
Std_ReturnType vs1053_test_comm(vs1053_handle_t *pDev);

/**
 * @brief vs1053_print_details
 * In ra log (ESP_LOGI) nội dung các thanh ghi chính (Mode/Status/ClockF/Volume/Audio/
 * HDAT0/HDAT1) - tiện ích debug, không dùng trong luồng chạy bình thường. Hiện chưa có nơi
 * nào trong project gọi hàm này - gọi thủ công khi cần soi trạng thái chip lúc debug.
 * @param pDev: con trỏ device VS1053
 * @return
 */
void vs1053_print_details(vs1053_handle_t *pDev);

#endif /* VS1053_H */
