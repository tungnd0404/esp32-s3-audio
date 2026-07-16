#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "ssd1306.h"
#include "i2c_config.h"
#include "ssd1306_config.h"

#define TAG "SSD1306"

/* I2C0_PORT_NUM dùng chung với I2c_Init() (i2c.c) - khai báo trong config/hardware/
   i2c_config.h, không tự định nghĩa lại 1 macro riêng (I2C_NUM) ở đây nữa như bản trước (từng
   có 1 bản sao y hệt logic chọn port của i2c.h, dễ lệch nhau nếu chỉ sửa 1 trong 2 chỗ) */

#define I2C_MASTER_FREQ_HZ 400000 // I2C clock of SSD1306 can run at 400 kHz max.
/* Đổi tên từ I2C_TICKS_TO_WAIT (bản vendor gốc) - tên cũ gợi ý đơn vị "tick" nhưng giá trị
   này thực chất được dùng THẲNG làm tham số xfer_timeout_ms (mili-giây) của
   i2c_master_transmit() (driver/i2c_master.h, ESP-IDF 5.x), không hề qua pdMS_TO_TICKS() ở
   bất kỳ nơi gọi nào bên dưới - tên cũ dễ khiến người sửa sau lầm tưởng cần bọc
   pdMS_TO_TICKS()/pdTICKS_TO_MS() quanh giá trị này, vô tình đổi sai đơn vị thời gian chờ */
#define I2C_XFER_TIMEOUT_MS 100	  // Maximum ms to wait before issuing a timeout.

// Chiều rộng tối đa 1 lần ghi page (bằng chiều rộng lớn nhất màn hình SSD1306 hỗ trợ,
// xem SSD1306_WIDTH trong config/hardware/ssd1306_config.h) - dùng làm kích thước buffer
// tĩnh trong i2c_display_image()
#define I2C_DISPLAY_MAX_WIDTH 128

/* i2c_master_init() (API "tự tạo bus riêng" của bản vendor gốc, nhận thẳng sda/scl) đã bị
   xoá khỏi đây - project dùng bus I2C dùng chung do I2c_Init() (driver/i2c/i2c.c) tạo 1 lần
   trong app_main(), Oled_Task chỉ gọi ssd1306_add_i2c_device() bên dưới để add device lên bus đã có
   sẵn, không cần API tự tạo bus riêng này. Không có call site nào tới hàm đó trong project
   (đã grep xác nhận trước khi xoá). */

void ssd1306_add_i2c_device(SSD1306_t * dev, i2c_port_t i2c_num, int16_t reset, uint16_t i2c_address)
{
	ESP_LOGI(TAG, "New i2c driver is used");
	ESP_LOGW(TAG, "Will not install i2c master driver");

	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = i2c_address,
		.scl_speed_hz = I2C_MASTER_FREQ_HZ,
	};
	i2c_master_dev_handle_t i2c_dev_handle;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(dev->_i2c_bus_handle, &dev_cfg, &i2c_dev_handle));

	if (reset >= 0) {
		//gpio_pad_select_gpio(reset);
		gpio_reset_pin(reset);
		gpio_set_direction(reset, GPIO_MODE_OUTPUT);
		gpio_set_level(reset, 0);
		vTaskDelay(50 / portTICK_PERIOD_MS);
		gpio_set_level(reset, 1);
	}

	dev->_address = i2c_address;
	dev->_flip = false;
	dev->_i2c_num = i2c_num;
	dev->_i2c_dev_handle = i2c_dev_handle;
}

