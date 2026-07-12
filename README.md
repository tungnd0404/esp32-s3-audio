# Audio Player (ESP32-S3) — Trình phát nhạc MP3 có màn hình OLED animation

Firmware cho một trình phát nhạc MP3 cầm tay dựa trên ESP32-S3: đọc file `.mp3` từ thẻ nhớ SD, giải mã bằng chip VS1053, phát ra loa, đồng thời hiển thị animation trên màn hình OLED SSD1306 đồng bộ theo thời gian giải mã thực tế của bài hát. Điều khiển bằng 3 nút bấm vật lý (Next/Prev/Play).

Tài liệu này giúp một developer mới nắm được toàn bộ kiến trúc hệ thống trong khoảng 15 phút, trước khi đọc code.

## Mục lục

- [Tổng quan dự án](#tổng-quan-dự-án)
- [Tính năng](#tính-năng)
- [Phần cứng](#phần-cứng)
- [Software Stack](#software-stack)
- [Cấu trúc thư mục](#cấu-trúc-thư-mục)
- [Kiến trúc hệ thống](#kiến-trúc-hệ-thống)
- [Trình tự khởi động (Boot Sequence)](#trình-tự-khởi-động-boot-sequence)
- [Luồng chạy runtime (Runtime Flow)](#luồng-chạy-runtime-runtime-flow)
- [Kiến trúc Task (Task Architecture)](#kiến-trúc-task-task-architecture)
- [Luồng sự kiện (Event Flow)](#luồng-sự-kiện-event-flow)
- [Luồng Queue (Queue Flow)](#luồng-queue-queue-flow)
- [Luồng Notification (Notification Flow)](#luồng-notification-notification-flow)
- [Các State Machine](#các-state-machine)
- [Audio Pipeline](#audio-pipeline)
- [Animation Pipeline](#animation-pipeline)
- [Display Pipeline](#display-pipeline)
- [Bluetooth Pipeline](#bluetooth-pipeline)
- [Quản lý bộ nhớ (Memory Management)](#quản-lý-bộ-nhớ-memory-management)
- [Cấu hình (Configuration)](#cấu-hình-configuration)
- [Build](#build)
- [Flash](#flash)
- [Debug](#debug)
- [Logging](#logging)
- [Hướng mở rộng trong tương lai](#hướng-mở-rộng-trong-tương-lai)
- [Hạn chế đã biết (Known Limitations)](#hạn-chế-đã-biết-known-limitations)

---

## Tổng quan dự án

Thiết bị gồm: 1 khe đọc thẻ SD (chứa file `.mp3` + file `.bin` animation đi kèm mỗi bài), 1 chip giải mã MP3 VS1053 (giao tiếp SPI), 1 màn hình OLED SSD1306 128x64 (giao tiếp I2C hoặc SPI), và 3 nút bấm vật lý.

Firmware chạy trên FreeRTOS (ESP-IDF), tổ chức thành 4 task chính giao tiếp với nhau hoàn toàn qua **task notification** và 1 queue lệnh, không dùng biến cờ toàn cục để đồng bộ trạng thái giữa các task.

## Tính năng

- Phát file MP3 từ thẻ nhớ SD qua chip giải mã cứng VS1053.
- Danh sách bài hát dạng menu cuộn được trên OLED, tự nhận diện bài hát khi quét thẻ SD (yêu cầu mỗi `song.mp3` phải có `song.bin` đi kèm mới được liệt kê).
- Animation trên OLED đồng bộ theo thời gian giải mã audio thực tế (không đồng bộ theo thời gian hệ thống, để không bị trôi khi audio bị delay).
- Điều khiển bằng 3 nút: Next, Prev, Play (Play còn dùng để double-click "peek" MENU trong lúc đang phát nhạc, tự động quay lại màn hình PLAYING sau 3 giây không thao tác).
- Play/Pause giữ nguyên vị trí phát (không phát lại từ đầu khi Resume).

## Phần cứng

| Thành phần | Giao tiếp | Chân GPIO (xem `config/config.h`, `driver/vs1053/vs1053.h`) |
|---|---|---|
| Thẻ nhớ SD | SDMMC 1-bit | CLK=39, CMD=38, D0=40 |
| VS1053 (giải mã MP3) | SPI | CS=5, DCS=26, DREQ=27, RESET=32 |
| OLED SSD1306 128x64 | I2C (mặc định, port 0) hoặc SPI (driver có sẵn cả 2, xem phần Display Pipeline) | SDA=8, SCL=9, RESET=-1 (không dùng) |
| Nút Next | GPIO input, ngắt cạnh xuống, pull-up | GPIO 16 |
| Nút Prev | GPIO input, ngắt cạnh xuống, pull-up | GPIO 17 |
| Nút Play | GPIO input, ngắt cạnh xuống, pull-up | GPIO 18 |

Chip mục tiêu: ESP32-S3 (2 nhân, CPU0/CPU1).

## Software Stack

- **ESP-IDF** (FreeRTOS + driver ESP32 chuẩn: `driver/gpio.h`, `driver/spi_master.h`, `driver/i2c_master.h`, `driver/sdmmc_host.h`, `esp_vfs_fat.h`...)
- Ngôn ngữ: C.
- Filesystem: FAT trên thẻ SD, truy cập qua VFS chuẩn (`fopen`/`fread`/`fwrite` như file thường).
- Không dùng RTOS abstraction layer nào khác ngoài FreeRTOS gốc của ESP-IDF (không LVGL, không Bluetooth stack, không WiFi — xem phần [Bluetooth Pipeline](#bluetooth-pipeline)).

## Cấu trúc thư mục

```
Audio/
├── main/
│   ├── Audio.c              # app_main() - điểm khởi động firmware
│   └── CMakeLists.txt       # danh sách source file được biên dịch
├── config/
│   └── config.h             # toàn bộ hằng số cấu hình phần cứng (chân GPIO, kích thước màn hình...)
├── task/                    # các FreeRTOS task chính (tầng ứng dụng)
│   ├── player_manager.c/h   # task điều phối trung tâm - state machine của toàn hệ thống
│   ├── oled.c/h             # task vẽ màn hình OLED (menu + animation)
│   ├── sdcard.c/h           # task nạp dữ liệu animation từ thẻ SD vào double buffer
│   └── mp3.c/h              # task owner duy nhất của VS1053, stream audio
├── driver/
│   ├── button/               # driver nút bấm (GPIO ISR)
│   ├── srm/                  # SRM - Shared Resource Manager (cơ chế Owner Task dùng chung)
│   ├── vs1053/                # driver chip giải mã MP3 VS1053 (SPI)
│   ├── buffer/
│   │   ├── double_buffer.c/h # cache 2 buffer luân phiên cho dữ liệu animation (frame.bin)
│   │   └── sync_frame.c/h    # tính frame animation cần hiển thị theo thời gian giải mã mp3
│   └── ssd1306/               # driver màn hình OLED (core + backend I2C/SPI + menu vẽ danh sách)
│       ├── ssd1306.c/h, ssd1306_i2c_new.c, ssd1306_spi.c
│       └── menu.c/h          # vẽ danh sách bài hát + cuộn menu
```

## Kiến trúc hệ thống

Hệ thống có **4 task FreeRTOS**, tất cả chạy ở priority 5, đều pin vào **core 1** (`xTaskCreatePinnedToCore(..., 1)`), giao tiếp với nhau hoàn toàn qua **task notification** (1 giá trị 32-bit/task, kiểu `eSetValueWithOverwrite`) phát ra từ `PlayerManager_Task`:

```
                    ┌─────────────────┐
   Button ISR ────► │ PlayerManager_   │
  (Next/Prev/Play)  │      Task        │  (chủ động duy nhất thay đổi gsPlayerContext)
                    └───────┬──────────┘
                            │ xTaskNotify (PlayerManager_ButtonStateType_e)
              ┌─────────────┼──────────────┐
              ▼             ▼              ▼
        ┌──────────┐  ┌──────────┐  ┌───────────┐
        │ Oled_Task│  │Sdcard_   │  │ Mp3_Task  │
        │          │  │Task      │  │ (owner    │
        │          │  │          │  │  VS1053)  │
        └──────────┘  └──────────┘  └─────┬─────┘
                                           │ xMp3CommandQueue (SRM)
                                     ┌─────┴──────┐
                                     │ sync_frame.c│ (chạy trong Oled_Task,
                                     │ gửi lệnh hỏi │  hỏi decode time để
                                     │ decode time  │  đồng bộ animation)
                                     └────────────┘
```

Nguyên tắc kiến trúc cốt lõi:

1. **1 nguồn sự thật duy nhất** cho trạng thái hệ thống: struct toàn cục `gsPlayerContext` (`task/player_manager.h`), chỉ `PlayerManager_Task` được phép ghi vào.
2. **"Owner Task" cho phần cứng dùng chung**: chỉ `Mp3_Task` được phép gọi trực tiếp API `vs1053_*`. Mọi module khác (vd `sync_frame.c`) muốn lấy dữ liệu từ VS1053 phải gửi lệnh qua queue và chờ phản hồi — xem [SRM](#luồng-queue-queue-flow).
3. **Không polling theo chu kỳ** ở tầng điều phối: mọi task (trừ lúc đang phát animation/audio) đều "ngủ" bằng `xTaskNotifyWait(..., portMAX_DELAY)`, chỉ thức khi có sự kiện thật.
4. **1 thuật toán dùng chung cho mọi task nhận notification** (xem [Task Architecture](#kiến-trúc-task-task-architecture)).

## Trình tự khởi động (Boot Sequence)

Thứ tự gọi trong `app_main()` (`main/Audio.c`), **thứ tự này có ý nghĩa** — đảo thứ tự có thể gây crash do gọi vào handle NULL:

1. `Srm_Init()` — tạo mutex bảo vệ registry của SRM. Phải gọi đầu tiên, khi hệ thống còn đơn luồng (chưa có task nào chạy song song để race).
2. `Oled_Init(&dev)` — khởi tạo I2C + SSD1306. Đặt trước các bước thao tác thẻ SD để có thể hiển thị lỗi ngay lên màn hình nếu mount/scan thất bại (xem bước 4).
3. `Sdcard_Mount()` — mount thẻ SD (SDMMC 1-bit), lấy `esp_err_t` trả về để kiểm tra ở bước 4.
4. `Sdcard_ScanAndCreateDb("/sdcard")` → `Sdcard_ReadDbFile()` — quét toàn bộ thẻ SD, ghi `/sdcard/songs.db`, nạp `gaSongList`/`gu16SongCount`. Nếu mount lỗi hoặc quét xong không có bài hát nào (`gu16SongCount == 0`), hiển thị thông báo lỗi lên OLED (`ssd1306_display_text`) và giữ 2 giây trước khi tiếp tục boot.
5. `Button_Init()` — cấu hình GPIO ngắt cho 3 nút (đặt sau cùng trong nhóm init phần cứng để tránh xử lý nhầm trạng thái nút được giữ sẵn lúc board đang boot).
6. `PlayerManager_Init()` — khởi tạo `gsPlayerContext` (dùng `gu16SongCount` đã có từ bước 4, nên **bước 4 bắt buộc phải chạy trước bước này**).
7. `Mp3_Init()` — tạo `xMp3CommandQueue` (bắt buộc trước khi tạo `Mp3_Task` và trước khi `PlayerManager_Task` có thể gửi notification tới `xMp3TaskHandle`).
8. Tạo lần lượt 4 task: `Sdcard_Task` → `Mp3_Task` → `PlayerManager_Task` → `Oled_Task` (đều priority 5, pin core 1).
9. `vTaskDelay(portMAX_DELAY);` — task `main` nhường CPU core 0 vĩnh viễn cho task Idle0 (trước đây là `while (1);` bận rộn, đã sửa — xem [Known Limitations](#hạn-chế-đã-biết-known-limitations)).

## Luồng chạy runtime (Runtime Flow)

Ví dụ luồng khi người dùng bấm nút **Next** lúc đang ở màn hình MENU:

1. `Button_NextISR` (`driver/button/button.c`) debounce (80ms) rồi `xTaskNotifyFromISR(xPlayerManagerTaskHandle, BTN_EVENT_NEXT, ...)`.
2. `PlayerManager_Task` thức dậy, thấy `mainState == MAIN_STATE_MENU` → gọi `PlayerManager_Update_Cursor()` để tính cursor mới (có xoay vòng), lưu `gsPlayerContext.buttonState = BTN_STATE_DOWN`.
3. Gửi `xTaskNotify(xOledTaskHandle, BTN_STATE_DOWN, eSetValueWithOverwrite)`. **Không** báo `Sdcard_Task`/`Mp3_Task` (chưa đổi bài đang phát, chỉ di chuyển con trỏ trong menu).
4. `buttonState` được đưa về `BTN_STATE_IDLE` ngay (giá trị này chỉ tồn tại trong khoảnh khắc xử lý).
5. `Oled_Task` nhận notification, map qua `Oled_GetDisplayState()` ra `OLED_DISPLAY_MENU` → gọi `Menu_UpdateScroll()` + `Menu_Draw()` vẽ lại danh sách.

Khi bấm **Play** lúc đang ở MENU (chọn bài + bắt đầu phát): `PlayerManager_Task` đổi `mainState = MAIN_STATE_PLAYING`, `playbackState = PLAYBACK_STATE_PLAY`, `currentSong = cursor`, rồi báo **cả 3 task** (Oled/Sdcard/Mp3) vì đây là sự kiện đổi bài thật sự — xem bảng đầy đủ trong [Notification Flow](#luồng-notification-notification-flow).

## Kiến trúc Task (Task Architecture)

`Oled_Task`, `Sdcard_Task`, `Mp3_Task` đều được viết theo **cùng 1 khuôn thuật toán** (chọn `Oled_Task` làm mẫu gốc):

```
void Xxx_Task(void *pvParameters)
{
    uint32_t lu32NotifyValue;
    bool lbHasPendingNotify = false;

    while (1)
    {
        if (lbHasPendingNotify == false)
        {
            xTaskNotifyWait(0, UINT32_MAX, &lu32NotifyValue, portMAX_DELAY);  // ngủ chờ sự kiện
        }
        lbHasPendingNotify = false;

        switch (Xxx_GetXState((PlayerManager_ButtonStateType_e)lu32NotifyValue))  // map sang state nội bộ
        {
            case ...:
                lbHasPendingNotify = Xxx_Worker(..., &lu32NotifyValue);  // vòng lặp "làm việc"
                break;
            ...
        }
    }
}
```

- **`Xxx_GetXState()`**: hàm map thuần (`switch`) từ `PlayerManager_ButtonStateType_e` (giá trị notification nhận từ `PlayerManager_Task`) sang 1 enum nội bộ, chỉ tồn tại trong file đó, giúp switch chính đọc theo đúng ngữ nghĩa của module (`Oled_DisplayStateType_e`, `Sdcard_LoadStateType_e`, `Mp3_PlaybackStateType_e`) thay vì phải nhớ ý nghĩa của từng `BTN_STATE_*`.
- **Hàm worker** (`Oled_PlayAnimation`, `Sdcard_LoadCurrentSong`, `Mp3_StreamCurrentSong`): 1 vòng `while` làm việc thật sự (vẽ animation / nạp buffer / stream mp3), **mỗi vòng lặp đều check non-blocking** (`xTaskNotifyWait(..., timeout=0)`) xem có sự kiện mới cần xử lý gấp không, để không bị trễ phản ứng.
- **Cơ chế `lbHasPendingNotify`**: nếu worker thoát ra vì vừa nhận 1 notification mới (không phải vì hết việc), giá trị notification đó đã nằm trong `lu32NotifyValue` — nếu vòng lặp ngoài gọi `xTaskNotifyWait()` lần nữa ngay thì giá trị đó sẽ mất (bị ghi đè bởi lần chờ tiếp theo). Cờ này báo cho vòng lặp ngoài: bỏ qua bước chờ, xử lý luôn giá trị đang có.

Vì cả 3 task cùng khuôn thuật toán, đọc hiểu 1 task là hiểu được cả 3 — đây là quyết định thiết kế tốt nhất của dự án, xem phần đánh giá cuối tài liệu.

`PlayerManager_Task` không theo khuôn này vì nó có thêm 1 nguồn sự kiện thứ 2 (timeout auto-return MENU) và không có "worker loop" riêng (mọi xử lý xảy ra ngay trong `switch`).

## Luồng sự kiện (Event Flow)

Có 2 tầng sự kiện tách biệt trong hệ thống:

- **Button → PlayerManager**: `Button_EventType_e` (`BTN_EVENT_NEXT/PREV/PLAY`) — sự kiện phần cứng thô, chỉ `PlayerManager_Task` hiểu.
- **PlayerManager → {Oled, Sdcard, Mp3}**: `PlayerManager_ButtonStateType_e` (`BTN_STATE_UP/DOWN/PLAY_NEW/PLAY/PAUSE/NEXT/PREV/BACK_MENU/IDLE`) — sự kiện đã được diễn giải theo ngữ cảnh (vd `BTN_EVENT_PLAY` ở MENU trở thành `BTN_STATE_PLAY_NEW`, ở PLAYING trở thành `BTN_STATE_PLAY`/`PAUSE`/`BACK_MENU` tuỳ tình huống). Đây cũng chính là kiểu dữ liệu truyền qua kênh notification — không có enum sự kiện riêng cho từng task nhận, tiết kiệm 1 tầng chuyển đổi không cần thiết.

## Luồng Queue (Queue Flow)

Kiến trúc **SRM (Shared Resource Manager — `driver/srm/`)** hiện thực hoá pattern "Owner Task": 1 task duy nhất sở hữu 1 tài nguyên dùng chung; mọi module khác truy cập gián tiếp qua queue lệnh. Hiện có **2 owner** dùng chung hạ tầng SRM:

| Tài nguyên | Owner task | Command queue | cmdId (`srm.h`) |
|---|---|---|---|
| VS1053 (phần cứng) | `Mp3_Task` | `xMp3CommandQueue` | `MP3_CMD_GET_DECODE_TIME` |
| Double buffer animation (RAM, xem [Animation Pipeline](#animation-pipeline)) | `Sdcard_Task` | `xSdCommandQueue` | `SDCARD_CMD_PRELOAD_BUFFER`, `SDCARD_CMD_LOAD_MISSING_FRAME` |

```
sync_frame.c                    xMp3CommandQueue                Mp3_Task
(chạy trong Oled_Task)          (tạo bởi Mp3_Init)          (owner VS1053)

Srm_SendCommand(                                          
  xMp3CommandQueue,        ────►  Srm_Message_s {          ───►  Mp3_ServicePendingCommand()
  MP3_CMD_GET_DECODE_TIME,          kind = COMMAND               (chỉ chạy trong lúc đang
  &payload,                          cmdId,                       stream nhạc - xem
  timeout)                           payload,                     Known Limitations)
                                     responseQueue  ←── riêng     
                                  }                    của task         │
                                                        gọi (SRM         ▼
       ◄──────────────────────  tự cấp phát,           Mp3_HandleCommand()
       response qua              xem bên dưới)          → vs1053_get_decoded_time()
       responseQueue riêng                              → Srm_Reply(payload)
```

- Mỗi **task gọi** `Srm_SendCommand()` lần đầu tiên sẽ được SRM tự tạo 1 response queue riêng (độ sâu 1), lưu trong registry nội bộ `gaTaskResponseQueueList[]` (`driver/srm/srm.c`), khoá theo `TaskHandle_t` — dùng lại cho mọi lần gọi sau. Việc đăng ký được bảo vệ bằng mutex `gxMutexTaskResponseQueueList` (tạo 1 lần trong `Srm_Init()`) để 2 task đăng ký lần đầu cùng lúc không ghi đè lẫn nhau.
- Vì mỗi task có response queue riêng, nhiều task gửi lệnh gần nhau **không bao giờ nhận nhầm response của nhau**, kể cả khi cùng gửi 1 loại `cmdId`.
- **Giới hạn quan trọng đã biết (VS1053)**: `Mp3_ServicePendingCommand()` chỉ được gọi bên trong `Mp3_StreamCurrentSong()` — tức chỉ khi `Mp3_Task` đang thực sự stream nhạc. Nếu `Mp3_Task` đang rảnh (chưa phát bài nào) hoặc đang Pause, lệnh gửi tới `xMp3CommandQueue` sẽ không có ai xử lý cho tới khi phát nhạc trở lại. Bên gọi (`Srm_SendCommand`) luôn có timeout + fallback dùng giá trị cũ nên không bị treo, nhưng dữ liệu trả về có thể "cũ" trong khoảng thời gian đó.
- Không có lệnh set volume/play/pause/seek qua SRM — các hành động này hiện được điều khiển trực tiếp bằng notification tới `Mp3_Task`, không qua SRM.
- Chỉ có **1 API gửi lệnh** (`Srm_SendCommand()`, luôn blocking + chờ phản hồi) cho mọi cmdId, kể cả `SDCARD_CMD_PRELOAD_BUFFER` (vốn chỉ là gợi ý, không bắt buộc owner phải xử lý ngay) — thay vì thêm 1 API "fire-and-forget" riêng, owner (`Sdcard_Task`) tự trả lời NGAY khi nhận được lệnh này (ack, trước khi thực sự xử lý) nên bên gửi hầu như không bị chặn. Xem chi tiết luồng double buffer trong [Animation Pipeline](#animation-pipeline).

## Luồng Notification (Notification Flow)

Bảng đầy đủ: `PlayerManager_Task` gửi notification tới task nào ứng với từng sự kiện (nguồn: `task/player_manager.c`):

| Sự kiện | Oled_Task | Sdcard_Task | Mp3_Task | Lý do |
|---|:---:|:---:|:---:|---|
| Next/Prev ở MENU (di chuyển cursor) | ✅ | ❌ | ❌ | Chưa đổi bài đang phát |
| Next/Prev lúc PLAYING (đổi bài) | ✅ | ✅ | ✅ | Đổi bài thật — cả 3 phải đồng bộ lại |
| Play ở MENU (chọn bài mới, autoplay) | ✅ | ✅ | ✅ | Đổi bài thật |
| Play/Pause đơn (lúc PLAYING) | ✅ | ❌ | ✅ | Không đổi bài nên Sdcard không cần nạp lại buffer |
| Double-click Play → về MENU (peek) | ✅ | ❌ | ❌ | Chỉ đổi giao diện, nhạc vẫn phát nền |
| Auto-return hết 3s ở MENU → về PLAYING | ✅ | ❌ | ❌ | Bài không đổi, chỉ đổi lại giao diện |

Giá trị notification gửi đi **chính là** `gsPlayerContext.buttonState` (kiểu `PlayerManager_ButtonStateType_e`) tại thời điểm gửi — dùng `eSetValueWithOverwrite` (ghi đè, không xếp hàng đợi nhiều giá trị).

## Các State Machine

`gsPlayerContext` (`task/player_manager.h`) gồm 3 field trạng thái tách biệt có chủ đích:

- **`mainState`** (`MAIN_STATE_MENU` / `MAIN_STATE_PLAYING`): đang ở màn hình nào.
- **`buttonState`**: ghi lại **hành động vừa xảy ra** do 1 lần bấm nút, **luôn tự về `BTN_STATE_IDLE`** ngay sau khi được xử lý xong trong cùng vòng lặp của `PlayerManager_Task` — mô phỏng đúng bản chất "bấm rồi nhả" của nút vật lý. Không dùng field này để biết nhạc đang PLAY hay PAUSE.
- **`playbackState`** (`PLAYBACK_STATE_IDLE/PLAY/PAUSE`): trạng thái phát nhạc **tồn tại liên tục**, chỉ đổi khi người dùng bấm Play/Pause thật sự. `Oled_PlayAnimation()`/`Mp3_StreamCurrentSong()` đều đọc field này (không đọc `buttonState`) để quyết định có tiếp tục vòng lặp phát/vẽ hay không — đây là lý do vì sao tách `playbackState` ra khỏi `buttonState` là cần thiết: nếu dùng chung 1 field, animation/audio sẽ dừng ngay khi `buttonState` tự reset về IDLE ở vòng lặp kế tiếp của `PlayerManager_Task`, dù người dùng chưa hề bấm Pause.

`cursor`/`currentSong`/`totalSong` cũng nằm trong struct này, dùng chung cho cả 3 task đọc.

## Audio Pipeline

```
Sdcard_GetSongByIndex(currentSong) → songPath (.mp3)
       │
       ▼
fopen(songPath) → gpMp3File (giữ mở xuyên suốt Play/Pause)
       │
       ▼ mỗi vòng lặp trong Mp3_StreamCurrentSong()
fread(VS1053_CHUNK_SIZE = 32 byte) → vs1053_send_buffer()
       │                                    │
       │                                    ▼ (bên trong vs1053_send_buffer)
       │                             vs1053_wait_dreq() — poll chân DREQ bằng
       │                             vTaskDelay(1ms), tự nhường CPU, không busy-wait
       ▼
Hết file (fread = 0) → fclose + vs1053_stop_song() → dừng, KHÔNG tự next bài
```

- File mp3 hiện tại (`gpMp3File`, static trong `mp3.c`) chỉ đóng khi **thực sự đổi bài** hoặc **hết file** — Pause không đóng file, nên Resume tiếp tục đúng vị trí đang dừng thay vì phát lại từ đầu.
- Không có auto-next khi hết bài (dừng hẳn, chờ người dùng bấm Next/Play).
- Không có điều khiển volume/seek nào được wiring vào UI (VS1053 giữ nguyên volume mặc định 50% do `vs1053_init()` đặt, xem `driver/vs1053/vs1053.c:150`).

## Animation Pipeline

```
Mp3_Task đang stream ──► VS1053 thanh ghi SCI_DECODE_TIME (giây, 16-bit, có rollover)
                              │
                              │ MP3_CMD_GET_DECODE_TIME qua SRM (mỗi 100ms - SYNC_FRAME_TICK_MS)
                              ▼
                     sync_frame.c: SyncFrame_UpdateVirtualTime()
                       - xử lý rollover 16-bit
                       - giới hạn delta ≤ 1s (chống nhiễu)
                       - nội suy mượt (gf32VirtualTime) giữa 2 mốc 100ms
                              │
                              ▼
                     SyncFrame_GetFrameIndex() = virtualTime * FRAME_PER_SECOND (15 fps)
                              │
                              ▼
                     Oled_PlayAnimation(): DoubleBuffer_GetFrame(frameIndex) → vẽ nếu frame đổi
```

Animation được đồng bộ theo **thời gian giải mã audio thực tế** (không phải thời gian hệ thống `xTaskGetTickCount`) — nếu VS1053 tạm khựng vì lý do phần cứng, animation tự động khựng theo, tránh trôi hình so với nhạc.

`Oled_PlayAnimation()` chỉ vẽ lại khi `frameIndex` thực sự đổi so với lần vẽ trước (tránh vẽ lại I2C/SPI thừa), dù vòng lặp poll nhanh gấp đôi tốc độ frame thật (`OLED_ANIMATION_DELAY_MS = frame_interval / 2`).

**Double buffer (`driver/buffer/double_buffer.c`)** — cache 2 buffer (A/B) luân phiên, mỗi buffer giữ `CACHE_FRAMES = 15` frame đọc trước từ `frame.bin`, để `Oled_Task` không phải đọc thẻ SD trực tiếp trên đường vẽ (I/O thẻ SD chậm và không dự đoán được độ trễ). `Sdcard_Task` là **owner** duy nhất được ghi vào 2 buffer này (qua kiến trúc Owner Task/SRM, giống VS1053) — `Oled_Task` chỉ đọc qua `DoubleBuffer_GetFrame()`:

```
Oled_Task                    xSdCommandQueue                 Sdcard_Task
(DoubleBuffer_GetFrame)      (tạo bởi Sdcard_Init)         (owner double buffer)

Đọc quá 80% buffer     ──► Srm_SendCommand(               ──► Srm_Reply(ack=1) NGAY LẬP TỨC
đang dùng, chưa gửi         SDCARD_CMD_PRELOAD_BUFFER,          (trước khi nạp - để bên gửi
hint (block, nhưng           payload=0, timeout 5s)              không phải chờ nạp xong thật)
trả lời gần như tức      ◄── payload trả về: ack (bỏ qua) ──┐         │
thì vì owner ack trước                                       │         ▼
khi thực sự xử lý)                                            │   DoubleBuffer_Preload()
                                                               │   (nạp buffer còn lại,
                                                               └── nối ngay sau buffer đang dùng)

Frame cần không có ở    ──► Srm_SendCommand(                 ──► DoubleBuffer_LoadFrame(index)
buffer nào (miss)            SDCARD_CMD_LOAD_MISSING_FRAME,        (nạp đích danh frame bị
(block, timeout 5s)          payload = frame index,                thiếu vào buffer không
                              timeout 5s)                           đang dùng, XONG mới reply)
       ◄──────────────────── payload trả về: 1 = OK, 0 = lỗi ──────┘
```

Cả 2 loại yêu cầu đều đi qua cùng 1 API `Srm_SendCommand()` (không có API "fire-and-forget" riêng) — điểm khác biệt duy nhất là **thời điểm owner gọi `Srm_Reply()`**: `SDCARD_CMD_PRELOAD_BUFFER` được trả lời ngay khi vừa nhận (trước khi xử lý, payload trả về không mang ý nghĩa) nên bên gửi hầu như không bị chặn dù về bản chất là lời gọi blocking; `SDCARD_CMD_LOAD_MISSING_FRAME` chỉ được trả lời sau khi đã nạp xong thật vì bên gửi cần biết kết quả trước khi đọc tiếp. Nhờ owner luôn trả lời **trước khi** tự lấy `gxMutexDoubleBuffer` để nạp, bên gửi không còn giữ mutex chờ phản hồi tại thời điểm owner cần mutex đó — không có deadlock dù không cần giải phóng mutex thủ công trước khi gửi lệnh preload.

Trước đây cơ chế preload/miss dùng chung 1 `EventGroupHandle_t` (`EVT_PRELOAD`/`EVT_LOAD_MISS`/`EVT_READY`) không mang được payload, khiến `Sdcard_Task` phải **đoán** frame nào bị thiếu (heuristic "nạp buffer còn lại, bắt đầu từ ngay sau buffer đang dùng"). Sau khi chuyển sang SRM, yêu cầu miss mang thẳng chỉ số frame cần (`payload`), loại bỏ hoàn toàn phần đoán này.

## Display Pipeline

- **Core**: `driver/ssd1306/ssd1306.c` — giữ framebuffer dạng `PAGE_t _page[8]` (8 page x 128 byte) trong struct `SSD1306_t`, cung cấp API vẽ text/bitmap/scroll, không tự khoá gì (caller — ở đây là `Oled_Task`, task duy nhất đụng vào `dev` — chịu trách nhiệm không truy cập song song).
- **Backend**: dự án có sẵn cả `ssd1306_i2c_new.c` (dùng API `i2c_master.h` mới) và `ssd1306_spi.c`. `Oled_Init()` (`task/oled.c`) hiện chỉ gọi `i2c_master_init()` — tức **đang dùng I2C**, code SPI có sẵn nhưng không được kích hoạt qua đường init hiện tại.
- **2 chế độ vẽ**: `Menu_Draw()` (danh sách bài hát dạng text, `driver/ssd1306/menu.c`) và `Oled_DrawFrame()` (animation dạng bitmap từ `frame.bin`, `task/oled.c`) — loại trừ lẫn nhau theo `mainState`, không double-buffer ở tầng hiển thị (framebuffer trong `SSD1306_t` được ghi đè trực tiếp mỗi lần vẽ).

## Bluetooth Pipeline

**Không áp dụng.** Đã xác nhận: không có `CONFIG_BT_ENABLED` trong `sdkconfig`, không có bất kỳ include/API nào liên quan `esp_bt`/A2DP/BLE trong toàn bộ source code của dự án. Firmware hiện tại không có bất kỳ chức năng Bluetooth nào.

## Quản lý bộ nhớ (Memory Management)

Toàn bộ dữ liệu runtime chính đều là **static/global**, không dùng heap cho dữ liệu lớn:

| Buffer | Vị trí | Kích thước |
|---|---|---|
| `bufferA`/`bufferB` (double buffer animation) | `driver/buffer/double_buffer.c` | 2 × 15 × 1024 byte = **30 KB** (.bss) |
| `gau8Frame` (frame đang vẽ) | `task/oled.c` | 1024 byte |
| `SSD1306_t::_page[8]` (framebuffer OLED) | `driver/ssd1306/ssd1306.h` | 1024 byte |
| `gaSongList[SDCARD_MAX_SONGS]` | `task/sdcard.c` | 200 × ~68 byte ≈ 13.6 KB |
| Stack mỗi task | `main/Audio.c` | 4096 byte × 4 task = 16 KB |

Không có `malloc`/`free` trong pipeline audio/animation. `i2c_display_image()` (`driver/ssd1306/ssd1306_i2c_new.c`) trước đây gọi `malloc`/`free` mỗi lần ghi 1 page (tối đa ~120 lần/giây lúc phát animation) — đã thay bằng buffer tĩnh cố định 129 byte (`I2C_DISPLAY_MAX_WIDTH + 1`) để loại bỏ heap traffic khỏi hot-path này. Không dùng PSRAM/DMA-capable allocation ở bất kỳ đâu trong code hiện tại.

## Cấu hình (Configuration)

Toàn bộ hằng số cấu hình phần cứng nằm trong `config/config.h`: chân GPIO (I2C, nút bấm, SD card), kích thước màn hình, `FRAME_PER_SECOND`. Chân VS1053 nằm riêng trong `driver/vs1053/vs1053.h`.

`DEVELOPER_CONFIGURATION` (`config/config.h:8`) hiện **luôn được định nghĩa** — không có build "release" nào tắt các đoạn `printf`/log debug rải trong `player_manager.c`/`sdcard.c`.

## Build

Dự án dùng chuẩn ESP-IDF (`idf.py build`), danh sách source biên dịch khai báo tường minh trong `main/CMakeLists.txt` (không dùng wildcard glob) — thêm file `.c` mới **phải** tự tay thêm vào `SRCS`, nếu không sẽ không được biên dịch (từng xảy ra thật với `driver/vs1053/vs1053.c`, đã fix).

## Flash

Flash bằng `idf.py -p <PORT> flash` chuẩn ESP-IDF. Không có cấu hình OTA/phân vùng đặc biệt được rà soát riêng trong tài liệu này.

## Debug

- Log qua `ESP_LOGI/E` (tag `"SDCARD"` trong `sdcard.c`) và `printf` (rải trong `player_manager.c`, chỉ khi `DEVELOPER_CONFIGURATION` được định nghĩa — hiện luôn bật).
- `Sdcard_ReadDbFile()` in toàn bộ danh sách bài hát đã quét được ra console lúc boot — hữu ích để xác nhận thẻ SD được nhận diện đúng.

## Logging

Không có framework log tập trung hay cấp độ log cấu hình được theo module — mỗi file dùng `printf` hoặc `ESP_LOG*` tuỳ ý. Không có log ra file/thẻ SD (chỉ log ra UART console).

## Hướng mở rộng trong tương lai

- **Thêm định dạng nhạc khác (WAV/AAC/FLAC...)**: VS1053 hỗ trợ phần cứng nhiều định dạng ngoài MP3 — `Mp3_StreamCurrentSong()` (`task/mp3.c`) hiện không có logic phân biệt định dạng theo phần mở rộng file, cần bổ sung, nhưng kiến trúc Owner Task không cản trở việc này.
- **Thêm animation/hiệu ứng khác**: `double_buffer.c`/`sync_frame.c` không phụ thuộc nội dung frame cụ thể — chỉ cần file `.bin` đúng định dạng `FRAME_SIZE` byte/frame là tương thích.
- **Thêm lệnh SRM mới** (volume, seek...): thêm enum vào `Srm_CommandType_e` (`driver/srm/srm.h`) và 1 case trong `Mp3_HandleCommand()` (`task/mp3.c`) — không cần đổi kiến trúc.
- **Bluetooth speaker mode / WiFi OTA / web config**: chưa có nền tảng nào trong code hiện tại (không BT/WiFi stack). Cần đánh giá kỹ tranh chấp CPU/RAM với 4 task hiện có trước khi thêm — xem đánh giá "Scalability" trong báo cáo review riêng.
- **Chuyển sang LVGL**: `ssd1306.c` hiện là driver low-level tự vẽ text/bitmap trực tiếp, không có khái niệm widget/object — migrate sang LVGL là 1 việc thay thế toàn bộ tầng Display Pipeline, không phải mở rộng.

## Hạn chế đã biết (Known Limitations)

Toàn bộ bug tìm thấy trong đợt review kỹ thuật (Critical/High/Medium/Low) đã được sửa trực tiếp trong code — bao gồm: busy-loop `while(1);` trong `app_main()` starve Task Watchdog (nay dùng `vTaskDelay(portMAX_DELAY);`), thiếu NULL-check mutex trong `double_buffer_close()`/`double_buffer_total_frames()`, code chết/không biên dịch được `ring_buffer.c/h`/`frame.c/h` (đã xoá khỏi repo), thiếu `volatile` trên `mainState`/`playbackState` (`task/player_manager.h`), SRM registry đầy thất bại âm thầm (nay có `ESP_LOGE`), `malloc`/`free` lặp lại trong `i2c_display_image()` (nay dùng buffer tĩnh), `Sdcard_Mount()` thất bại không có phản hồi cho người dùng (nay hiển thị lỗi lên OLED), và cặp `start`/`count` không nhất quán khi `load_double_buffer_internal()` load lỗi giữa chừng. Chi tiết từng bug xem báo cáo review kỹ thuật riêng đi kèm (mục 17 — Bug Report).

Các hạn chế còn lại (không phải bug, là tính năng/thiết kế còn thiếu, chưa được yêu cầu sửa):

- **Không có auto-next khi hết bài**: `Mp3_StreamCurrentSong()` dừng hẳn khi `fread()` trả về 0, không tự chuyển bài kế tiếp.
- **Không có điều khiển volume**: `vs1053_set_volume()` chỉ được gọi 1 lần trong `vs1053_init()` (50% mặc định), không có đường dẫn UI nào gọi lại.
- **`Mp3_ServicePendingCommand()` chỉ chạy khi đang stream nhạc** — lệnh SRM gửi tới `Mp3_Task` lúc đang rảnh/Pause sẽ luôn timeout, dùng giá trị cache cũ (xem [Luồng Queue](#luồng-queue-queue-flow)).
- **4 task chính đều cùng priority 5, cùng pin core 1** — không có phân bậc ưu tiên giữa task time-critical (audio) và task ít quan trọng hơn (menu di chuyển cursor).
- **`DEVELOPER_CONFIGURATION` (`config/config.h`) luôn bật** — chưa tách được build debug/release, mọi bản build đều mang theo log debug.
