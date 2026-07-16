/* ===================================================
 *  INCLUDE FILES
 * =================================================== */

#include <string.h>
#include "vs1053.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/* ===================================================
 *  MACROS / DEFINES
 * =================================================== */

/* VS1053_SPI_INIT_CLOCK_HZ/VS1053_SPI_RUN_CLOCK_HZ khai báo trong vs1053_config.h cùng các
   chân VS1053 khác - không khai báo riêng ở đây nữa */

/* Thời gian giữ mức reset / chờ ổn định sau reset (ms) - theo trình tự reset khuyến nghị
   trong datasheet VS1053b */
#define VS1053_RESET_DELAY_MS       100U

/* Số byte "end fill byte" cần gửi để xả hết dữ liệu còn dở trong FIFO nội bộ chip lúc dừng
   bài - con số chuẩn theo tài liệu VS1053 (kích thước FIFO decode lớn nhất có thể gặp) */
#define VS1053_STOP_FILLER_BYTES    2052U

/* Địa chỉ RAM nội bộ chip lưu "end fill byte" đúng cho codec/patch đang nạp - đọc gián tiếp
   qua SCI_WRAMADDR + SCI_WRAM (xem vs1053_start_song) */
#define VS1053_WRAM_ADDR_ENDFILLBYTE 0x1E06U

/* Thời gian tối đa chờ DREQ lên cao (ms) trước khi coi là chip treo/mất kết nối - xem
   vs1053_wait_dreq(). Rộng rãi so với thời gian DREQ deassert thực tế theo datasheet (tối đa
   vài ms ngay cả lúc xử lý nặng/nạp patch), nên không có nguy cơ timeout giả trong điều kiện
   chip hoạt động bình thường; đủ nhỏ để Mp3_Task không còn treo vô thời hạn khi chip thực sự
   không phản hồi (vd brown-out thoáng qua, ESD, lỗi phần cứng giữa chừng) */
#define VS1053_DREQ_TIMEOUT_MS      200U

/* ===================================================
 *  LOCAL VARIABLES
 * =================================================== */

/* Tag dùng cho ESP_LOGx trong module này */
static const char *TAG = "VS1053";

/* ===================================================
 *  LOCAL FUNCTION
 * =================================================== */

/**
 * @brief delay_ms
 * Delay theo ms, bọc lại vTaskDelay cho gọn tại các lời gọi trong file này.
 * @param u32Ms: thời gian delay (ms)
 * @return
 */
static inline void delay_ms(uint32_t u32Ms)
{
    vTaskDelay(pdMS_TO_TICKS(u32Ms));
}

/**
 * @brief vs1053_add_spi_devices
 * Add VS1053 làm 2 SPI device ĐỘC LẬP trên cùng SPI2_HOST_ID (xem spi.h): 1 cho kênh lệnh SCI
 * (spics_io_num = VS1053_XCS_PIN/XCS) và 1 cho kênh dữ liệu SDI (spics_io_num =
 * VS1053_XDCS_PIN/XDCS) - VS1053 có 2 chân CS vật lý riêng biệt cho 2 kênh này. Truyền thẳng
 * chân CS thật (KHÔNG dùng -1) để driver SPI Master của ESP-IDF tự động assert/deassert CS
 * quanh mỗi spi_device_transmit(), không còn tự gpio_set_level() thủ công như trước. Cùng 1
 * clockSpeedHz cho cả 2 device vì VS1053 chỉ có 1 clock nội bộ áp dụng chung cho SCI/SDI.
 * KHÔNG tự khởi tạo bus - Spi_Init() (spi.c) PHẢI được gọi thành công từ trước (app_main(),
 * trước khi tạo Mp3_Task). Đặt tên khác Spi_Init() (chữ hoa, spi.c - khởi tạo BUS dùng chung)
 * để tránh nhầm lẫn dù không đụng độ lúc biên dịch (C phân biệt hoa/thường, hàm này lại
 * `static` nên chỉ nhìn thấy trong file này) - hàm này chỉ ADD DEVICE lên bus đã có sẵn. Gọi 2
 * lần từ vs1053_init(): 1 lần lúc mới reset (clock thấp), 1 lần sau khi đã cấu hình xong
 * SCI_CLOCKF (clock chạy bình thường).
 * @param pDev: con trỏ device VS1053, ghi kết quả vào pDev->sci_handle/sdi_handle nếu thành công
 * @param clockSpeedHz: tốc độ SPI áp dụng cho cả 2 device (xem VS1053_SPI_INIT_CLOCK_HZ/
 *        VS1053_SPI_RUN_CLOCK_HZ trong vs1053_config.h)
 * @return ESP_OK nếu add cả 2 device thành công, mã lỗi esp_err_t khác nếu bất kỳ device nào
 *         thất bại (vd bus chưa được Spi_Init(), hoặc hết chỗ device trên bus)
 */
