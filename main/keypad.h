#ifndef KEYPAD_H
#define KEYPAD_H

#include <esp_err.h>

#define PCF8574_ADDR 0x23
esp_err_t keypad_init(i2c_port_t i2c_port);
char keypad_scan(void);

#endif // KEYPAD_H