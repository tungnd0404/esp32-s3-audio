<!-- markdownlint-disable -->
# Báo cáo Review Kiến trúc & Code — Audio Player ESP32-S3

**Phạm vi:** toàn bộ source code trong `e:\project\Audio` (không tính `build/`). Đã đọc trực tiếp: `main/Audio.c`, `main/CMakeLists.txt`, `config/config.h`, `driver/button/button.c/h`, `task/player_manager.c/h`, `task/oled.c/h`, `driver/ssd1306/menu.c/h`, `driver/buffer/sync_frame.c/h`, `driver/srm/srm.c/h`, `task/mp3.c/h`, `task/sdcard.c/h`, `driver/vs1053/vs1053.h` (+ các hàm gọi trong `.c`), `driver/buffer/double_buffer.c/h`, `driver/buffer/ring_buffer.c/h`, `driver/ssd1306/frame.c/h`, `driver/ssd1306/ssd1306.c/h`, `driver/ssd1306/ssd1306_i2c_new.c`, `driver/ssd1306/ssd1306_spi.c`, cùng `sdkconfig` (đối chiếu cấu hình Task Watchdog, main task affinity, BT enable).

Mọi khẳng định dưới đây đều được xác minh bằng cách đọc trực tiếp source/`sdkconfig`, không suy đoán. Trích dẫn theo `file:dòng`/tên hàm.

> **✅ Cập nhật:** Toàn bộ 9 bug trong Bug Report (mục 17) — C1, H1, H2, M1, M2, M3, M4, L1, L3 — đã được sửa trực tiếp trong code sau khi báo cáo này được viết. L2 tự động biến mất do file chứa nó (`ring_buffer.c`) đã bị xoá (H2). S1-S3 (Suggestion) chưa áp dụng vì đó là đề xuất tính năng/thiết kế, không phải bug. Nội dung bên dưới giữ nguyên như tại thời điểm review (mô tả hiện trạng **trước khi sửa**) để làm hồ sơ tham khảo — xem tóm tắt các thay đổi thực tế trong `README.md` mục "Hạn chế đã biết".

---

## 1. Tóm tắt kiến trúc

Hệ thống gồm **4 FreeRTOS task**, cùng priority 5, cùng pin vào **core 1** (`main/Audio.c:47-53`):

- `PlayerManager_Task` — bộ điều phối trung tâm, chủ sở hữu duy nhất của `gsPlayerContext` (state machine toàn hệ thống).
- `Oled_Task` — vẽ menu/animation lên OLED.
- `Sdcard_Task` — nạp dữ liệu animation (`frame.bin`) từ thẻ SD vào double buffer.
- `Mp3_Task` — owner phần cứng duy nhất của chip VS1053, stream audio.

Giao tiếp giữa 4 task **hoàn toàn bằng task notification** (không dùng queue cho luồng điều khiển chính), phát ra từ `PlayerManager_Task` mỗi khi `gsPlayerContext` thay đổi. Riêng truy cập VS1053 từ module không phải `Mp3_Task` (cụ thể là `sync_frame.c`) đi qua 1 queue lệnh (`xMp3CommandQueue`) theo pattern "Owner Task", hiện thực bởi thư viện dùng chung **SRM** (`driver/srm/`).

3 trong 4 task (`Oled_Task`, `Sdcard_Task`, `Mp3_Task`) được viết theo **cùng một khuôn thuật toán**: `Xxx_GetXState()` (map notification → state nội bộ) + `Xxx_Worker()` (vòng lặp làm việc, tự check non-blocking notify mỗi vòng) + cờ `lbHasPendingNotify` ở vòng lặp ngoài để không đánh mất notification vừa nhận. Đây là quyết định thiết kế nhất quán nhất của dự án — xem mục 20.

---

## 2. Dependency Map

| Module | Chức năng | Public API | Sở hữu | Người dùng | Thứ tự init |
|---|---|---|---|---|---|
| `driver/srm/srm.c/h` | Cơ chế Owner Task dùng chung (command/response queue theo từng task gọi) | `Srm_Init`, `Srm_SendCommand`, `Srm_Reply` | Không sở hữu tài nguyên vật lý nào, chỉ là hạ tầng | `sync_frame.c` (gọi), `mp3.c` (trả lời) | **1** — `Srm_Init()` phải chạy trước mọi task |
| `driver/button/button.c/h` | ISR GPIO 3 nút, debounce, gửi `Button_EventType_e` | `Button_Init`, 3 ISR | `gau32LastTickTime[]` | `PlayerManager_Task` (nhận notify) | 6 |
| `task/player_manager.c/h` | State machine trung tâm | `PlayerManager_Init`, `PlayerManager_Task` | `gsPlayerContext` (ghi độc quyền) | `oled.c`/`menu.c`/`sdcard.c`/`mp3.c`/`sync_frame.c` (đọc) | 7 (init), task tạo thứ 3 |
| `task/mp3.h/c` | Owner VS1053, stream mp3 | `Mp3_Init`, `Mp3_Task` | `gpMp3File`, `xMp3CommandQueue`, `vs1053_handle_t` cục bộ | `player_manager.c` (notify), `sync_frame.c` (SRM) | 8 (init), task tạo thứ 2 |
| `task/sdcard.c/h` | Quét & nạp DB bài hát, nạp double buffer | `Sdcard_Mount/ScanAndCreateDb/ReadDbFile/GetSongByIndex/Task` | `gaSongList`, `gu16SongCount`, file `songs.db` | `player_manager.c` (totalSong), `menu.c` (hiển thị), `mp3.c` (đọc songPath) | 2-4 (init hàm), task tạo thứ 1 |
| `task/oled.h/c` | Vẽ OLED (menu + animation) | `Oled_Init`, `Oled_Task` | không state riêng đáng kể | — | 5 (init), task tạo thứ 4 |
| `driver/ssd1306/menu.c/h` | Vẽ danh sách bài hát, cuộn | `Menu_Draw`, `Menu_UpdateScroll` | `gi32StartIndex` (static) | `oled.c` | — |
| `driver/buffer/sync_frame.c/h` | Tính frame animation theo decode time | `SyncFrame_Init`, `SyncFrame_GetFrameIndex` | state nội bộ (virtual time) | `oled.c` | — |
| `driver/buffer/double_buffer.c/h` | Cache 2 buffer luân phiên cho `frame.bin` | `double_buffer_init/open/close/total_frames/get_frame`, `sd_task_load_double_buffer` | `bufferA/B`, `double_buffer_event`, `double_buffer_mutex` | `sdcard.c` (nạp), `oled.c` (đọc qua `sync_frame`/trực tiếp) | — |
| `driver/ssd1306/ssd1306.c/h` + `_i2c_new.c`/`_spi.c` | Driver low-level SSD1306 | `ssd1306_init`, `ssd1306_display_*`, `i2c_*`/`spi_*` | `SSD1306_t::_page[8]` | `oled.c`, `menu.c` | — |
| `driver/vs1053/vs1053.c/h` | Driver chip giải mã VS1053 (SPI) | `vs1053_init/send_buffer/get_decoded_time/...` | SPI device handle | **chỉ** `mp3.c` (Owner Task) | — |
| `driver/buffer/ring_buffer.c/h` | **Không dùng, không build** | — | — | — | — |
| `driver/ssd1306/frame.c/h` | **File rỗng, không dùng** | — | — | — | — |

---

## 3. FreeRTOS Design Review

