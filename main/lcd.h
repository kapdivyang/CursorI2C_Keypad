#ifndef LCD_H
#define LCD_H

#include <driver/i2c.h>
#include <esp_err.h>
#define LCD_ADDR 0x27

esp_err_t lcd_init(i2c_port_t i2c_port, uint8_t addr);
void lcd_clear(void);
void lcd_set_cursor(uint8_t row, uint8_t col);
void lcd_print(const char *fmt, ...);  // Changed to accept format arguments
void lcd_backlight(bool on);

// Add new cursor control functions
void lcd_cursor_show(bool show);     // Show/hide the cursor underscore
void lcd_cursor_blink(bool blink);   // Enable/disable cursor blinking

#endif // LCD_H