static esp_err_t vs1053_add_spi_devices(vs1053_handle_t *pDev, uint32_t clockSpeedHz)
{
    spi_device_interface_config_t lSciCfg = {
        .clock_speed_hz = clockSpeedHz,
        .mode = 0,                         /* SPI mode 0 */
        .spics_io_num = VS1053_XCS_PIN,     /* Driver SPI tự điều khiển CS qua chân này */
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };

    esp_err_t lRet = spi_bus_add_device(SPI2_HOST_ID, &lSciCfg, &pDev->sci_handle);
    if (lRet != ESP_OK)
    {
        return lRet;
    }

    spi_device_interface_config_t lSdiCfg = {
        .clock_speed_hz = clockSpeedHz,
        .mode = 0,
        .spics_io_num = VS1053_XDCS_PIN,     /* Driver SPI tự điều khiển CS qua chân này */
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };

    return spi_bus_add_device(SPI2_HOST_ID, &lSdiCfg, &pDev->sdi_handle);
}

/* ===================================================
 *  GLOBAL FUNCTION
 * =================================================== */

Std_ReturnType vs1053_wait_dreq(vs1053_handle_t *pDev)
{
    uint32_t lu32WaitedMs = 0U;

    while (gpio_get_level(pDev->dreq_pin) == 0)
    {
        /* Giới hạn thời gian chờ (VS1053_DREQ_TIMEOUT_MS) - trước đây vòng lặp này không có
           timeout, nếu chip treo/mất kết nối giữa chừng (DREQ kẹt mức thấp) thì Mp3_Task (task
           duy nhất gọi tới các hàm vs1053_*, kiến trúc Owner Task - xem srm.h) sẽ treo vĩnh
           viễn tại đây, không log, không cách phục hồi. Bên gọi (vs1053_write_sci/read_sci/
           send_buffer/reset) tự quyết định phản ứng với E_NOT_OK trả về, hàm này chỉ có trách
           nhiệm KHÔNG BAO GIỜ chờ vô thời hạn. */
        if (lu32WaitedMs >= VS1053_DREQ_TIMEOUT_MS)
        {
            ESP_LOGE(TAG, "DREQ timeout - VS1053 not responding");
            return E_NOT_OK;
        }

        delay_ms(1);
        lu32WaitedMs++;
    }

    return E_OK;
}