### 3.1 Bug nghiêm trọng nhất của toàn bộ báo cáo: busy-loop trong `app_main` starve Task Watchdog

`main/Audio.c:56`:
```c
while(1);
```
Đây là vòng lặp rỗng, **không có `vTaskDelay`/yield nào**, chạy vĩnh viễn sau khi task `main` đã tạo xong 4 task con.

Đối chiếu `sdkconfig`:
- `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y` (dòng 1202) → task `main` bị pin vào **CPU0**.
- `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y` (dòng 1222-1226) → Task Watchdog Timer theo dõi cả task Idle của CPU0, timeout 5 giây.
- `# CONFIG_ESP_TASK_WDT_PANIC is not set` → khi watchdog trigger, hệ thống **không reset**, chỉ in lỗi.

Task `main` trong ESP-IDF chạy ở priority cố định `ESP_TASK_MAIN_PRIO` (mặc định 1) — cao hơn task Idle (priority 0). Vì `while(1);` không bao giờ nhường CPU, task Idle0 **không bao giờ được cấp CPU trên core 0** để tự "feed" watchdog của chính nó. Hệ quả: cứ mỗi 5 giây, Task Watchdog Timer in ra lỗi kiểu `Task watchdog got triggered. The following tasks/users did not reset the watchdog in time: IDLE0` — lặp lại **vĩnh viễn trong suốt vòng đời thiết bị**, làm ngập log console và (tuỳ phiên bản IDF/cấu hình) có thể ảnh hưởng tới các cơ chế phụ thuộc vào Idle task (dọn rác task đã xoá, tick vào power-management nếu bật). Vì `PANIC` không bật nên không gây reset ngay lập tức, nhưng đây vẫn là hành vi sai nghiêm trọng, hoàn toàn không cố ý, và dễ bị hiểu nhầm là "lỗi phần cứng ngẫu nhiên" khi debug thực tế trên field vì log bị spam liên tục.

**Sửa:** thay `while(1);` bằng `vTaskDelay(portMAX_DELAY);` hoặc `vTaskDelete(NULL);` (an toàn vì task `main` không giữ tài nguyên nào cần dọn thêm).

### 3.2 Đồng bộ hoá qua task notification — đúng đắn nhưng có 1 giả định ngầm cần ghi nhận

Toàn bộ 4 task dùng notification 32-bit đơn (`eSetValueWithOverwrite`) — đúng bản chất FreeRTOS: **không xếp hàng đợi nhiều giá trị**, giá trị mới luôn ghi đè giá trị cũ chưa được đọc. Thiết kế hiện tại chấp nhận việc này có chủ đích (mỗi lần gửi đều đại diện cho "trạng thái mới nhất", không phải một sự kiện rời rạc cần xử lý tuần tự) — hợp lý cho use case này (menu di chuyển liên tục, chỉ cần vẽ đúng vị trí cuối cùng).

**Rủi ro thật đã được kiểm chứng qua code:** ở `PlayerManager_Task` (`task/player_manager.c:180-184`), khi đổi bài, 3 lệnh `xTaskNotify` liên tiếp gửi tới Oled/Sdcard/Mp3 **trong cùng 1 vòng lặp `switch`**, không có bảo vệ nào nếu 1 trong 3 task đích chưa xử lý xong notification trước đó khi notification mới tới — nhưng vì kênh là "ghi đè" (`eSetValueWithOverwrite`) và giá trị mới nhất luôn đúng ngữ nghĩa hơn giá trị cũ (đổi bài lần 2 luôn "thắng" đổi bài lần 1 nếu người dùng bấm Next liên tiếp rất nhanh), hành vi này **là đúng theo thiết kế**, không phải bug.

### 3.3 Truy cập `gsPlayerContext` không đồng bộ hoá tường minh (không mutex, không `volatile`)

`gsPlayerContext` (`task/player_manager.h:77`) được `PlayerManager_Task` ghi độc quyền, và được **đọc liên tục trong vòng lặp `while`** của 2 nơi khác, ngoài cơ chế notification:

- `Oled_PlayAnimation()` (`task/oled.c:128`): `while ((gsPlayerContext.mainState == MAIN_STATE_PLAYING) && (gsPlayerContext.playbackState != PLAYBACK_STATE_PAUSE))`
- `Mp3_StreamCurrentSong()` (`task/mp3.c:196`): điều kiện y hệt.

Đây là đọc **cross-task, ngoài mọi lock, không `volatile`**. Về lý thuyết C/trình biên dịch, compiler được phép cache giá trị field trong thanh ghi qua nhiều vòng lặp nếu nó chứng minh được không có gì trong thân vòng lặp có thể thay đổi giá trị đó — khiến vòng lặp không bao giờ thoát dù `PlayerManager_Task` đã đổi `playbackState`. Trên thực tế điều này **không xảy ra** ở build hiện tại vì mỗi vòng lặp đều gọi `xTaskNotifyWait()` (một hàm ngoài không được inline theo mặc định của ESP-IDF/GCC không LTO toàn cục) — lời gọi hàm ngoài buộc compiler phải coi như "có thể có side-effect không biết trước" và đọc lại giá trị từ RAM. Đây là một **bất biến mong manh** (fragile invariant): nếu tương lai bật LTO, hoặc ai đó tối ưu bằng cách bỏ bớt lệnh gọi hàm trong thân vòng lặp, bug "animation/audio không bao giờ dừng khi Pause" có thể xuất hiện im lặng mà không đổi gì ở logic nhìn thấy được. Vì `mainState`/`playbackState` là enum (word, aligned tự nhiên trên ESP32) nên không có nguy cơ **torn read** — chỉ có nguy cơ **stale read do compiler caching**, mức độ Medium.

**Khuyến nghị:** khai báo `volatile` cho tối thiểu `mainState` và `playbackState` trong `PlayerManager__PlayerContextType_s`, hoặc — tốt hơn — chỉ đọc các field này 1 lần đầu mỗi vòng lặp qua 1 local copy nếu chấp nhận độ trễ 1 vòng lặp (không cần thay đổi lớn).

### 3.4 Task priorities đồng nhất — không phân bậc real-time

Cả 4 task cùng priority 5. Không có tách biệt giữa task time-critical thực sự (audio, phải cấp DREQ VS1053 kịp thời để tránh underrun) và task ít quan trọng hơn (`Oled_Task` vẽ menu). Vì FreeRTOS scheduler round-robin giữa các task cùng priority trên cùng core (cả 4 đều pin core 1), một `Oled_Task` đang vẽ animation dày đặc (mỗi `OLED_ANIMATION_DELAY_MS` ≈ 33ms) về lý thuyết có thể trì hoãn `Mp3_Task` một khoảng thời gian bằng 1 time-slice trước khi tick scheduler chuyển lượt — trong thực tế `Mp3_Task` tự nhường CPU thường xuyên qua `vs1053_send_buffer()` (chờ DREQ bằng `vTaskDelay`), nên rủi ro underrun thấp, nhưng đây vẫn là 1 quyết định thiết kế đáng cân nhắc lại nếu sau này thêm nhiều task khác vào core 1 (ghi trong mục Scalability).

### 3.5 Queue/Semaphore trong `srm.c`/`double_buffer.c` — review chi tiết

