#include <driver/i2c.h>
#include <esp_log.h>
#include <esp_rom_sys.h> // Include for esp_rom_delay_us in ESP-IDF 5.2.1
#include "keypad.h"

#define PCF8574_ADDR 0x23
#define I2C_TIMEOUT_MS 1000
#define DEBOUNCE_DELAY_MS 300 // Match Arduino debounce delay

static i2c_port_t keypad_i2c_port;
static const char keys[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'.', '0', '#', 'D'}
};
static TickType_t button_timer = 0;
static bool button_pressed = false;
static char pressed_character[2] = {0}; // Single character string

static SemaphoreHandle_t i2c_semaphore;

static esp_err_t write_pcf8574(uint8_t row_mask) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF8574_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, row_mask, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        ESP_LOGE("Keypad", "Failed to write row mask 0x%02X: %s", row_mask, esp_err_to_name(ret));
    }
    return ret;
}

static uint8_t read_pcf8574(uint8_t row_mask) {
    if (xSemaphoreTake(i2c_semaphore, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE("Keypad", "Failed to take I2C semaphore");
        return 0xFF;
    }

    // Write the row mask
    esp_err_t write_ret = write_pcf8574(row_mask);
    if (write_ret != ESP_OK) {
        xSemaphoreGive(i2c_semaphore);
        return 0xFF;
    }

    // Small delay to allow PCF8574 to settle
    esp_rom_delay_us(100); // 100µs delay

    // Read the data
    uint8_t data;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF8574_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_semaphore);

    if (ret != ESP_OK) {
        ESP_LOGE("Keypad", "Failed to read PCF8574 with mask 0x%02X: %s", row_mask, esp_err_to_name(ret));
        return 0xFF;
    }

    // ESP_LOGI("Keypad", "Row mask 0x%02X, data: 0x%02X (%d)", row_mask, data, data);
    return data;
}

// char keypad_scan(void) {
//     uint8_t row_data[4];
//     char key = '\0';

//     // Handle button press state and debounce
//     if (button_pressed) {
//         if ((xTaskGetTickCount() - button_timer) > (DEBOUNCE_DELAY_MS / portTICK_PERIOD_MS)) {
//             button_timer = xTaskGetTickCount();
//             button_pressed = false;
//             pressed_character[0] = '\0'; // Reset pressed character
//         }
//     } else {
//         // Scan all rows
//         row_data[0] = read_pcf8574(0b11111110); // Row 1 (P0 low)
//         vTaskDelay(1 / portTICK_PERIOD_MS);
//         row_data[1] = read_pcf8574(0b11111101); // Row 2 (P1 low)
//         vTaskDelay(1 / portTICK_PERIOD_MS);
//         row_data[2] = read_pcf8574(0b11111011); // Row 3 (P2 low)
//         vTaskDelay(1 / portTICK_PERIOD_MS);
//         row_data[3] = read_pcf8574(0b11110111); // Row 4 (P3 low)

//         // Debug raw data for all rows
//         ESP_LOGI("Keypad", "Raw row data: R0=0x%02X, R1=0x%02X, R2=0x%02X, R3=0x%02X",
//                  row_data[0], row_data[1], row_data[2], row_data[3]);

//         // Check for keypress in each row
//         for (int row = 0; row < 4; row++) {
//             if (row_data[row] == 0xFF) continue; // Skip if I2C read failed
//             switch (row_data[row]) {
//                 case 0b11101110: key = keys[row][0]; break; // 238, '1', '4', '7', '*'
//                 case 0b11011110: key = keys[row][1]; break; // 222, '2', '5', '8', '0'
//                 case 0b10111110: key = keys[row][2]; break; // 190, '3', '6', '9', '#'
//                 case 0b01111110: key = keys[row][3]; break; // 126, 'A', 'B', 'C', 'D'
//             }
//             if (key != '\0') {
//                 if (!button_pressed) {
//                     button_pressed = true;
//                     button_timer = xTaskGetTickCount();
//                     pressed_character[0] = key;
//                     pressed_character[1] = '\0';
//                     ESP_LOGI("Keypad", "Detected '%c' (Raw: 0x%02X)", key, row_data[row]);
//                     return key;
//                 }
//             }
//         }
//     }