esp_err_t vs1053_write_sci(vs1053_handle_t *pDev, uint8_t reg, uint16_t value)
{
    /* Lệnh ghi SCI: byte 0 = 0x02 (write), byte 1 = địa chỉ thanh ghi, byte 2-3 = dữ liệu
       16-bit (big-endian) */
    spi_transaction_t lTrans = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8 * 4,
        .tx_data = {0x02, reg, (value >> 8) & 0xFF, value & 0xFF}
    };

    /* CS (VS1053_XCS_PIN/XCS) do driver SPI Master tự assert/deassert quanh transaction này
       qua spics_io_num đã cấu hình lúc spi_bus_add_device() (xem vs1053_add_spi_devices()) - không
       còn tự gpio_set_level() như bản trước */
    esp_err_t lRet = spi_device_transmit(pDev->sci_handle, &lTrans);

    if (lRet != ESP_OK)
    {
        ESP_LOGW(TAG, "SCI write failed for reg 0x%02X", reg);
    }

    /* Chờ DREQ ngay sau khi gửi xong, để lệnh SCI/SDI KẾ TIẾP (của bất kỳ hàm vs1053_* nào
       gọi sau, không nhất thiết cùng 1 lần gọi) luôn thấy chip đã sẵn sàng - tương đương
       "chờ trước khi gửi" xét trên toàn bộ chuỗi lệnh, chỉ khác vị trí đặt lệnh chờ. Chỉ ghi
       đè lRet thành ESP_ERR_TIMEOUT nếu bản thân SPI transaction ở trên đã thành công (lRet ==
       ESP_OK) - không che mất lỗi SPI thật sự (nếu có) bằng lỗi DREQ timeout */
    if ((lRet == ESP_OK) && (vs1053_wait_dreq(pDev) != E_OK))
    {
        lRet = ESP_ERR_TIMEOUT;
    }
    return lRet;
}

uint16_t vs1053_read_sci(vs1053_handle_t *pDev, uint8_t reg)
{
    uint8_t lau8TxData[4] = {0x03, reg, 0xFF, 0xFF};
    uint8_t lau8RxData[4] = {0};

    spi_transaction_t lTrans = {
        .length = 8 * 4,
        .tx_buffer = lau8TxData,
        .rx_buffer = lau8RxData
    };

    /* CS do driver SPI Master tự assert/deassert, cùng lý do với vs1053_write_sci() ở trên */
    esp_err_t lRet = spi_device_transmit(pDev->sci_handle, &lTrans);

    if (lRet != ESP_OK)
    {
        /* Không có cách trả lỗi qua giá trị trả về (uint16_t, không phải esp_err_t) - chỉ
           log cảnh báo, bên gọi sẽ nhận nội dung lau8RxData hiện có (rác hoặc 0) */
        ESP_LOGW(TAG, "SCI read failed for reg 0x%02X", reg);
    }

    /* Hàm này vẫn không có cách báo lỗi DREQ timeout qua giá trị trả về (uint16_t, không phải
       esp_err_t) - chỉ log cảnh báo để dễ chẩn đoán, giống hệt cách xử lý lỗi SPI transaction
       ở trên. Quan trọng hơn: vs1053_wait_dreq() nay có giới hạn thời gian chờ
       (VS1053_DREQ_TIMEOUT_MS) nên lời gọi này KHÔNG còn treo vô thời hạn nếu chip mất kết
       nối, dù hàm vẫn trả về nội dung lau8RxData hiện có (rác hoặc 0) như trước */
    if (vs1053_wait_dreq(pDev) != E_OK)
    {
        ESP_LOGW(TAG, "DREQ timeout after SCI read for reg 0x%02X", reg);
    }
    return (lau8RxData[2] << 8) | lau8RxData[3];
}