- `srm.c`: mutex `gxMutexTaskResponseQueueList` được tạo đúng 1 lần trong `Srm_Init()`, **trước khi có task nào chạy** (`Srm_Init()` là lệnh đầu tiên trong `app_main()`) — đúng, tránh race "lazy-create mutex". Registry tối đa `SRM_MAX_REGISTERED_TASKS = 8` (`driver/srm/srm.c:16`) — hiện có 4 task, còn dư chỗ, nhưng **không có cơ chế nào cảnh báo/log khi registry đầy** (`Srm_GetOwnResponseQueue()` chỉ trả `NULL` âm thầm — `srm.c:91,108` — khiến `Srm_SendCommand()` trả `false` không rõ lý do là do registry đầy hay do timeout thật).
- `double_buffer.c`: `double_buffer_close()` **không NULL-check `double_buffer_mutex`** trước khi `xSemaphoreTake` (đối lập với `double_buffer_open()` có check ở dòng 190) — nếu gọi `double_buffer_close()` trước khi `double_buffer_init()` từng chạy thành công, gọi `xSemaphoreTake(NULL, ...)` là **undefined behavior**. Hiện tại `Sdcard_Task` luôn gọi `double_buffer_init()` (`task/sdcard.c:439`) trước khi có bất kỳ notify nào có thể kích hoạt `Sdcard_LoadCurrentSong()` → `double_buffer_close()` (`task/sdcard.c:120`), nên **không bị trigger trong luồng chạy hiện tại**, nhưng là code dễ vỡ nếu thứ tự gọi thay đổi trong tương lai.

### 3.6 ISR review (`driver/button/button.c`)

3 ISR (`Button_NextISR/PrevISR/PlayISR`) đều: đọc `xTaskGetTickCountFromISR()`, so sánh debounce, gọi `xTaskNotifyFromISR(..., &xHigherPriorityTaskWoken)`, rồi `portYIELD_FROM_ISR()` nếu cần — đúng chuẩn ISR-safe FreeRTOS. Không có tính toán nặng, không gọi API blocking trong ISR. `IRAM_ATTR` được đặt đúng trên cả 3 hàm. Không phát hiện lỗi.

Một điểm nhỏ: `gpio_install_isr_service(0)` (`button.c:133`) dùng flags=0 (mặc định), nghĩa là ISR service chạy trên core nào gọi `Button_Init()` — tức core 0 (vì `Button_Init()` được gọi từ `app_main`, pin core 0). Không sai, chỉ là một chi tiết nên biết khi debug timing.

---

## 4. Memory Review

- **Không có `malloc`/`free` trong pipeline audio/animation** — toàn bộ buffer lớn (double buffer 30KB, framebuffer OLED 1KB, DB bài hát ~13.6KB) đều static/global, loại bỏ hoàn toàn rủi ro fragmentation/leak ở đường dẫn hot-path. Đây là một điểm mạnh về độ tin cậy lâu dài (long-running stability).
- **`ssd1306_i2c_new.c:i2c_display_image()` gọi `malloc`/`free` mỗi lần ghi 1 page** (dòng ~173-198, theo khảo sát trước đó) — với `Oled_DrawFrame()` gọi hàm này 8 lần/frame (`OLED_PAGE_COUNT = 8`, `task/oled.c:100-103`) và animation chạy ở `FRAME_PER_SECOND = 15` fps khi frame thực sự đổi, đây là **8 × 15 = tối đa 120 lần malloc/free mỗi giây** trong suốt thời gian phát nhạc. Mỗi lần được `NULL`-check và `free` đúng cách (không leak), nhưng đây là nguồn heap-fragmentation tiềm ẩn trên một hệ heap chung với các module khác (FAT filesystem, SPI driver...) khi chạy liên tục hàng giờ — nên thay bằng buffer tĩnh cố định (`width+1` byte tối đa 129 byte).
- `ring_buffer.c` (không build) dùng `xRingbufferCreate()` — cấp phát động, nhưng vì không nằm trong `SRCS` nên không ảnh hưởng runtime thực tế.
- Không có buffer nào truy cập DMA/PSRAM trong toàn bộ code hiện tại (SPI driver VS1053/SSD1306 dùng API chuẩn `spi_device_transmit`, không tự quản lý DMA descriptor).
- Không phát hiện double-free, dangling pointer, hay thiếu NULL-check gây crash trực tiếp ở các đường dẫn chính (đã audit `Mp3_StreamCurrentSong`, `Sdcard_LoadCurrentSong`, `double_buffer_get_frame`).

---

## 5. Public API Review

Nhìn chung API các module tuân thủ khá tốt: validate tham số đầu vào cơ bản (`Sdcard_GetSongByIndex` check `index >= gu16SongCount`/`pOut == NULL`; `Srm_Reply` check `pRequest == NULL`/`responseQueue == NULL`; `Srm_SendCommand` check `ownerQueue == NULL`, `cmdId >= SRM_CMD_INVALID`). Naming nhất quán (`Module_PascalCaseFunction`), return-value convention rõ ràng (`bool`/`0 hoặc -1`/`esp_err_t` tuỳ ngữ cảnh, dùng đúng chỗ).

**Điểm yếu chung của toàn bộ tầng `ssd1306.c`/`_i2c_new.c`/`_spi.c`** (driver vendor gốc, không phải code tự viết trong dự án): **không một hàm nào NULL-check tham số `SSD1306_t *dev`** trước khi dereference. Vì trong kiến trúc hiện tại chỉ có duy nhất `Oled_Task` cầm `dev` hợp lệ (khởi tạo 1 lần ở `main/Audio.c:14`, truyền vào task), rủi ro thực tế thấp, nhưng nếu API này được expose cho module khác trong tương lai (vd 1 task debug/service khác cũng muốn vẽ lên OLED) thì đây là nơi dễ crash nhất.

`ring_buffer.h` có 2 hàm định nghĩa trong `.c` nhưng không khai báo trong `.h` (`ring_buffer_open`, `sd_task_load_ring_buffer`) — vi phạm nguyên tắc header/impl khớp nhau, dù không ảnh hưởng vì cả file không được build.

---

## 6. Audio Pipeline Review

Luồng: `Sdcard_GetSongByIndex` → `fopen` → vòng lặp `fread(32 byte)` → `vs1053_send_buffer()` (tự chờ DREQ bên trong bằng `vTaskDelay`) → lặp tới EOF (`task/mp3.c:152-227`).

- **Đúng đắn cơ bản đã kiểm chứng**: Play/Pause giữ nguyên `gpMp3File` mở, không đóng — Resume tiếp tục đúng vị trí (`mp3.c:162-166` chỉ đóng khi `bIsNewSong == true`). Có phòng thủ `gpMp3File == NULL` trước khi `fread` (`mp3.c:188-191`) chống trường hợp lý thuyết nhận `PLAY_PAUSE` khi chưa từng mở bài.
- **Không có auto-next khi hết bài**: `fread() == 0` → `fclose` + `vs1053_stop_song()` + `break` (`mp3.c:213-220`), task quay về chờ notify mới, **không tự chuyển bài kế tiếp**. Đây là thiếu sót tính năng thực sự với 1 sản phẩm máy nghe nhạc thương mại (người dùng kỳ vọng playlist tự next).
- **Không seek**: không có API/luồng nào set lại vị trí phát giữa bài — chỉ có phát từ đầu (bài mới) hoặc tiếp tục đúng chỗ (resume).
- **Không có xử lý lỗi đọc file giữa chừng khác 0**: `fread()` trả về giá trị `0 < n < VS1053_CHUNK_SIZE` (đọc dở, sát cuối file) vẫn được gửi đi bình thường qua `vs1053_send_buffer(pDev, lau8Chunk, lChunkLen)` — đúng, vì `lChunkLen` truyền theo giá trị thực đọc được, không phải hằng số.
- **State-machine transition đổi bài/play/pause đã review qua `player_manager.c`**: không phát hiện lỗi transition sai (đã trace toàn bộ 3 case `BTN_EVENT_NEXT/PREV/PLAY`).

