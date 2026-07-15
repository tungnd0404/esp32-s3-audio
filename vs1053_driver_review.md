# Đánh giá driver VS1053 (`driver/vs1053/`)

Phạm vi: `vs1053.h` + `vs1053.c` (251 dòng), đối chiếu với toàn bộ call site thực tế trong project (chỉ `task/mp3.c` dùng driver này).

## Tóm tắt nhanh

Driver **chạy được** cho luồng chính (init → stream → stop), nhưng **chưa "chuẩn"** theo 3 nghĩa:

1. **1 bug thật, âm thầm sai chức năng**: cấu hình audio ghi là "stereo" nhưng giá trị thực tế set chip sang **mono**.
2. **2 hàm khai báo nhưng không có thân hàm** (`vs1053_reset`, `vs1053_set_tone`) — chỉ chưa vỡ vì chưa ai gọi tới, gọi là lỗi link ngay.
3. **4 hàm tồn tại nhưng không ai gọi** (dead code) — không sai, nhưng là diện tích chưa được test/dùng thật.

---

## 1. Bảng liệt kê toàn bộ hàm

| Hàm | Chữ ký | Chức năng | Ai gọi | Trạng thái |
|---|---|---|---|---|
| `vs1053_init` | `esp_err_t (vs1053_handle_t *dev)` | Cấu hình GPIO, reset phần cứng, khởi tạo SPI (200kHz), test comm, set clock/audio/mode/volume mặc định, rồi nâng SPI lên 4MHz | `Mp3_Task` (mp3.c) | Đang dùng, có bug (mục 2.1) |
| `vs1053_reset` | `esp_err_t (vs1053_handle_t *dev)` | *(theo tên: reset cứng)* | **Không ai** | **Khai báo, KHÔNG có thân hàm** |
| `vs1053_soft_reset` | `esp_err_t (vs1053_handle_t *dev)` | Set bit `SM_RESET` trong `SCI_MODE` để reset mềm | Nội bộ, chỉ `vs1053_switch_to_mp3_mode` gọi | Có thân hàm, không ai dùng trực tiếp |
| `vs1053_wait_dreq` | `void (vs1053_handle_t *dev)` | Poll GPIO DREQ tới khi lên mức cao (chip sẵn sàng nhận lệnh/data) | Nội bộ (`write_sci`/`read_sci`/`send_buffer`/`init`) | Đang dùng nội bộ |
| `vs1053_write_sci` | `esp_err_t (vs1053_handle_t*, uint8_t reg, uint16_t value)` | Ghi 1 thanh ghi SCI qua SPI (lệnh `0x02`) | Đang dùng rộng rãi nội bộ + gián tiếp qua mp3.c | Đang dùng |
| `vs1053_read_sci` | `uint16_t (vs1053_handle_t*, uint8_t reg)` | Đọc 1 thanh ghi SCI qua SPI (lệnh `0x03`) | Đang dùng rộng rãi nội bộ | Đang dùng, thiếu check lỗi (mục 2.4) |
| `vs1053_send_buffer` | `esp_err_t (vs1053_handle_t*, const uint8_t*, size_t)` | Gửi tối đa 32 byte dữ liệu MP3 qua SDI (chờ DREQ trước) | `Mp3_StreamSong` (mp3.c), `vs1053_stop_song` | Đang dùng |
| `vs1053_set_volume` | `esp_err_t (vs1053_handle_t*, uint8_t volume)` | Set volume 0–100% (ghi `SCI_VOL`, kênh trái=phải) | `vs1053_init`, `vs1053_switch_to_mp3_mode` | Đang dùng, **không expose ra UI** (mục 2.5) |
| `vs1053_set_tone` | `esp_err_t (vs1053_handle_t*, uint16_t tone)` | *(theo tên: chỉnh tone/bass)* | **Không ai** | **Khai báo, KHÔNG có thân hàm** |
| `vs1053_switch_to_mp3_mode` | `esp_err_t (vs1053_handle_t *dev)` | Soft reset + set lại clock/mode/volume để quay lại chế độ MP3 | **Không ai** | Có thân hàm, dead code |
| `vs1053_start_song` | `void (vs1053_handle_t *dev)` | Reset `end_fill_byte` về 0 cho bài mới | `Mp3_Task` switch case NEXT/PREV/PLAY_NEW | Đang dùng, đơn giản hoá (mục 2.6) |
| `vs1053_stop_song` | `void (vs1053_handle_t *dev)` | Gửi 2052 byte filler để xả hết buffer nội bộ, rồi delay 100ms | `Mp3_StreamSong` khi hết bài (EOF) | Đang dùng, thiếu bước SM_CANCEL (mục 2.7) |
| `vs1053_cancel_song` | `void (vs1053_handle_t *dev)` | Set bit `SM_CANCEL` trong `SCI_MODE` để huỷ bài đang phát | **Không ai** | Có thân hàm, dead code |
| `vs1053_get_decoded_time` | `uint16_t (vs1053_handle_t *dev)` | Đọc `SCI_DECODE_TIME` (giây đã giải mã) | `Mp3_HandleCommand` (mp3.c, phục vụ `MP3_CMD_GET_DECODE_TIME`) | Đang dùng |
| `vs1053_clear_decoded_time` | `void (vs1053_handle_t *dev)` | Ghi 0 vào `SCI_DECODE_TIME` | **Không ai** | Có thân hàm, dead code |
| `vs1053_test_comm` | `Std_ReturnType (vs1053_handle_t *dev)` | Kiểm tra kết nối: check DREQ high, rồi ghi/đọc lại `SCI_VOL` với giá trị test `0x1234` | `vs1053_init` | Đang dùng |
| `vs1053_print_details` | `void (vs1053_handle_t *dev)` | In ra log các thanh ghi Mode/Status/ClockF/Volume/Audio/HDAT0/HDAT1 | **Không ai** | Có thân hàm, dead code (tiện ích debug) |