//     return '\0';
// }

char keypad_scan(void) {
    uint8_t row_data[4];
    char key = '\0';

    // Only scan if no button is currently pressed (debounce)
    if (!button_pressed) {
        // Scan all rows
        row_data[0] = read_pcf8574(0b11111110); // Row 1 (P0 low)
        vTaskDelay(1 / portTICK_PERIOD_MS);
        row_data[1] = read_pcf8574(0b11111101); // Row 2 (P1 low)
        vTaskDelay(1 / portTICK_PERIOD_MS);
        row_data[2] = read_pcf8574(0b11111011); // Row 3 (P2 low)
        vTaskDelay(1 / portTICK_PERIOD_MS);
        row_data[3] = read_pcf8574(0b11110111); // Row 4 (P3 low)

        // Debug raw data for all rows
        // ESP_LOGI("Keypad", "Raw row data: R0=0x%02X, R1=0x%02X, R2=0x%02X, R3=0x%02X",
        //          row_data[0], row_data[1], row_data[2], row_data[3]);

        // Check for keypress in each row
        for (int row = 0; row < 4; row++) {
            if (row_data[row] == 0xFF) continue; // Skip if I2C read failed
            
            // Match the patterns from Arduino code
            switch (row_data[row]) {
                // Row 1 (P0 low)
                case 238: key = keys[0][0]; break; // 1110 1110 - '1'
                case 222: key = keys[0][1]; break; // 1101 1110 - '2'
                case 190: key = keys[0][2]; break; // 1011 1110 - '3'
                case 126: key = keys[0][3]; break; // 0111 1110 - 'A'
                
                // Row 2 (P1 low)
                case 237: key = keys[1][0]; break; // 1110 1101 - '4'
                case 221: key = keys[1][1]; break; // 1101 1101 - '5'
                case 189: key = keys[1][2]; break; // 1011 1101 - '6'
                case 125: key = keys[1][3]; break; // 0111 1101 - 'B'
                
                // Row 3 (P2 low)
                case 235: key = keys[2][0]; break; // 1110 1011 - '7'
                case 219: key = keys[2][1]; break; // 1101 1011 - '8'
                case 187: key = keys[2][2]; break; // 1011 1011 - '9'
                case 123: key = keys[2][3]; break; // 0111 1011 - 'C'
                
                // Row 4 (P3 low)
                case 231: key = keys[3][0]; break; // 1110 0111 - '*'
                case 215: key = keys[3][1]; break; // 1101 0111 - '0'
                case 183: key = keys[3][2]; break; // 1011 0111 - '#'
                case 119: key = keys[3][3]; break; // 0111 0111 - 'D'
            }
            
            if (key != '\0') {
                if (!button_pressed) {
                    button_pressed = true;
                    button_timer = xTaskGetTickCount();
                    pressed_character[0] = key;
                    pressed_character[1] = '\0';
                    ESP_LOGI("Keypad", "Detected '%c' (Raw: 0x%02X)", key, row_data[row]);
                    return key;
                }
            }
        }
    } else {
        // Handle debounce timeout
        if ((xTaskGetTickCount() - button_timer) > (DEBOUNCE_DELAY_MS / portTICK_PERIOD_MS)) {
            button_timer = xTaskGetTickCount();
            button_pressed = false;
            pressed_character[0] = '\0'; // Reset pressed character
        }
    }

    return '\0';
}

esp_err_t keypad_init(i2c_port_t i2c_port) {
    keypad_i2c_port = i2c_port;
    i2c_semaphore = xSemaphoreCreateMutex();
    if (i2c_semaphore == NULL) {
        ESP_LOGE("Keypad", "Failed to create I2C semaphore");
        return ESP_FAIL;
    }
    ESP_LOGI("Keypad", "Initialized keypad on I2C port %d, address 0x%02X", i2c_port, PCF8574_ADDR);
    return ESP_OK;
}