---

## 7. Animation Pipeline Review

`sync_frame.c` tính `SyncFrame_GetFrameIndex()` bằng cách nội suy thời gian ảo (`gf32VirtualTime`) dựa trên thanh ghi `SCI_DECODE_TIME` của VS1053 (16-bit, giây, có rollover), đọc mỗi 100ms qua SRM.

- **Xử lý rollover đúng**: `SyncFrame_UpdateVirtualTime()` (`sync_frame.c:71-110`) tính delta có so sánh `lu16RawDecodeTime >= gu16DecodePrev`, nếu không thì cộng bù `65535 - gu16DecodePrev + lu16RawDecodeTime + 1` — công thức đúng cho rollover 16-bit.
- **Giới hạn delta ≤ 1 giây/tick** (`sync_frame.c:95-98`) để chống nhiễu/giá trị đọc bất thường — hợp lý vì thanh ghi tăng đúng 1 mỗi giây theo datasheet VS1053.
- **`while` xử lý các mốc bị bỏ lỡ** (`sync_frame.c:76-103`, "chống trượt") — đúng đắn: nếu `Oled_Task` bận (vd đang trong `vTaskDelay` dài), khi quay lại vòng lặp có thể đã trôi qua nhiều hơn 1 chu kỳ `SYNC_FRAME_TICK_MS`, code xử lý bằng vòng lặp thay vì chỉ 1 lần — không bị "đọng" delta.
- **`double_buffer_get_frame()` timeout tối đa 5 giây** (theo khảo sát `double_buffer.c`) khi buffer miss — nếu SD card chậm bất thường, `Oled_PlayAnimation()` có thể bị block dài, làm animation đứng hình, nhưng không crash/deadlock.
- **Chỉ vẽ lại khi `frameIndex` đổi** (`task/oled.c:142-147`) — tối ưu hợp lý, tránh ghi I2C thừa.

---

## 8. Display Pipeline Review

- `ssd1306.c` giữ framebuffer `_page[8]` trong `SSD1306_t`, không tự khoá — an toàn trong kiến trúc hiện tại vì chỉ `Oled_Task` đụng vào `dev`.
- `Oled_Init()` (`task/oled.c:167-171`) chỉ gọi `i2c_master_init()` + `ssd1306_init()` — đường dẫn SPI (`ssd1306_spi.c`) tồn tại trong build nhưng **không được kích hoạt bởi bất kỳ code nào trong dự án hiện tại** (không ai gọi `spi_master_init()`). Đây là code "có sẵn nhưng chết" theo nghĩa runtime, dù vẫn được biên dịch (khác với `ring_buffer.c`/`frame.c` không build luôn).
- Không có double buffering ở tầng hiển thị (framebuffer bị ghi đè trực tiếp) — với animation tốc độ thấp (15fps) và không có hiện tượng tearing được báo cáo, đây là chấp nhận được cho use case hiện tại.
- Truncation im lặng ở các hàm text (`ssd1306_display_text_x3` cắt còn 5 ký tự, `ssd1306_display_rotate_text` cắt còn 8 ký tự) không được project code (`menu.c`) sử dụng — `Menu_Draw()` tự giới hạn `.27s` bằng `snprintf` trước khi gọi `ssd1306_display_text`, nên không bị ảnh hưởng.

---

## 9. Bluetooth Pipeline Review

**N/A — đã xác nhận không tồn tại.** Grep toàn bộ source (loại trừ `build/`) cho `esp_bt`/A2DP/BLE: không có kết quả thật (chỉ match giả do search substring trên các từ như "table", "double", "enable"). Kiểm tra `sdkconfig`: không có dòng `CONFIG_BT_ENABLED` nào được set. Kết luận: **không có bất kỳ hạ tầng Bluetooth nào** trong firmware hiện tại, kể cả ở dạng khung sườn chưa hoàn thiện.

---

## 10. Error Handling Review

- `Sdcard_Mount()` dùng `esp_vfs_fat_sdmmc_mount()` trả `esp_err_t`, có check và `ESP_LOGE` khi lỗi (`sdcard.c:225-229`) — đúng chuẩn, nhưng **không có xử lý gì tiếp theo** nếu mount thất bại: `app_main()` (`main/Audio.c:23`) gọi `Sdcard_Mount()` không kiểm tra giá trị trả về, vẫn tiếp tục `Sdcard_ScanAndCreateDb()` ngay sau đó — hàm này sẽ `opendir("/sdcard")` thất bại (in "Cannot open dir", return sớm — `sdcard.c:253-257`, không crash), nhưng hệ thống tiếp tục boot với `gu16SongCount = 0` một cách im lặng, không có cách nào báo cho người dùng biết "thẻ SD lỗi" ngoài dòng log console. Với 1 sản phẩm thương mại, đây là thiếu sót UX: nên hiển thị lỗi lên OLED thay vì chỉ log UART.
- `vs1053_init()` được check return value đúng cách trong `Mp3_Task` (`mp3.c:314-323`) — nếu lỗi, task vào vòng lặp `vTaskDelay` vô hạn thay vì tiếp tục dùng device có thể chưa hợp lệ — xử lý đúng, an toàn.
- `ssd1306_spi.c` dùng `assert()` cho lỗi khởi tạo SPI bus (không phải `ESP_ERROR_CHECK`) — hành vi khi assert fail phụ thuộc cấu hình build (có thể abort hệ thống hoặc bị tối ưu bỏ nếu `NDEBUG`), kém nhất quán so với phần còn lại của code dùng `ESP_ERROR_CHECK`/`ESP_LOGE` — nhưng đường dẫn này (SPI) hiện không được gọi trong runtime thực tế (xem mục 8) nên rủi ro thực tế bằng 0 cho tới khi ai đó bật lại backend SPI.
- `Srm_SendCommand`/`Srm_Reply` trả `bool` nhất quán cho mọi lỗi (queue đầy, timeout, invalid cmd) — bên gọi (`sync_frame.c:54-59`) luôn có fallback dùng giá trị cache cũ, không có code nào bỏ qua giá trị trả về của `Srm_SendCommand`.
- Không dùng `ESP_ERROR_CHECK` ở bất kỳ đâu có thể "chết cứng" hệ thống ngoài `Button_Init()` (`gpio_config`/`gpio_install_isr_service`, `button.c:132-133`) — hợp lý, vì lỗi cấu hình GPIO ở đây là lỗi cấu hình sai (không thể phục hồi runtime), nên fail-fast là đúng.

---

## 11. Performance Review