esp_err_t vs1053_send_buffer(vs1053_handle_t *pDev, const uint8_t *pData, size_t len)
{
    if (len > VS1053_CHUNK_SIZE)
    {
        ESP_LOGW(TAG, "Buffer size exceeds chunk limit");
        len = VS1053_CHUNK_SIZE;
    }

    /* Chờ DREQ TRƯỚC khi gửi dữ liệu - đảm bảo FIFO nội bộ chip còn chỗ trống, khác với
       vs1053_write_sci()/vs1053_read_sci() (chờ sau) vì đây là đường dữ liệu (SDI, chân DCS)
       tách biệt hoàn toàn khỏi đường lệnh (SCI, chân CS). Đây là điểm gọi vs1053_wait_dreq()
       QUAN TRỌNG NHẤT trong toàn bộ driver - Mp3_StreamSong() (mp3.c) gọi hàm này cho MỖI
       khối dữ liệu mp3 trong lúc phát nhạc, nên đây chính là nơi 1 chip bị treo giữa chừng sẽ
       lộ ra đầu tiên. Không gửi SPI nếu DREQ timeout - chip coi như không sẵn sàng/mất kết
       nối, gửi tiếp cũng vô nghĩa */
    if (vs1053_wait_dreq(pDev) != E_OK)
    {
        return ESP_ERR_TIMEOUT;
    }

    spi_transaction_t lTrans = {
        .length = 8 * len,
        .tx_buffer = pData,
        .rx_buffer = NULL
    };

    /* CS (VS1053_XDCS_PIN/XDCS) do driver SPI Master tự assert/deassert qua sdi_handle - khác
       sci_handle ở trên vì đây là kênh dữ liệu (SDI), CS vật lý riêng biệt với kênh lệnh (SCI) */
    esp_err_t lRet = spi_device_transmit(pDev->sdi_handle, &lTrans);

    return lRet;
}

