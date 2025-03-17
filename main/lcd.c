#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "lcd.h"
#include "stdarg.h"


#define LCD_ADDR 0x27
#define I2C_TIMEOUT_MS 1000

static i2c_port_t lcd_i2c_port;
static uint8_t lcd_addr;
static uint8_t backlight_state = 0x08;

// HD44780 commands
#define LCD_CLEAR 0x01
#define LCD_HOME 0x02
#define LCD_ENTRY_MODE 0x06
#define LCD_DISPLAY_ON 0x0C
#define LCD_SET_DDRAM 0x80

// Add LCD cursor control command definitions
#define LCD_DISPLAY_OFF           0x08
#define LCD_DISPLAY_ON_CURSOR_OFF 0x0C
#define LCD_DISPLAY_ON_CURSOR_ON  0x0E
#define LCD_DISPLAY_ON_CURSOR_BLINK 0x0F

static void lcd_write_nibble(uint8_t nibble, uint8_t rs) {
    uint8_t data = (nibble << 4) | (rs ? 0x01 : 0x00) | backlight_state;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (lcd_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data | 0x04, true); // Enable high
    i2c_master_write_byte(cmd, data, true);        // Enable low
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(lcd_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE("LCD", "Failed to write nibble (0x%02X): %s", data, esp_err_to_name(ret));
    }
    vTaskDelay(1 / portTICK_PERIOD_MS); // Reduced delay
}

static void lcd_write_byte(uint8_t data, uint8_t rs) {
    lcd_write_nibble(data >> 4, rs);
    lcd_write_nibble(data & 0x0F, rs);
    vTaskDelay(1 / portTICK_PERIOD_MS); // Reduced delay
}

static void lcd_command(uint8_t cmd) {
    vTaskDelay(50 / portTICK_PERIOD_MS); // Reduced delay
    lcd_write_byte(cmd, 0);
    vTaskDelay(5 / portTICK_PERIOD_MS); // Reduced delay for commands
}

esp_err_t lcd_init(i2c_port_t i2c_port, uint8_t addr) {
    lcd_i2c_port = i2c_port;
    lcd_addr = addr;

    ESP_LOGI("LCD", "Initializing LCD at address 0x%02X", lcd_addr);
    vTaskDelay(50 / portTICK_PERIOD_MS); // Reduced initial delay

    for (int i = 0; i < 3; i++) {
        lcd_write_nibble(0x03, 0);
        vTaskDelay(5 / portTICK_PERIOD_MS);
        lcd_write_nibble(0x03, 0);
        vTaskDelay(1 / portTICK_PERIOD_MS);
        lcd_write_nibble(0x03, 0);
        lcd_write_nibble(0x02, 0); // Set 4-bit mode
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    lcd_command(0x28); // Function set: 4-bit, 2 lines, 5x8 dots
    vTaskDelay(5 / portTICK_PERIOD_MS);
    lcd_command(LCD_DISPLAY_ON); // Display on, cursor off, blink off
    vTaskDelay(5 / portTICK_PERIOD_MS);
    lcd_command(LCD_CLEAR); // Clear display
    vTaskDelay(5 / portTICK_PERIOD_MS);
    lcd_command(LCD_HOME); // Reset cursor to home
    vTaskDelay(5 / portTICK_PERIOD_MS);
    lcd_command(LCD_ENTRY_MODE); // Entry mode: increment, no shift
    vTaskDelay(5 / portTICK_PERIOD_MS);

    return ESP_OK;
}

void lcd_clear(void) {
    // ESP_LOGI("LCD", "Clearing display");
    
    lcd_command(LCD_CLEAR);
    vTaskDelay(5 / portTICK_PERIOD_MS); // Reduced delay
    lcd_command(LCD_HOME);
    vTaskDelay(2 / portTICK_PERIOD_MS);
}

// void lcd_set_cursor(uint8_t col, uint8_t row) {
//     // ESP_LOGI("LCD", "Setting cursor to row %d, col %d", row, col);
//     uint8_t address = (row == 0) ? 0x40 : 0x00;
//     address += col;
//     lcd_command(LCD_SET_DDRAM | address);
//     vTaskDelay(2 / portTICK_PERIOD_MS); // Reduced delay
// }

// In lcd.c (corrected)
void lcd_set_cursor(uint8_t row, uint8_t col) { // <-- Row first, then column
    uint8_t address = (row == 0) ? 0x00 : 0x40;
    address += col;
    lcd_command(LCD_SET_DDRAM | address);
    vTaskDelay(2 / portTICK_PERIOD_MS);
}

// void lcd_print(const char *str) {
//     // ESP_LOGI("LCD", "Printing: %s", str);
//     while (*str) {
//         lcd_write_byte(*str++, 1);
//         vTaskDelay(1 / portTICK_PERIOD_MS); // Reduced delay
//     }
// }

void lcd_backlight(bool on) {
    // ESP_LOGI("LCD", "Setting backlight: %s", on ? "ON" : "OFF");
    backlight_state = on ? 0x08 : 0x00;
    lcd_write_byte(0, 0);
    vTaskDelay(2 / portTICK_PERIOD_MS); // Reduced delay
}

// Update lcd_print to handle format strings
void lcd_print(const char *fmt, ...) {
    char buf[33];  // 32 chars max plus null terminator
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    // Original implementation
    char *str = buf;
    while (*str) {
        lcd_write_byte(*str++, 1);
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// Implement cursor control functions
void lcd_cursor_show(bool show) {
    if (show) {
        // Display on, cursor on, blink off
        lcd_command(LCD_DISPLAY_ON_CURSOR_ON);
    } else {
        // Display on, cursor off, blink off
        lcd_command(LCD_DISPLAY_ON_CURSOR_OFF);
    }
}

void lcd_cursor_blink(bool blink) {
    if (blink) {
        // Display on, cursor on, blink on
        lcd_command(LCD_DISPLAY_ON_CURSOR_BLINK);
    } else {
        // Display on, cursor on, blink off
        lcd_command(LCD_DISPLAY_ON_CURSOR_ON);
    }
}