- **`i2c_display_image()` malloc/free mỗi page** — đã nêu ở mục 4, tối đa 120 lần/giây trong lúc phát nhạc. Nên thay bằng buffer tĩnh 129 byte.
- **`VS1053_CHUNK_SIZE = 32` byte/lần gửi** (`vs1053.h:41`) — kích thước nhỏ khớp với FIFO nội bộ 32-byte của VS1053 theo datasheet gốc, không phải lựa chọn tuỳ tiện — không coi là vấn đề hiệu năng cần sửa, chỉ là đặc tính phần cứng.
- **`Sdcard_GetSongByIndex()` mở lại file `songs.db` bằng `fopen` mỗi lần gọi** (`sdcard.c:370`) thay vì giữ file handle mở — chỉ được gọi khi đổi bài (không phải trong vòng lặp nóng), nên chi phí này chấp nhận được, không phải hot-path.
- **`OLED_ANIMATION_DELAY_MS = frame_interval / 2`** (`task/oled.c:24`) khiến `Oled_PlayAnimation()` poll gấp đôi tốc độ frame thật — có chủ đích để bắt kịp thời điểm đổi frame sớm hơn, được lọc bằng so sánh `lu32FrameIndex != lu32LastDrawnFrameIndex` để không vẽ lại thừa — đánh đổi hợp lý giữa độ trễ phản ứng và tải CPU.
- Không phát hiện thuật toán bậc cao bất thường (không có vòng lặp lồng nhau O(n²) trên dữ liệu lớn) ở bất kỳ đâu trong code review được.

---

## 12. Architecture Review

Điểm mạnh: tách biệt rõ ràng Owner Task (VS1053) khỏi phần còn lại qua SRM, state machine trung tâm 1 nguồn sự thật (`gsPlayerContext`), thuật toán task thống nhất giữa 3/4 task.

Điểm yếu:
- **God-struct nhẹ**: `gsPlayerContext` gộp cả UI state (`mainState`, `cursor`) và playback state (`playbackState`, `currentSong`) trong 1 struct — chấp nhận được ở quy mô hiện tại (5 field), nhưng nếu mở rộng thêm nhiều tính năng (playlist, shuffle, repeat, volume...) sẽ cần tách thành nhiều struct nhỏ hơn theo domain để tránh mọi module phải include toàn bộ context chỉ để đọc 1 field.
- **SRM là hạ tầng tốt nhưng mới chỉ có 1 command** (`MP3_CMD_GET_DECODE_TIME`) — chưa được kiểm chứng ở quy mô nhiều lệnh/nhiều owner khác nhau; thiết kế registry tĩnh 8 slot là đủ cho hiện tại nhưng cứng (không có cách phát hiện/log khi hết chỗ, xem mục 3.5).
- **Không có interface/abstraction cho "nguồn animation"**: `double_buffer.c` gắn chặt với khái niệm file `frame.bin` cụ thể (`FRAME_SIZE = 1024`, `CACHE_FRAMES = 15` cố định bằng macro, không cấu hình runtime) — nếu sau này muốn hỗ trợ nhiều độ phân giải/định dạng animation khác nhau, cần refactor.
- Không phát hiện dependency vòng (cyclic include) giữa các module đã review.

---

## 13. Coding Standard Review

- Naming nhất quán cao trong toàn bộ code tự viết (không tính driver vendor `ssd1306.c`/`vs1053.c`): `Module_PascalCaseFunction`, biến local tiền tố Hungarian (`lu32`, `li32`, `lb`, `le`, `la`/`lau8`, `lp`), biến global (`g`, `ga`, `gs`, `gx`, `gu16`...). Section banner `/* === ... === */` được áp dụng đồng đều ở mọi file mới (`button.c`, `player_manager.c`, `oled.c`, `sdcard.c`, `mp3.c`, `srm.c`, `sync_frame.c`).
- Doxygen-style comment (`@brief/@param/@return`) lặp lại đồng nhất giữa `.h` và `.c` — tốt cho tra cứu nhanh.
- `config/config.h:8` — `#define DEVELOPER_CONFIGURATION` **luôn được bật vô điều kiện**, không có cấu hình build release nào tắt nó đi. Mọi `printf` debug rải trong `player_manager.c`/`sdcard.c` sẽ **luôn** được biên dịch và chạy trên field, kể cả bản đóng gói cho khách hàng — không đúng với ý định ban đầu của macro này (phân biệt dev/release).
- `BTN1_BIT`/`BTN2_BIT`/`BTN3_BIT` (`task/player_manager.h:19-21`) khai báo nhưng **không được dùng ở bất kỳ đâu** trong toàn bộ codebase (đã grep xác nhận) — tàn dư từ 1 thiết kế cũ (có thể từng dùng event group thay vì task notification), nên xoá.
- `driver/ssd1306/ssd1306_spi.c` (code vendor, không phải coding convention của dự án): global `int clock_speed_hz` không có `static` (namespace leak tiềm ẩn), macro `SPI_DEFAULT_FREQUENCY` có `;` thừa trong định nghĩa — sloppy nhưng vô hại trong build hiện tại.
- Không phát hiện vi phạm MISRA nghiêm trọng (không có phép gán trong điều kiện `if`, không có fallthrough `switch` không chủ đích, magic number được đặt tên bằng macro ở hầu hết chỗ quan trọng).

---

## 14. Hidden-Bug Hunting

Các kịch bản đã trace thủ công qua code (không phải phỏng đoán):

- **Rapid Play/Stop liên tục**: `Mp3_StreamCurrentSong(bIsNewSong=false, ...)` khi `gpMp3File == NULL` được chặn đúng ở dòng 188-191 → không crash. Đã kiểm chứng bằng đọc code, không cần chạy thử.
- **Chuyển bài liên tục rất nhanh (spam Next)**: mỗi lần đều `xTaskNotify` ghi đè giá trị cũ (`eSetValueWithOverwrite`) tới cả 3 task — `Sdcard_Task`/`Mp3_Task` chỉ xử lý giá trị **cuối cùng** nhận được nhờ cơ chế `lbHasPendingNotify`/non-blocking check trong vòng lặp worker — không bị "dồn" xử lý từng bài trung gian, đúng ý định thiết kế (chỉ cần load đúng bài cuối cùng người dùng dừng lại).
- **Bộ nhớ dưới áp lực (low heap)**: vì hot-path không dùng heap (trừ `i2c_display_image` malloc nhỏ, luôn free đúng), rủi ro cạn heap trong lúc phát nhạc thấp — rủi ro chính nằm ở việc mở nhiều file FAT đồng thời (`gpMp3File` + `frame_file` trong `double_buffer.c` + `songs.db` mở/đóng liên tục) so với `max_files = 5` cấu hình trong `esp_vfs_fat_sdmmc_mount_config_t` (`sdcard.c:220`) — hiện tại tối đa mở đồng thời 2-3 file, còn dư, không phải vấn đề với cấu hình hiện tại.
- **Queue full (`xMp3CommandQueue` đầy)**: `Srm_SendCommand()` dùng `xQueueSend(ownerQueue, &lRequest, 0)` (timeout=0, `srm.c:187`) — nếu đầy, trả `false` ngay, không block `sync_frame.c` — đúng, an toàn.
- **Mất notification (lost notification)**: đã phân tích ở mục 3.2 — về bản chất kênh 32-bit overwrite **luôn có khả năng "mất" giá trị trung gian**, nhưng đây là hành vi được chấp nhận có chủ đích trong thiết kế, không phải bug ẩn.
- **OLED refresh trong lúc audio callback**: không có "audio callback" nào theo nghĩa ISR trong kiến trúc này (VS1053 không dùng interrupt cho luồng dữ liệu, chỉ poll DREQ) — không áp dụng.
- **Reset watchdog do busy-loop `main`**: xem mục 3.1 — bug thật, đã xác minh qua `sdkconfig`.

---

## 15. Concurrency Review

Tổng hợp các điểm đã nêu rải rác ở trên:

| Biến chia sẻ | Ghi bởi | Đọc bởi | Bảo vệ | Đánh giá |
|---|---|---|---|---|
| `gsPlayerContext.*` | `PlayerManager_Task` | `Oled_Task`, `Sdcard_Task`, `Mp3_Task`, `menu.c` | Không mutex, không `volatile`; đồng bộ ngầm qua "notify sau khi ghi xong" | An toàn với build hiện tại (đọc/ghi word-aligned atomic, notify tạo compiler barrier gián tiếp qua lời gọi hàm), nhưng mong manh — xem mục 3.3 |
| `bufferA`/`bufferB`, `startA/B`, `countA/B`, `readyA/B` (`double_buffer.c`) | `Sdcard_Task` (qua `sd_task_load_double_buffer`) | `Oled_Task` (qua `double_buffer_get_frame`) | `double_buffer_mutex` | Đúng đắn, có mutex bảo vệ toàn bộ đường đọc/ghi |
| `gaTaskResponseQueueList[]`/`gu32RegisteredCount` (`srm.c`) | Bất kỳ task nào gọi `Srm_SendCommand` lần đầu | như trên | `gxMutexTaskResponseQueueList` | Đúng đắn, đã có mutex sau khi user tự phát hiện race và yêu cầu sửa |
| `gpMp3File` (`mp3.c`) | Chỉ `Mp3_Task` | Chỉ `Mp3_Task` | Không cần (single-owner, không chia sẻ) | An toàn |
| `gu16DecodePrev`/`gu16DecodeTotal`/`gf32VirtualTime` (`sync_frame.c`) | Chỉ chạy trong ngữ cảnh gọi từ `Oled_Task` (`SyncFrame_Init`/`GetFrameIndex` đều được `oled.c` gọi) | như trên | Không cần (single-task access) | An toàn — `gf32VirtualTime` khai báo `volatile` dù không thực sự cần thiết (không có ISR/task khác đọc), vô hại |
| `gau32LastTickTime[]` (`button.c`) | 3 ISR khác nhau, mỗi ISR chỉ ghi vào ô riêng của mình (`BTN_EVENT_NEXT/PREV/PLAY` là 3 index riêng biệt) | như trên | Không cần (không có ô nào bị 2 ISR cùng ghi) | An toàn, khai báo `volatile` đúng |

Không phát hiện race điều kiện gây crash thực sự trong bất kỳ luồng nào đã trace. Vấn đề nghiêm trọng nhất về concurrency là mục 3.1 (watchdog) và 3.3 (fragile invariant), cả hai đều KHÔNG phải data race theo nghĩa cổ điển.

---

## 16. Đề xuất đơn giản hoá code

- Xoá `BTN1_BIT`/`BTN2_BIT`/`BTN3_BIT` (`task/player_manager.h:19-21`) — không dùng.
- Xoá hẳn `driver/buffer/ring_buffer.c/h` và `driver/ssd1306/frame.c/h` khỏi repo (không phải chỉ khỏi build) — code chết, `ring_buffer.c` còn không biên dịch được, dễ gây nhầm lẫn cho dev mới tưởng đây là đường dẫn đang dùng.
- Thay `malloc`/`free` trong `i2c_display_image()` bằng buffer tĩnh 129 byte (loại bỏ hoàn toàn heap traffic khỏi hot-path hiển thị).
- Gộp macro cấu hình `DEVELOPER_CONFIGURATION` thành 1 cờ build thật (qua `idf.py menuconfig`/CMake option) thay vì hard-code `#define` trong `config.h`, để có thể build bản release tắt log debug.

---

## 17. Bug Report (xếp hạng theo mức độ nghiêm trọng)

### 🔴 Critical

**C1 — ✅ ĐÃ FIX — `main/Audio.c:56` `while(1);` không yield → starve Task Watchdog trên IDLE0 vĩnh viễn**
- **Vị trí**: `main/Audio.c`, dòng 56, hàm `app_main()`.
- **Lý do**: Task `main` (pin CPU0 theo `sdkconfig: CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y`) chạy priority 1, cao hơn Idle (priority 0), busy-loop vô hạn không nhường CPU. `sdkconfig` có `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`, timeout 5s.
- **Rủi ro**: Task Watchdog liên tục báo lỗi "IDLE0 not fed" mỗi 5 giây suốt vòng đời thiết bị, spam log; `CONFIG_ESP_TASK_WDT_PANIC` hiện tắt nên chưa gây reset ngay, nhưng đây là hành vi runtime sai hoàn toàn không chủ đích, cực kỳ dễ bị hiểu lầm là lỗi phần cứng khi debug field, và là rủi ro treo/reset thật nếu ai đó bật `PANIC` lên (thường được bật ở bản production để tự phục hồi khi treo thật).
- **Đề xuất fix**: `while(1);` → `vTaskDelay(portMAX_DELAY);` (hoặc `vTaskDelete(NULL);`).

### 🟠 High

**H1 — ✅ ĐÃ FIX — `double_buffer_close()`/`double_buffer_total_frames()` không NULL-check `double_buffer_mutex`**
- **Vị trí**: `driver/buffer/double_buffer.c`, hàm `double_buffer_close()` (~dòng 246) và `double_buffer_total_frames()` (~dòng 272).
- **Lý do**: Không đối xứng với `double_buffer_open()` (có check ở dòng 190). Nếu gọi trước `double_buffer_init()` thành công, `xSemaphoreTake(NULL, ...)` là undefined behavior.
- **Rủi ro**: Hiện tại không bị kích hoạt vì `Sdcard_Task` luôn init trước, nhưng là quả bom hẹn giờ nếu thứ tự gọi thay đổi (vd thêm 1 task khác cũng dùng double buffer, hoặc thêm đường dẫn lỗi gọi `close()` sớm).
- **Đề xuất fix**: thêm `if (double_buffer_mutex == NULL) { return; }` đầu 2 hàm, giống `double_buffer_open()`.

**H2 — ✅ ĐÃ FIX (đã xoá file) — `ring_buffer.c` chứa code không biên dịch được (dead code nhưng nguy hiểm nếu bật lại)**
- **Vị trí**: `driver/buffer/ring_buffer.c`, các hàm `ring_buffer_open()`/`sd_task_load_ring_buffer()` tham chiếu `mp3_rb` và `ring_write()` chưa từng được khai báo ở bất kỳ đâu.
- **Lý do**: Đây là tàn dư 1 thiết kế cũ bị bỏ dở giữa chừng.
- **Rủi ro**: Nếu 1 dev sau này vô tình thêm file này vào `SRCS` (vd copy nhầm từ 1 dự án khác có cùng tên file), build sẽ fail ngay với lỗi "undeclared identifier" — không nguy hiểm runtime nhưng gây mất thời gian debug không cần thiết.
- **Đề xuất fix**: xoá hẳn 2 file khỏi repo.

### 🟡 Medium

**M1 — ✅ ĐÃ FIX — Đọc `gsPlayerContext.mainState`/`playbackState` cross-task không `volatile`, dựa vào bất biến ngầm "luôn có lời gọi hàm trong thân vòng lặp"**
- **Vị trí**: `task/oled.c:128` (`Oled_PlayAnimation`), `task/mp3.c:196` (`Mp3_StreamCurrentSong`).
- **Rủi ro**: Nếu tối ưu hoá build thay đổi (bật LTO/inline hàm gọi trong vòng lặp), compiler có thể cache giá trị và vòng lặp không bao giờ thoát khi Pause được kích hoạt từ task khác — animation/audio "không chịu dừng" khi bấm Pause.
- **Đề xuất fix**: thêm `volatile` cho 2 field này trong `PlayerManager__PlayerContextType_s`.