---

## 2. Đánh giá chi tiết

### 2.1. BUG: cấu hình "stereo" nhưng giá trị thực tế set chip sang mono

```c
// Cấu hình âm thanh: 44.1kHz stereo
vs1053_write_sci(dev, SCI_AUDATA, 44101);
```

Theo datasheet VS1053b, thanh ghi `SCI_AUDATA`: bit 15-1 là sample rate (Hz), **bit 0 (LSB) là cờ mono** — LSB = 1 nghĩa là ép chip sang mono, không liên quan gì tới sample rate. `44101` là **số lẻ** (`0xAC45`, LSB = 1) → chip nhận lệnh "mono" chứ không phải "44.1kHz". Muốn đúng ý comment (44.1kHz **stereo**) thì giá trị phải là **`44100`** (`0xAC44`, số chẵn, LSB = 0).

**Ảnh hưởng thực tế**: nếu file MP3 gốc là stereo, có thể bị VS1053 mix xuống mono khi phát (tuỳ chip có tôn trọng đúng field kênh trong header MP3 hay ép cứng theo AUDATA — cần nghe thử để xác nhận triệu chứng, nhưng giá trị ghi vào chắc chắn sai so với ý định trong comment).

**Đề xuất fix**: đổi `44101` → `44100`, hoặc rõ ràng hơn: `#define VS1053_SAMPLE_RATE_44100_STEREO 44100U` để không ai vô tình gõ nhầm số lẻ nữa.

### 2.2. Khai báo nhưng không có thân hàm: `vs1053_reset`, `vs1053_set_tone`

Cả 2 đều có trong `vs1053.h` nhưng **không tồn tại định nghĩa nào trong `vs1053.c`**. Hiện tại project build được vì không module nào gọi 2 hàm này — nhưng đây là "bom nổ chậm": ai đó thấy khai báo trong header, gọi thử, sẽ gặp lỗi linker (`undefined reference`) khó hiểu vì trông như hàm có tồn tại.

**Đề xuất**: hoặc implement (nếu vẫn cần reset cứng qua GPIO riêng biệt với phần reset đã nằm trong `vs1053_init`, và chỉnh bass/treble qua `SCI_BASS`), hoặc xoá khai báo khỏi header nếu không có kế hoạch dùng.

### 2.3. 4 hàm dead code: `vs1053_switch_to_mp3_mode`, `vs1053_cancel_song`, `vs1053_clear_decoded_time`, `vs1053_print_details`

