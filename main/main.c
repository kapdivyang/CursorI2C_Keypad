#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/i2c.h>
#include <esp_err.h>
#include <esp_log.h>
// #include "keypad.h"
#include "lcd.h"
#include <string.h>
#include <esp_log.h>
#include "keyboard.h"
#include "nvs_flash.h"


#define I2C_PORT I2C_NUM_0
#define I2C_SDA_IO 21
#define I2C_SCL_IO 22
#define I2C_FREQ_HZ 100000 // Reduced to 100kHz for better DS1307 compatibility
#define LCD_ADDR 0x27
#define MAX_INPUT_LEN 15

SemaphoreHandle_t lcd_semaphore;
bool in_keypad_mode = false;
bool in_keyboard_mode = false;
static char full_string[MAX_INPUT_LEN + 1] = {0}; // Global string for input

static void i2c_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));
    ESP_LOGI("I2C", "Initialized I2C on port %d, SDA: %d, SCL: %d", I2C_PORT, I2C_SDA_IO, I2C_SCL_IO);
}

void keypad_task(void *pvParameters) {
    char input[MAX_INPUT_LEN + 1] = {0};
    int input_pos = 0;
    bool local_in_keypad_mode = false;
    bool semaphore_taken = false;

    while (1) {
        char key = keypad_scan();
        if (key != '\0') {
            ESP_LOGI("KeypadTask", "Key pressed: '%c'", key);
            if (!local_in_keypad_mode && key == 'A') {
                local_in_keypad_mode = true;
                in_keypad_mode = true;
                if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE) {
                    semaphore_taken = true;
                    ESP_LOGI("KeypadTask", "Took semaphore for entering mode");
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("String: ");
                    lcd_set_cursor(0, 1);
                    lcd_print("Double: ");
                    xSemaphoreGive(lcd_semaphore);
                    semaphore_taken = false;
                    ESP_LOGI("KeypadTask", "Released semaphore after entering mode");
                }
            } else if (local_in_keypad_mode) {
                if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE) {
                    semaphore_taken = true;
                    ESP_LOGI("KeypadTask", "Took semaphore for key input");
                    lcd_clear();
                    if (key == 'A') {
                        // Convert string to double and display
                        double converted_number = atof(full_string);
                        lcd_set_cursor(0, 0);
                        lcd_print("String: ");
                        lcd_set_cursor(8, 0);
                        lcd_print(full_string);
                        lcd_set_cursor(0, 1);
                        lcd_print("Double: ");
                        lcd_set_cursor(8, 1);
                        char buf[16];
                        snprintf(buf, sizeof(buf), "%.2f", converted_number);
                        lcd_print(buf);
                        ESP_LOGI("KeypadTask", "String: %s, Double: %.2f", full_string, converted_number);
                        full_string[0] = '\0';
                        input_pos = 0;
                        memset(input, 0, sizeof(input));
                    } else if (key == 'D') {
                        if (input_pos > 0) {
                            input[--input_pos] = '\0';
                            strncpy(full_string, input, MAX_INPUT_LEN);
                            full_string[MAX_INPUT_LEN] = '\0';
                            lcd_set_cursor(0, 0);
                            lcd_print("String: ");
                            lcd_set_cursor(8, 0);
                            lcd_print(full_string);
                            lcd_set_cursor(0, 1);
                            lcd_print("Double: ");
                            lcd_set_cursor(8, 1);
                            lcd_print("       ");
                        }
                    } else if (key != 'B' && input_pos < MAX_INPUT_LEN) {
                        input[input_pos++] = key;
                        input[input_pos] = '\0';
                        strncpy(full_string, input, MAX_INPUT_LEN);
                        full_string[MAX_INPUT_LEN] = '\0';
                        lcd_set_cursor(0, 0);
                        lcd_print("String: ");
                        lcd_set_cursor(8, 0);
                        lcd_print(full_string);
                        lcd_set_cursor(0, 1);
                        lcd_print("Double: ");
                        lcd_set_cursor(8, 1);
                        lcd_print("       ");
                    }
                    xSemaphoreGive(lcd_semaphore);
                    semaphore_taken = false;
                    ESP_LOGI("KeypadTask", "Released semaphore after key input");
                }
            }
        } else {
            if (semaphore_taken) {
                xSemaphoreGive(lcd_semaphore);
                semaphore_taken = false;
                ESP_LOGI("KeypadTask", "Released semaphore (no key)");
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

// void seconds_task(void *pvParameters) {
//     int seconds = 0;
//     while (1) {
//         if (!in_keypad_mode && xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE) {
//             ESP_LOGI("SecondsTask", "Took semaphore for display update");
//             lcd_clear();
//             lcd_set_cursor(0, 0);
//             lcd_print("Seconds:");
//             char buf[8];
//             snprintf(buf, sizeof(buf), "%d", seconds++);
//             lcd_set_cursor(0, 9);
//             lcd_print(buf);
//             lcd_set_cursor(1, 0);
//             lcd_print("Press A to edit");
//             xSemaphoreGive(lcd_semaphore);
//             ESP_LOGI("SecondsTask", "Released semaphore after display update");
//         }
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//     }
// }

void seconds_task(void *pvParameters) {
    int seconds = 0;
    while (1) {
        // Only take the semaphore and update display if not in keypad mode
        if (!in_keyboard_mode) {
            if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE) {
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Seconds: %d", seconds++);
                lcd_set_cursor(1, 0);
                lcd_print("Press A to edit");
                xSemaphoreGive(lcd_semaphore);
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void splash_task(void *pvParameters) {
    if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI("SplashTask", "Took semaphore for splash screen");
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_print("Keypad 123-ABC");
        lcd_set_cursor(1, 0);
        lcd_print("Demonstration");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        xSemaphoreGive(lcd_semaphore);
        ESP_LOGI("SplashTask", "Released semaphore after splash screen");
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI("Main", "Starting application");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI("Main", "NVS Flash initialized");
    
    lcd_semaphore = xSemaphoreCreateMutex();
    if (lcd_semaphore == NULL) {
        ESP_LOGE("Main", "Failed to create LCD semaphore");
        return;
    }

    i2c_init();
    ESP_ERROR_CHECK(lcd_init(I2C_PORT, LCD_ADDR));
    ESP_ERROR_CHECK(keypad_init(I2C_PORT));

    lcd_backlight(true);

    xTaskCreate(splash_task, "splash_task", 2048, NULL, 7, NULL);
    // xTaskCreate(keypad_task, "keypad_task", 1024*4, NULL, 6, NULL);
    xTaskCreate(keyboard_task, "keypad_task", 1024*4, NULL, 6, NULL);
    xTaskCreate(seconds_task, "seconds_task", 2048, NULL, 5, NULL);

    ESP_LOGI("Main", "Tasks created, entering idle");
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}