**M2 — ✅ ĐÃ FIX — `Srm_GetOwnResponseQueue()` hết chỗ đăng ký (>8 task) thất bại âm thầm**
- **Vị trí**: `driver/srm/srm.c:91-104`.
- **Rủi ro**: Không log, không phân biệt được lý do thất bại (registry đầy vs timeout thật) khi debug ở tương lai lúc hệ thống có nhiều task hơn 8.
- **Đề xuất fix**: thêm `ESP_LOGE` khi `gu32RegisteredCount >= SRM_MAX_REGISTERED_TASKS`.

**M3 — ✅ ĐÃ FIX — `i2c_display_image()` malloc/free mỗi page (tối đa 120 lần/giây khi phát nhạc)**
- **Vị trí**: `driver/ssd1306/ssd1306_i2c_new.c`, hàm `i2c_display_image()`.
- **Rủi ro**: Heap fragmentation tích luỹ khi chạy liên tục nhiều giờ.
- **Đề xuất fix**: thay bằng buffer tĩnh cố định 129 byte (`width` tối đa 128).

**M4 — ✅ ĐÃ FIX — `Sdcard_Mount()` thất bại không được xử lý ở `app_main()`**
- **Vị trí**: `main/Audio.c:23` — không check giá trị trả về của `Sdcard_Mount()`.
- **Rủi ro**: Thẻ SD lỗi/không cắm → hệ thống boot "thành công" với danh sách bài hát rỗng, không có thông báo nào cho người dùng ngoài UART log.
- **Đề xuất fix**: hiển thị lỗi lên OLED nếu `Sdcard_Mount()` hoặc `gu16SongCount == 0` sau khi scan.

### 🟢 Low

**L1 — ✅ ĐÃ FIX** — `load_double_buffer_internal()` ghi `*start` trước khi biết `fread` có thành công không (`double_buffer.c` ~dòng 64-82) — có thể để lại cặp `(start, count)` không nhất quán sau 1 lần load lỗi, ảnh hưởng phép tính preload kế tiếp. Chưa quan sát được kịch bản trigger cụ thể trong luồng chạy hiện tại (SD card lỗi đọc giữa chừng là hiếm), nhưng đáng sửa cho chắc chắn.

**L2 — ✅ ĐÃ FIX (giải quyết gián tiếp qua H2 — đã xoá file)** — `ring_buffer_write_isr()` (dead code) không gọi `portYIELD_FROM_ISR` dù có tính `xHigherPriorityTaskWoken` — vô hại vì không được gọi ở đâu, chỉ đáng sửa nếu code này được hồi sinh.

**L3 — ✅ ĐÃ FIX** — Biến toàn cục `clock_speed_hz` trong `ssd1306_spi.c` thiếu `static` — namespace leak tiềm ẩn, không có xung đột thực tế hiện tại.

### 🔵 Suggestion

**S1** — Gộp `DEVELOPER_CONFIGURATION` thành build option thật thay vì `#define` cứng, để có bản release tắt log debug.

**S2** — Xoá `BTN1_BIT`/`BTN2_BIT`/`BTN3_BIT` không dùng trong `player_manager.h`.

**S3** — Thêm auto-next khi hết bài trong `Mp3_StreamCurrentSong()` (tính năng UX cơ bản còn thiếu, không phải bug).

---

## 18. Technical Debt Report

- **Nợ kiến trúc**: `gsPlayerContext` là 1 struct phẳng gộp nhiều domain (UI + playback) — chấp nhận được ở quy mô hiện tại, sẽ cần tách khi thêm tính năng (playlist/repeat/shuffle/volume) để tránh mọi module phải phụ thuộc toàn bộ struct.
- **Nợ kiểm thử**: không có bất kỳ unit test/integration test nào trong repo (không tìm thấy thư mục `test/`). Với 1 state machine phức tạp như `PlayerManager_Task` (nhiều nhánh double-click/auto-return), thiếu test khiến mọi thay đổi tương lai rủi ro cao về regression, chỉ có thể kiểm chứng bằng test tay trên phần cứng thật.
- **Nợ bảo trì**: file rỗng (`frame.c/h`) và code chết không biên dịch được (`ring_buffer.c/h`) vẫn nằm trong repo — gây nhiễu cho dev mới, tăng thời gian onboarding.
- **Nợ hiệu năng**: malloc/free lặp lại trong hot-path hiển thị (mục M3) — chưa gây vấn đề ở quy mô hiện tại nhưng sẽ lộ rõ khi refresh rate/kích thước màn hình tăng.
- **Nợ khả năng mở rộng tương lai**: SRM mới có 1 command, chưa được kiểm chứng ở quy mô nhiều lệnh — sẽ cần thêm cơ chế định danh lỗi rõ ràng hơn (mục M2) trước khi mở rộng nhiều tính năng qua kênh này (volume, seek, EQ...).
- **Nợ cấu hình**: không tách biệt build debug/release (`DEVELOPER_CONFIGURATION` luôn bật) — mọi bản build đều mang theo overhead + lộ log debug ra console sản phẩm thật.

---

## 19. README.md

Đã tạo file `README.md` tại gốc repo (`e:\project\Audio\README.md`) với đầy đủ các mục được yêu cầu: Project Overview, Features, Hardware, Software Stack, Folder Structure, System Architecture, Boot Sequence, Runtime Flow, Task Architecture, Event Flow, Queue Flow, Notification Flow, State Machines, Audio/Animation/Display/Bluetooth Pipeline, Memory Management, Configuration, Build, Flash, Debug, Logging, Future Extension Points, Known Limitations — viết bằng tiếng Việt để nhất quán với toàn bộ comment trong codebase, mục tiêu giúp dev mới hiểu toàn bộ dự án trong khoảng 15 phút.

---

## 20. Đánh giá cuối cùng

### Điểm số (thang 1-10)

| Tiêu chí | Điểm | Ghi chú ngắn |
|---|:---:|---|
| Architecture | 7 | Owner Task + state machine tập trung rõ ràng; còn thiếu tách domain trong `gsPlayerContext` |
| Readability | 8 | Convention nhất quán cao, comment tiếng Việt chi tiết, section banner đồng đều |
| Modularity | 7 | 3/4 task cùng khuôn thuật toán rất tốt; SRM mới thử nghiệm ở quy mô nhỏ (1 command) |
| Performance | 6 | Ổn cho use case hiện tại; có 1 điểm malloc/free lặp lại trong hot-path (M3) |
| Reliability | 5 | Bug watchdog (C1) là điểm trừ nặng nhất — ảnh hưởng toàn hệ thống dù chưa gây reset ngay |
| Scalability | 5 | 4 task cùng priority, cùng core; chưa có chỗ trống rõ ràng cho tính năng nặng hơn (BT/WiFi) mà không đánh giá lại phân bổ core/priority |
| Maintainability | 7 | Convention tốt giúp dễ maintain, nhưng thiếu test tự động và còn code chết trong repo |
| ESP-IDF Best Practices | 6 | Dùng đúng API chuẩn (VFS, SDMMC, i2c_master mới), nhưng bug C1 vi phạm giả định cơ bản nhất của ESP-IDF (không bao giờ block task main không yield) |
| FreeRTOS Design | 7 | Notification-based design nhất quán, đúng đắn cho use case; thiếu `volatile`/phân bậc priority là điểm trừ |
| **Tổng thể (Overall)** | **6.5** | Kiến trúc và coding convention thuộc hàng tốt so với 1 dự án ESP32 hobby/mid-scale, nhưng bug C1 + thiếu test tự động khiến chưa sẵn sàng release thương mại nếu không sửa trước |