void ssd1306_i2c_send_init(SSD1306_t * dev, int width, int height) {
	dev->_width = width;
	dev->_height = height;
	dev->_pages = 8;
	if (dev->_height == 32) dev->_pages = 4;
	
	uint8_t out_buf[27];
	int out_index = 0;
	out_buf[out_index++] = OLED_CONTROL_BYTE_CMD_STREAM;
	out_buf[out_index++] = OLED_CMD_DISPLAY_OFF;				// AE
	out_buf[out_index++] = OLED_CMD_SET_MUX_RATIO;			 // A8
	if (dev->_height == 64) out_buf[out_index++] = 0x3F;
	if (dev->_height == 32) out_buf[out_index++] = 0x1F;
	out_buf[out_index++] = OLED_CMD_SET_DISPLAY_OFFSET;		 // D3
	out_buf[out_index++] = 0x00;
	//out_buf[out_index++] = OLED_CONTROL_BYTE_DATA_STREAM;	// 40
	out_buf[out_index++] = OLED_CMD_SET_DISPLAY_START_LINE;	// 40
	//out_buf[out_index++] = OLED_CMD_SET_SEGMENT_REMAP;		// A1
	if (dev->_flip) {
		out_buf[out_index++] = OLED_CMD_SET_SEGMENT_REMAP_0; // A0
	} else {
		out_buf[out_index++] = OLED_CMD_SET_SEGMENT_REMAP_1;	// A1
	}
	out_buf[out_index++] = OLED_CMD_SET_COM_SCAN_MODE;		// C8
	out_buf[out_index++] = OLED_CMD_SET_DISPLAY_CLK_DIV;		// D5
	out_buf[out_index++] = 0x80;
	out_buf[out_index++] = OLED_CMD_SET_COM_PIN_MAP;			// DA
	if (dev->_height == 64) out_buf[out_index++] = 0x12;
	if (dev->_height == 32) out_buf[out_index++] = 0x02;
	out_buf[out_index++] = OLED_CMD_SET_CONTRAST;			// 81
	out_buf[out_index++] = 0xFF;
	out_buf[out_index++] = OLED_CMD_DISPLAY_RAM;				// A4
	out_buf[out_index++] = OLED_CMD_SET_VCOMH_DESELCT;		// DB
	out_buf[out_index++] = 0x40;
	out_buf[out_index++] = OLED_CMD_SET_MEMORY_ADDR_MODE;	// 20
	//out_buf[out_index++] = OLED_CMD_SET_HORI_ADDR_MODE;	// 00
	out_buf[out_index++] = OLED_CMD_SET_PAGE_ADDR_MODE;		// 02
	// Set Lower Column Start Address for Page Addressing Mode
	out_buf[out_index++] = 0x00;
	// Set Higher Column Start Address for Page Addressing Mode
	out_buf[out_index++] = 0x10;
	out_buf[out_index++] = OLED_CMD_SET_CHARGE_PUMP;			// 8D
	out_buf[out_index++] = 0x14;
	out_buf[out_index++] = OLED_CMD_DEACTIVE_SCROLL;			// 2E
	out_buf[out_index++] = OLED_CMD_DISPLAY_NORMAL;			// A6
	out_buf[out_index++] = OLED_CMD_DISPLAY_ON;				// AF

	esp_err_t res;
	res = i2c_master_transmit(dev->_i2c_dev_handle, out_buf, out_index, I2C_XFER_TIMEOUT_MS);
	if (res == ESP_OK) {
		ESP_LOGI(TAG, "OLED configured successfully");
	} else {
		ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d (%s)", dev->_address, dev->_i2c_num, res, esp_err_to_name(res));
	}
}


void i2c_display_image(SSD1306_t * dev, int page, int seg, const uint8_t * images, int width) {
	if (page >= dev->_pages) return;
	if (seg >= dev->_width) return;
	if (width > I2C_DISPLAY_MAX_WIDTH) {
		ESP_LOGE(TAG, "width %d exceeds max %d", width, I2C_DISPLAY_MAX_WIDTH);
		return;
	}

	int _seg = seg + SSD1306_OFFSETX;
	uint8_t columLow = _seg & 0x0F;
	uint8_t columHigh = (_seg >> 4) & 0x0F;

	int _page = page;
	if (dev->_flip) {
		_page = (dev->_pages - page) - 1;
	}

	// Buffer tĩnh cố định thay cho malloc/free mỗi lần gọi - hàm này được Oled_DrawFrame()
	// gọi tới 8 lần/frame trong lúc phát animation (tối đa hàng trăm lần/giây), malloc/free
	// lặp lại liên tục là nguồn gây phân mảnh heap không cần thiết trên hệ heap dùng chung
	uint8_t out_buf[I2C_DISPLAY_MAX_WIDTH + 1];
	int out_index = 0;
	out_buf[out_index++] = OLED_CONTROL_BYTE_CMD_STREAM;
	// Set Lower Column Start Address for Page Addressing Mode
	out_buf[out_index++] = (0x00 + columLow);
	// Set Higher Column Start Address for Page Addressing Mode
	out_buf[out_index++] = (0x10 + columHigh);
	// Set Page Start Address for Page Addressing Mode
	out_buf[out_index++] = 0xB0 | _page;

	esp_err_t res;
	res = i2c_master_transmit(dev->_i2c_dev_handle, out_buf, out_index, I2C_XFER_TIMEOUT_MS);
	if (res != ESP_OK)
		ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d (%s)", dev->_address, dev->_i2c_num, res, esp_err_to_name(res));

	out_buf[0] = OLED_CONTROL_BYTE_DATA_STREAM;
	memcpy(&out_buf[1], images, width);

	res = i2c_master_transmit(dev->_i2c_dev_handle, out_buf, width + 1, I2C_XFER_TIMEOUT_MS);
	if (res != ESP_OK)
		ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d (%s)", dev->_address, dev->_i2c_num, res, esp_err_to_name(res));
}

