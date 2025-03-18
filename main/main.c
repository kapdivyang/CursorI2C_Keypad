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

// Add extern declaration for the variable from keyboard.c
extern bool in_keyboard_mode; // Defined in keyboard.c

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_IO 21
#define I2C_SCL_IO 22
#define I2C_FREQ_HZ 100000 // Reduced to 100kHz for better DS1307 compatibility
#define LCD_ADDR 0x27
#define MAX_INPUT_LEN 15
#define INACTIVITY_TIMEOUT_MS 30000 // 30 seconds timeout

SemaphoreHandle_t lcd_semaphore;
bool in_keypad_mode = false;
// Remove this variable as we're using in_keyboard_mode from keyboard.c
// bool main_in_keyboard_mode = false;
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

// Remove the keypad_task function as we're now using keyboard_task from keyboard.c

// Also remove the commented-out seconds_task function

void seconds_task(void *pvParameters) {
    int seconds = 0;
    while (1) {
        // Only take the semaphore and update display if not in keyboard mode
        if (!in_keyboard_mode) { // Use the variable from keyboard.c instead of main_in_keyboard_mode
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

// Remove the main_keyboard_task function as we're now using keyboard_task from keyboard.c

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
    
    // Only use the keyboard_task from keyboard.c
    xTaskCreate(keyboard_task, "keyboard_task", 1024*4, NULL, 6, NULL);
    
    // Keep the seconds task
    xTaskCreate(seconds_task, "seconds_task", 2048, NULL, 5, NULL);

    ESP_LOGI("Main", "Tasks created, entering idle");
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}