### Trả lời 7 câu hỏi cuối

**1. Sản phẩm đã sẵn sàng release thương mại chưa?**
Chưa. Lý do chính không phải vì thiếu tính năng (kiến trúc lõi đã vững), mà vì: (a) bug C1 (watchdog starve) là hành vi runtime sai chưa từng được phát hiện qua test thủ công thông thường — chỉ lộ ra khi đọc kỹ `sdkconfig`; (b) hoàn toàn không có test tự động nào để bảo vệ trước khi release; (c) không có xử lý lỗi cho người dùng cuối khi thẻ SD hỏng/thiếu (M4) — trải nghiệm sản phẩm thật sẽ là "màn hình đứng im, không hiểu vì sao".

**2. Cần đổi gì trước khi ship?**
Bắt buộc: sửa C1 (`while(1);` → `vTaskDelay`/`vTaskDelete`), sửa H1 (NULL-check mutex), thêm xử lý lỗi SD card hiển thị lên OLED (M4), xoá code chết (H2 + `frame.c/h`) để giảm rủi ro nhầm lẫn khi onboarding dev mới trước khi maintain lâu dài. Nên làm nhưng không bắt buộc ngay: auto-next cuối bài (S3), tách build debug/release (S1).

**3. Top 10 bug rủi ro cao nhất** (đã liệt kê đầy đủ ở mục 17, tóm tắt theo thứ tự ưu tiên): C1 (watchdog starve) > H1 (NULL mutex check) > H2 (dead code không compile) > M1 (thiếu volatile, fragile invariant) > M2 (SRM registry đầy âm thầm) > M3 (malloc/free hot-path) > M4 (SD mount lỗi không xử lý) > L1 (double_buffer start/count lệch khi load lỗi) > L2 (ISR yield không dùng, dead code) > L3 (global thiếu static).

**4. Top 10 quyết định thiết kế tốt nhất:**
1. Thuật toán thống nhất `GetXState + Worker + lbHasPendingNotify` cho `Oled_Task`/`Sdcard_Task`/`Mp3_Task` — dễ đọc, dễ maintain, dễ thêm task mới theo đúng khuôn.
2. Tách `playbackState` khỏi `buttonState` — giải quyết đúng bản chất "trạng thái tức thời vs trạng thái liên tục" của hệ thống nút bấm.
3. Kiến trúc Owner Task (SRM) cho VS1053 — loại bỏ hoàn toàn rủi ro 2 task cùng lúc bắn lệnh SPI vào VS1053.
4. SRM registry per-task response queue (thay vì 1 queue chung cho mọi bên gọi) — tránh đúng bug "nhận nhầm response của task khác" mà thiết kế đơn giản hơn (1 queue chung) chắc chắn sẽ gặp.
5. `gpMp3File` giữ mở xuyên suốt Pause/Resume — đúng UX, tránh phát lại từ đầu.
6. `currentSong` tách khỏi `cursor` — cho phép browse menu trong lúc nhạc vẫn phát nền (tính năng peek-menu) mà không làm rối logic phát nhạc.
7. Double buffer + event bits (`EVT_PRELOAD`/`EVT_LOAD_MISS`) tách nạp dữ liệu animation (Sdcard_Task) khỏi vẽ (Oled_Task) — không block nhau.
8. Đồng bộ animation theo decode-time thực tế của VS1053 (không theo tick hệ thống) — animation không bao giờ trôi so với nhạc dù có trễ phần cứng.
9. Toàn bộ buffer lớn dùng static thay vì heap — loại bỏ rủi ro fragmentation ở hot-path.
10. Coding convention nhất quán cao (naming + section banner + doc comment song song `.h`/`.c`) — giảm chi phí đọc hiểu cho người maintain sau này.

**5. Nên refactor gì đầu tiên?**
Theo thứ tự: (1) sửa C1 ngay lập tức (rẻ, 1 dòng, rủi ro cao nếu để lâu); (2) dọn dead code (`ring_buffer.*`, `frame.*`, `BTN*_BIT`) để giảm nhiễu trước khi có thêm người tham gia dự án; (3) thêm `volatile` cho `mainState`/`playbackState` (rẻ, phòng ngừa bug tương lai); (4) tách domain trong `gsPlayerContext` — nên làm **trước khi** thêm tính năng playlist/volume, không phải sau, vì càng nhiều module phụ thuộc struct phẳng càng khó tách.

**6. Điều gì sẽ hỏng khi thêm tính năng?**
Thêm nhiều bài hát hơn: `SDCARD_MAX_SONGS = 200` cứng (`sdcard.h:18`) — vượt quá sẽ bị bỏ qua âm thầm ở `Sdcard_ScanAndCreateDb()` (`while` điều kiện dừng khi đầy, không log cảnh báo). Thêm nhiều task hơn (vd network task cho OTA): tất cả đang pin core 1 cùng priority 5 — cần đánh giá lại phân bổ core/priority, nếu không sẽ ảnh hưởng độ mượt animation/audio. Thêm nhiều lệnh SRM hơn: registry 8 slot vẫn đủ (theo task, không theo lệnh) nhưng cần thêm log khi đầy (M2).

**7. Kiến trúc có hỗ trợ được các hướng mở rộng sau không?**
- **Nhiều animation hơn**: Có, dễ dàng — `double_buffer.c`/`sync_frame.c` không phụ thuộc nội dung animation cụ thể, chỉ cần đúng định dạng file.
- **Nhiều định dạng nhạc hơn**: Có, ở mức phần cứng (VS1053 hỗ trợ đa định dạng) nhưng cần thêm logic phân loại theo extension trong `Mp3_StreamCurrentSong()` — hiện chưa có.
- **Bluetooth speaker mode**: Về mặt lý thuyết kiến trúc Owner Task/SRM có thể mở rộng thêm 1 "owner" mới cho Bluetooth audio sink, nhưng **cần đánh giá lại toàn bộ phân bổ priority/core** trước (mục 3.4) vì BT stack trên ESP32 tốn CPU/RAM đáng kể và hiện tất cả 4 task đã dồn vào core 1.
- **WiFi OTA**: Tương tự — không có gì trong kiến trúc hiện tại cản trở về mặt logic (OTA task độc lập, không cần đụng `gsPlayerContext`), nhưng cần dự trù RAM (WiFi stack tốn nhiều heap) trong khi hiện tại chưa có ngân sách bộ nhớ nào được tính toán tường minh cho việc này.
- **Web config**: cần thêm WiFi (như trên) + 1 lớp HTTP server — không có trở ngại kiến trúc, nhưng "config" hiện tại nằm cứng trong `config.h` (compile-time), sẽ cần thêm 1 cơ chế đọc/ghi cấu hình runtime (NVS) hoàn toàn mới, chưa có nền tảng nào trong code hiện tại.
- **Migrate sang LVGL**: Đây là thay thế toàn bộ tầng Display Pipeline, không phải mở rộng — `ssd1306.c` hiện là driver vẽ trực tiếp (immediate-mode), không có khái niệm object/widget. `Menu_Draw()`/`Oled_DrawFrame()` sẽ cần viết lại hoàn toàn theo mô hình LVGL (object tree + `lv_timer`), nhưng phần backend I2C/SPI (`ssd1306_i2c_new.c`) có thể tái sử dụng làm display driver adapter cho LVGL nếu viết lại đúng interface `lv_disp_drv_t`.