void i2c_contrast(SSD1306_t * dev, int contrast) {
	uint8_t _contrast = contrast;
	if (contrast < 0x0) _contrast = 0;
	if (contrast > 0xFF) _contrast = 0xFF;

	uint8_t out_buf[3];
	int out_index = 0;
	out_buf[out_index++] = OLED_CONTROL_BYTE_CMD_STREAM; // 00
	out_buf[out_index++] = OLED_CMD_SET_CONTRAST; // 81
	out_buf[out_index++] = _contrast;

	esp_err_t res = i2c_master_transmit(dev->_i2c_dev_handle, out_buf, 3, I2C_XFER_TIMEOUT_MS);
	if (res != ESP_OK)
		ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d (%s)", dev->_address, dev->_i2c_num, res, esp_err_to_name(res));
}


void i2c_hardware_scroll(SSD1306_t * dev, ssd1306_scroll_type_t scroll) {
	uint8_t out_buf[11];
	int out_index = 0;
	out_buf[out_index++] = OLED_CONTROL_BYTE_CMD_STREAM; // 00

	if (scroll == SCROLL_RIGHT) {
		out_buf[out_index++] = OLED_CMD_HORIZONTAL_RIGHT; // 26
		out_buf[out_index++] = 0x00; // Dummy byte
		out_buf[out_index++] = 0x00; // Define start page address
		out_buf[out_index++] = 0x07; // Frame frequency
		out_buf[out_index++] = 0x07; // Define end page address
		out_buf[out_index++] = 0x00; //
		out_buf[out_index++] = 0xFF; //
		out_buf[out_index++] = OLED_CMD_ACTIVE_SCROLL; // 2F
	} 

	if (scroll == SCROLL_LEFT) {
		out_buf[out_index++] = OLED_CMD_HORIZONTAL_LEFT; // 27
		out_buf[out_index++] = 0x00; // Dummy byte
		out_buf[out_index++] = 0x00; // Define start page address
		out_buf[out_index++] = 0x07; // Frame frequency
		out_buf[out_index++] = 0x07; // Define end page address
		out_buf[out_index++] = 0x00; //
		out_buf[out_index++] = 0xFF; //
		out_buf[out_index++] = OLED_CMD_ACTIVE_SCROLL; // 2F
	} 

	if (scroll == SCROLL_DOWN) {
		out_buf[out_index++] = OLED_CMD_CONTINUOUS_SCROLL; // 29
		out_buf[out_index++] = 0x00; // Dummy byte
		out_buf[out_index++] = 0x00; // Define start page address
		out_buf[out_index++] = 0x07; // Frame frequency
		//out_buf[out_index++] = 0x01; // Define end page address
		out_buf[out_index++] = 0x00; // Define end page address
		out_buf[out_index++] = 0x3F; // Vertical scrolling offset

		out_buf[out_index++] = OLED_CMD_VERTICAL; // A3
		out_buf[out_index++] = 0x00;
		if (dev->_height == 64)
		//out_buf[out_index++] = 0x7F;
		out_buf[out_index++] = 0x40;
		if (dev->_height == 32)
		out_buf[out_index++] = 0x20;
		out_buf[out_index++] = OLED_CMD_ACTIVE_SCROLL; // 2F
	}

	if (scroll == SCROLL_UP) {
		out_buf[out_index++] = OLED_CMD_CONTINUOUS_SCROLL; // 29
		out_buf[out_index++] = 0x00; // Dummy byte
		out_buf[out_index++] = 0x00; // Define start page address
		out_buf[out_index++] = 0x07; // Frame frequency
		//out_buf[out_index++] = 0x01; // Define end page address
		out_buf[out_index++] = 0x00; // Define end page address
		out_buf[out_index++] = 0x01; // Vertical scrolling offset

		out_buf[out_index++] = OLED_CMD_VERTICAL; // A3
		out_buf[out_index++] = 0x00;
		if (dev->_height == 64)
		//out_buf[out_index++] = 0x7F;
		out_buf[out_index++] = 0x40;
		if (dev->_height == 32)
		out_buf[out_index++] = 0x20;
		out_buf[out_index++] = OLED_CMD_ACTIVE_SCROLL; // 2F
	}

	if (scroll == SCROLL_STOP) {
		out_buf[out_index++] = OLED_CMD_DEACTIVE_SCROLL; // 2E
	}

	esp_err_t res = i2c_master_transmit(dev->_i2c_dev_handle, out_buf, out_index, I2C_XFER_TIMEOUT_MS);
	if (res != ESP_OK)
		ESP_LOGE(TAG, "Could not write to device [0x%02x at %d]: %d (%s)", dev->_address, dev->_i2c_num, res, esp_err_to_name(res));
}