Không sai, nhưng chưa từng chạy qua trong hệ thống thật (không có call site nào). Nghĩa là:
- Chưa được test bằng thực tế chạy trên phần cứng.
- `vs1053_switch_to_mp3_mode` gọi `vs1053_soft_reset` — nếu có bug ở soft reset (xem 2.8) thì cũng chưa lộ ra vì đường này chưa từng chạy.

Không cần xoá ngay (có thể là API dự phòng hợp lý), nhưng nên biết rằng "driver có nhiều hàm" không đồng nghĩa "nhiều hàm đã được kiểm chứng".

### 2.4. `vs1053_read_sci` không kiểm tra lỗi SPI

```c
spi_device_transmit(dev->spi_handle, &trans);   // return value bị bỏ qua
```

So sánh với `vs1053_write_sci` (có bắt `err` và trả về), `vs1053_read_sci` bỏ qua hoàn toàn kết quả `spi_device_transmit()`. Nếu SPI transaction lỗi, hàm vẫn trả về nội dung `rx_data` (có thể là rác hoặc toàn 0 vì được khởi tạo `{0}`) mà không có cách nào báo cho caller biết đó là dữ liệu không đáng tin. Vì kiểu trả về là `uint16_t` (không phải `esp_err_t`), không có chỗ để truyền lỗi ra ngoài với chữ ký hiện tại — muốn sửa đúng phải đổi sang out-parameter + `esp_err_t`, ảnh hưởng tới toàn bộ call site (đổi tương đối lớn, không phải chuyện nhỏ).

### 2.5. `vs1053_set_volume` tồn tại nhưng không có đường nào cho người dùng chỉnh volume

Chỉ được gọi nội bộ với giá trị cứng `50` lúc init. Không có button/menu nào trong `player_manager.c`/`menu.c` gọi lại hàm này. Đây là khoảng trống ở tầng ứng dụng (không phải lỗi của driver) — driver đã sẵn sàng, chỉ chưa được dùng.

### 2.6. `vs1053_start_song` hardcode `end_fill_byte = 0` thay vì đọc từ chip

```c
void vs1053_start_song(vs1053_handle_t *dev) {
    dev->end_fill_byte = 0;
}
```

Theo tài liệu/thư viện tham khảo phổ biến (Adafruit VS1053, VLSI app note), giá trị "end fill byte" đúng chuẩn nên **đọc từ RAM nội bộ chip** (qua `SCI_WRAMADDR` = `0x1E06` rồi đọc `SCI_WRAM`), vì giá trị im lặng đúng có thể khác 0 tuỳ codec/patch đang nạp. Hardcode 0 là cách đơn giản hoá phổ biến (thường vẫn nghe ổn với MP3 thường), nhưng lệch khỏi quy trình chuẩn — nếu sau này nghe thấy tiếng "click" nhỏ lúc dừng bài, đây là nghi phạm đầu tiên cần xem lại.

### 2.7. `vs1053_stop_song` không set `SM_CANCEL`, không có cơ chế fallback

Hàm chỉ xả buffer bằng cách gửi 2052 byte filler (đúng con số chuẩn theo tài liệu), nhưng không:
- Set bit `SM_CANCEL` (việc này để riêng cho `vs1053_cancel_song` — mà hàm đó lại **không ai gọi**, xem 2.3).
- Poll lại `SCI_MODE` để xác nhận `SM_CANCEL` đã tự clear (chip đã thực sự dừng decode).
- Fallback soft-reset nếu cancel không thành công sau vài frame (trường hợp hiếm nhưng tài liệu VLSI có cảnh báo).

Trong ngữ cảnh hiện tại (chỉ gọi khi hết bài tự nhiên - EOF, không phải "dừng đột ngột giữa chừng"), cách làm đơn giản này **đủ dùng**. Nhưng nếu sau này thêm tính năng "bấm Stop/skip giữa bài", nên cân nhắc bổ sung `SM_CANCEL` + poll, vì gửi filler không đảm bảo huỷ ngay 1 frame đang decode dở.

### 2.8. `vs1053_soft_reset` không khôi phục lại `SCI_CLOCKF` sau reset