esp_err_t vs1053_reset(vs1053_handle_t *pDev)
{
    /* Kéo RESET xuống thấp, đợi ổn định, rồi thả RESET lên cao và đợi thêm - đúng trình tự
       reset phần cứng theo datasheet VS1053b. KHÔNG còn tự kéo CS/DCS lên cao ở đây như bản
       trước - CS của cả 2 kênh SCI/SDI đã được driver SPI Master tự đưa về trạng thái nhàn
       rỗi (high) ngay khi spi_bus_add_device() chạy xong (trong vs1053_add_spi_devices()), và
       vs1053_init() nay gọi vs1053_add_spi_devices() TRƯỚC vs1053_reset() (xem vs1053_init()) nên CS
       luôn ở đúng trạng thái idle suốt quá trình reset mà không cần set tay */
    gpio_set_level(pDev->reset_pin, 0);
    delay_ms(VS1053_RESET_DELAY_MS);
    gpio_set_level(pDev->reset_pin, 1);
    delay_ms(VS1053_RESET_DELAY_MS);

    /* Sau khi thả RESET, chờ DREQ lên cao mới coi là chip đã sẵn sàng nhận lệnh SCI đầu tiên.
       DREQ không lên cao trong giới hạn thời gian chờ nghĩa là chip không có/đấu sai dây/hỏng
       ngay từ bước reset - trả ESP_ERR_TIMEOUT thay vì luôn ESP_OK như bản trước, để
       vs1053_init() (nếu sau này cần) có thể phân biệt được, dù hiện tại vs1053_test_comm()
       gọi ngay sau đó đã tự phát hiện lại đúng tình huống này qua kiểm tra DREQ riêng */
    if (vs1053_wait_dreq(pDev) != E_OK)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t vs1053_init(vs1053_handle_t *pDev)
{
    /* KHÔNG tự cấu hình GPIO ở đây - Gpio_Init() (driver/gpio/gpio.c) đã cấu hình sẵn reset
       (output) và dreq (input) từ app_main(), TRƯỚC KHI Mp3_Task được tạo. CS/DCS không còn
       là GPIO thuần do module này quản lý - driver SPI Master tự cấu hình 2 chân đó ngay
       trong vs1053_add_spi_devices() bên dưới (qua spics_io_num).

       Add SPI device (sci_handle/sdi_handle) TRƯỚC vs1053_reset() - ĐỔI THỨ TỰ so với bản
       trước (trước đây reset rồi mới add device) để driver SPI đưa CS của cả 2 kênh về trạng
       thái nhàn rỗi (high) ngay từ bước này, đảm bảo CS ở đúng trạng thái trong suốt trình tự
       reset ngay sau. spi_bus_add_device() không tự gửi bất kỳ transaction nào (chỉ đăng ký
       device + cấu hình chân CS), nên đổi thứ tự này không phát sinh giao tiếp SPI sớm ngoài
       ý muốn - an toàn để làm trước reset, không ảnh hưởng timing DREQ/protocol SCI-SDI */
    esp_err_t lRet = vs1053_add_spi_devices(pDev, VS1053_SPI_INIT_CLOCK_HZ);
    if (lRet != ESP_OK)
    {
        ESP_LOGE(TAG, "vs1053_add_spi_devices failed: %s", esp_err_to_name(lRet));
        return lRet;
    }

    /* Reset phần cứng trước khi đụng gì tới thanh ghi - chip cần ở trạng thái sạch mới giao
       tiếp đáng tin cậy */
    vs1053_reset(pDev);

    /* Kiểm tra kết nối - phát hiện sớm trường hợp chip không có/đấu sai dây trước khi cấu
       hình thêm gì khác */
    if (vs1053_test_comm(pDev) == E_NOT_OK)
    {
        ESP_LOGE(TAG, "VS1053 communication test failed");
        return ESP_FAIL;
    }

    /* Cấu hình clock (3.0x = 12.288 MHz) - PHẢI làm trước khi nâng tốc độ SPI bên dưới,
       nếu không SPI sẽ chạy nhanh hơn khả năng xử lý thật của chip */
    vs1053_write_sci(pDev, SCI_CLOCKF, 0x6000);

    /* Cấu hình lấy mẫu 44.1kHz, STEREO (bit thấp nhất = 0). LƯU Ý: theo datasheet VS1053b,
       bit 0 của SCI_AUDATA là cờ mono/stereo (1 = ép mono), KHÔNG phải 1 phần của sample
       rate - dùng đúng 44100 (số chẵn), không phải 44101, để không vô tình ép chip sang
       mono */
    vs1053_write_sci(pDev, SCI_AUDATA, 44100);

    /* Cấu hình chế độ: SDINEW (bắt buộc) + LINE1 (nguồn vào từ input line thay vì mic) */
    vs1053_write_sci(pDev, SCI_MODE, SM_SDINEW | SM_LINE1);

    /* Thiết lập volume mặc định (50%) */
    vs1053_set_volume(pDev, 50);

    /* Đã cấu hình clock xong - nâng tốc độ SPI lên mức chạy bình thường cho CẢ 2 device để
       tăng thông lượng gửi dữ liệu MP3. Phải remove device cũ (tốc độ thấp) rồi add lại với
       config mới, ESP-IDF không cho đổi tốc độ 1 device đang tồn tại */
    esp_err_t lRemoveSciRet = spi_bus_remove_device(pDev->sci_handle);
    esp_err_t lRemoveSdiRet = spi_bus_remove_device(pDev->sdi_handle);

    if ((lRemoveSciRet != ESP_OK) || (lRemoveSdiRet != ESP_OK))
    {
        /* Hiếm khi xảy ra với handle vừa mới add thành công ở trên, nhưng KHÔNG được tiếp tục
           gọi vs1053_add_spi_devices() bên dưới nếu gỡ thất bại - hàm đó sẽ ghi đè thẳng
           pDev->sci_handle/sdi_handle bằng handle mới, làm mất khả năng gỡ lại handle CŨ (nếu
           gỡ thất bại, handle cũ có thể vẫn còn tồn tại trên bus) sau này - rò rỉ vĩnh viễn 1
           slot device trên SPI2_HOST_ID. Coi đây là lỗi init (dừng lại an toàn) thay vì chỉ
           log cảnh báo rồi tiếp tục như trước - Mp3_Task đã có sẵn cơ chế halt êm khi
           vs1053_init() trả về khác ESP_OK (xem Mp3_Task, mp3.c) */
        ESP_LOGE(TAG, "spi_bus_remove_device failed (SCI=%s, SDI=%s) - aborting speed raise to avoid handle leak",
                 esp_err_to_name(lRemoveSciRet), esp_err_to_name(lRemoveSdiRet));
        return ESP_FAIL;
    }

    esp_err_t lAddRet = vs1053_add_spi_devices(pDev, VS1053_SPI_RUN_CLOCK_HZ);
    if (lAddRet != ESP_OK)
    {
        /* Không nâng được tốc độ SPI -> sci_handle/sdi_handle không còn chắc chắn hợp lệ để
           dùng tiếp, coi như init thất bại thay vì âm thầm chạy với handle hỏng */
        ESP_LOGE(TAG, "Failed to raise SPI clock speed after init: %s", esp_err_to_name(lAddRet));
        return lAddRet;
    }

    ESP_LOGI(TAG, "VS1053 initialized successfully");
    return ESP_OK;
}

esp_err_t vs1053_soft_reset(vs1053_handle_t *pDev)
{
    uint16_t lu16Mode = vs1053_read_sci(pDev, SCI_MODE);
    lu16Mode |= SM_RESET;
    esp_err_t lRet = vs1053_write_sci(pDev, SCI_MODE, lu16Mode);

    /* SM_RESET làm chip trả SCI_CLOCKF về mặc định (không nhân clock) - ghi lại ngay tại
       đây để hàm AN TOÀN khi gọi độc lập (không phụ thuộc bên gọi có nhớ khôi phục CLOCKF
       hay không), nếu không SPI bus đang chạy nhanh hơn clock nội bộ mặc định có thể giao
       tiếp sai ở lệnh kế tiếp */
    vs1053_write_sci(pDev, SCI_CLOCKF, 0x6000);

    return lRet;
}

esp_err_t vs1053_set_volume(vs1053_handle_t *pDev, uint8_t volume)
{
    if (volume > 100)
    {
        volume = 100;
    }
    pDev->volume = volume;

    /* SCI_VOL: giá trị càng NHỎ càng to (0x00 = to nhất, 0xFE = nhỏ nhất theo từng nấc),
       nên phải đảo chiều volume (0-100, 100 = to nhất) trước khi ghi thanh ghi */
    uint16_t lu16Vol = ((100 - volume) * 0xFEU / 100U);
    uint16_t lu16Val = (lu16Vol << 8) | lu16Vol;
    return vs1053_write_sci(pDev, SCI_VOL, lu16Val);
}

esp_err_t vs1053_set_tone(vs1053_handle_t *pDev, uint16_t tone)
{
    return vs1053_write_sci(pDev, SCI_BASS, tone);
}

esp_err_t vs1053_switch_to_mp3_mode(vs1053_handle_t *pDev)
{
    /* vs1053_soft_reset() đã tự khôi phục SCI_CLOCKF bên trong - không cần ghi lại ở đây */
    esp_err_t lRet = vs1053_soft_reset(pDev);
    vs1053_write_sci(pDev, SCI_MODE, SM_SDINEW | SM_LINE1);
    vs1053_set_volume(pDev, pDev->volume);
    return lRet;
}

void vs1053_start_song(vs1053_handle_t *pDev)
{
    /* Đọc "end fill byte" thật từ RAM nội bộ chip (địa chỉ VS1053_WRAM_ADDR_ENDFILLBYTE) -
       giá trị này phụ thuộc codec/patch đang nạp, không phải lúc nào cũng là 0. Đọc gián
       tiếp qua SCI_WRAMADDR (chọn địa chỉ) rồi SCI_WRAM (đọc dữ liệu tại địa chỉ đó), theo
       đúng thủ tục truy cập RAM nội bộ của VS1053. Dùng đúng byte này khi flush buffer lúc
       vs1053_stop_song() để tránh tiếng click nhỏ so với hardcode 0 */
    vs1053_write_sci(pDev, SCI_WRAMADDR, VS1053_WRAM_ADDR_ENDFILLBYTE);
    pDev->end_fill_byte = (uint8_t)(vs1053_read_sci(pDev, SCI_WRAM) & 0xFF);
}

void vs1053_stop_song(vs1053_handle_t *pDev)
{
    /* Gửi VS1053_STOP_FILLER_BYTES byte "im lặng" (end_fill_byte) để xả hết dữ liệu còn dở
       trong FIFO nội bộ chip - xem giới hạn đã biết (không SM_CANCEL, không poll) trong doc
       comment ở vs1053.h */
    uint8_t lau8Filler[VS1053_CHUNK_SIZE];
    memset(lau8Filler, pDev->end_fill_byte, VS1053_CHUNK_SIZE);

    for (uint32_t lu32Sent = 0; lu32Sent < VS1053_STOP_FILLER_BYTES; lu32Sent += VS1053_CHUNK_SIZE)
    {
        vs1053_send_buffer(pDev, lau8Filler, VS1053_CHUNK_SIZE);
    }

    delay_ms(VS1053_RESET_DELAY_MS);
}

void vs1053_cancel_song(vs1053_handle_t *pDev)
{
    uint16_t lu16Mode = vs1053_read_sci(pDev, SCI_MODE);
    lu16Mode |= SM_CANCEL;
    vs1053_write_sci(pDev, SCI_MODE, lu16Mode);
}

uint16_t vs1053_get_decoded_time(vs1053_handle_t *pDev)
{
    return vs1053_read_sci(pDev, SCI_DECODE_TIME);
}

void vs1053_clear_decoded_time(vs1053_handle_t *pDev)
{
    vs1053_write_sci(pDev, SCI_DECODE_TIME, 0);
}

Std_ReturnType vs1053_test_comm(vs1053_handle_t *pDev)
{
    if (gpio_get_level(pDev->dreq_pin) == 0)
    {
        ESP_LOGE(TAG, "VS1053 not detected (DREQ low)");
        return E_NOT_OK;
    }

    /* Ghi 1 giá trị test bất kỳ vào SCI_VOL rồi đọc lại - đọc đúng giá trị vừa ghi nghĩa là
       kênh SPI hoạt động đúng cả 2 chiều. Luôn khôi phục lại giá trị cũ ngay sau đó, không
       ảnh hưởng volume thật đang phát */
    uint16_t lu16OldVol = vs1053_read_sci(pDev, SCI_VOL);
    vs1053_write_sci(pDev, SCI_VOL, 0x1234);
    uint16_t lu16Test = vs1053_read_sci(pDev, SCI_VOL);
    vs1053_write_sci(pDev, SCI_VOL, lu16OldVol);

    return ((lu16Test == 0x1234) ? E_OK : E_NOT_OK);
}

void vs1053_print_details(vs1053_handle_t *pDev)
{
    ESP_LOGI(TAG, "VS1053 Details:");
    ESP_LOGI(TAG, "  Mode:    0x%04X", vs1053_read_sci(pDev, SCI_MODE));
    ESP_LOGI(TAG, "  Status:  0x%04X", vs1053_read_sci(pDev, SCI_STATUS));
    ESP_LOGI(TAG, "  ClockF:  0x%04X", vs1053_read_sci(pDev, SCI_CLOCKF));
    ESP_LOGI(TAG, "  Volume:  0x%04X", vs1053_read_sci(pDev, SCI_VOL));
    ESP_LOGI(TAG, "  Audio:   0x%04X", vs1053_read_sci(pDev, SCI_AUDATA));
    ESP_LOGI(TAG, "  HDAT0:   0x%04X", vs1053_read_sci(pDev, SCI_HDAT0));
    ESP_LOGI(TAG, "  HDAT1:   0x%04X", vs1053_read_sci(pDev, SCI_HDAT1));
}