```c
esp_err_t vs1053_soft_reset(vs1053_handle_t *dev) {
    uint16_t mode = vs1053_read_sci(dev, SCI_MODE);
    mode |= SM_RESET;
    return vs1053_write_sci(dev, SCI_MODE, mode);
}
```

Soft reset (`SM_RESET`) làm chip trả `SCI_CLOCKF` về giá trị mặc định (không nhân clock), trong khi SPI bus vẫn đang chạy ở tốc độ đã nâng lên (4MHz sau `vs1053_init`). Nếu gọi `vs1053_soft_reset` đứng một mình (không qua `vs1053_switch_to_mp3_mode`, vốn có tự ghi lại `SCI_CLOCKF` ngay sau), giao tiếp SPI kế tiếp có nguy cơ không ổn định vì tốc độ SPI vượt quá khả năng xử lý của clock nội bộ lúc đó. Hiện tại không phải vấn đề thực tế vì `vs1053_soft_reset` chỉ được gọi từ bên trong `vs1053_switch_to_mp3_mode` (đã tự fix), nhưng nếu expose `vs1053_soft_reset` ra dùng độc lập sau này thì đây là bẫy.

### 2.9. `vs1053_init` bỏ qua lỗi khi nâng tốc độ SPI

```c
spi_bus_remove_device(dev->spi_handle);
spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev->spi_handle);

ESP_LOGI(TAG, "VS1053 initialized successfully");
return ESP_OK;   // luôn trả OK dù 2 dòng trên có lỗi hay không
```

2 lời gọi thay đổi device SPI cuối `vs1053_init` không kiểm tra kết quả trả về — nếu `spi_bus_add_device` fail (hiếm, nhưng có thể xảy ra nếu hết slot device trên bus), `dev->spi_handle` sẽ ở trạng thái không xác định trong khi hàm vẫn báo `ESP_OK` thành công. Nên bắt lỗi 2 dòng này và trả `ESP_FAIL` nếu có lỗi, giống cách các bước trước đó trong hàm đã làm.

### 2.10. Pin SPI vật lý (MOSI/MISO/SCLK) hardcode trong `.c`, không đồng nhất với style 4 chân còn lại

```c
// vs1053.c, bên trong spi_init() - static, không expose
.mosi_io_num = GPIO_NUM_23,
.miso_io_num = GPIO_NUM_19,
.sclk_io_num = GPIO_NUM_18,
```

Trong khi `VS1053_CS_PIN`/`VS1053_DCS_PIN`/`VS1053_DREQ_PIN`/`VS1053_RESET_PIN` đều là `#define` công khai trong `vs1053.h`, dễ đổi khi đấu dây board khác — 3 chân SPI chuẩn (MOSI/MISO/SCLK) lại bị chôn cứng trong `.c`, phải sửa code mới đổi được. Không phải bug, nhưng thiếu nhất quán trong cách "tham số hoá phần cứng" của chính file này.

---

## 3. Xếp hạng ưu tiên nếu muốn dọn dẹp

| # | Việc | Mức độ |
|---|---|---|
| 1 | Sửa `44101` → `44100` (bug stereo/mono) | **Cao** — sai chức năng đang chạy |
| 2 | Implement hoặc xoá khai báo `vs1053_reset`/`vs1053_set_tone` | **Cao** — API giả, bẫy cho người dùng sau |
| 3 | Bắt lỗi `spi_bus_remove_device`/`spi_bus_add_device` trong `vs1053_init` | Trung bình |
| 4 | Đưa `vs1053_set_volume` lên tầng UI (nếu muốn có tính năng chỉnh volume) | Trung bình — tính năng, không phải bug |
| 5 | Đưa MOSI/MISO/SCLK ra `#define` trong header cho nhất quán | Thấp |
| 6 | `vs1053_read_sci` thêm kiểm tra lỗi SPI (đổi chữ ký) | Thấp — cần đổi API, rủi ro cao hơn lợi ích trước mắt |
| 7 | `vs1053_start_song` đọc `end_fill_byte` thật từ chip thay vì hardcode 0 | Thấp — chỉ có giá trị nếu nghe thấy click khi dừng |
| 8 | Bổ sung `SM_CANCEL` + poll cho `vs1053_stop_song` | Thấp — chỉ cần nếu thêm tính năng dừng giữa bài |
