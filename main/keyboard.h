#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <esp_err.h>
#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Define missing variables
#define I2C_TIMEOUT_MS 1000
#define DEBOUNCE_DELAY_MS 300

// Device I2C addresses
#define DS1307_ADDR 0x68
#define EEPROM_24C32_ADDR 0x50

// Global variables that need to be declared
extern i2c_port_t keypad_i2c_port;
extern SemaphoreHandle_t i2c_semaphore;
extern bool button_pressed;
extern TickType_t button_timer;
extern char pressed_character[2];

typedef enum {
    PARAM_TYPE_DATE,
    PARAM_TYPE_TIME,
    PARAM_TYPE_NUMBER,
    PARAM_TYPE_ENABLE_DISABLE,
    PARAM_TYPE_MULTIPLE
} param_type_t;

typedef enum {
    GROUP_DATE_TIME,
    GROUP_PROTECTION,
    GROUP_STAGGERING,
    GROUP_CIVIL_TWILIGHT
} param_group_t;

// Define storage types
typedef enum {
    STORAGE_NVS,       // Store in ESP32 NVS
    STORAGE_RTC,       // Store in DS1307 RTC
    STORAGE_EEPROM     // Store in 24C32 EEPROM
} storage_type_t;

typedef enum {
    PARAM_ADDRESS_DATE,
    PARAM_ADDRESS_TIME,    
    PARAM_ADDRESS_3,
    PARAM_ADDRESS_4,
    PARAM_ADDRESS_5,
    PARAM_ADDRESS_6,
    PARAM_ADDRESS_7,
    PARAM_ADDRESS_8,
    PARAM_ADDRESS_9,
    PARAM_ADDRESS_10,
    PARAM_ADDRESS_11,
    PARAM_ADDRESS_12,
    PARAM_ADDRESS_13,
    PARAM_ADDRESS_14,
    PARAM_ADDRESS_15,
    PARAM_ADDRESS_16,
    PARAM_ADDRESS_17,
    PARAM_ADDRESS_18,
    PARAM_ADDRESS_19,
    PARAM_ADDRESS_20,
    PARAM_ADDRESS_21,
    PARAM_ADDRESS_22,
    PARAM_ADDRESS_23
} param_address_t;

typedef struct {
    const char *name;
    param_type_t type;
    param_group_t group;
    storage_type_t storage;  // Added storage type
    param_address_t address;
    void *value;
    void *default_value;
    void (*validate)(void *value);
} parameter_t;

// Function prototypes
esp_err_t keypad_init(i2c_port_t i2c_port);
char keypad_scan(void);
void keyboard_task(void *pvParameters);
void seconds_task(void *pvParameters);

// Validation functions
void validate_date(void *value);
void validate_time(void *value);
void validate_number(void *value);
void validate_enable_disable(void *value);
void validate_multiple(void *value);

// Storage functions
void store_parameters_to_nvs(void);
void load_parameters_from_nvs(void);
esp_err_t store_parameter_to_rtc(int param_idx);
esp_err_t load_parameter_from_rtc(int param_idx);
esp_err_t store_parameter_to_eeprom(int param_idx);
esp_err_t load_parameter_from_eeprom(int param_idx);
void store_parameter(int param_idx);
void load_parameter(int param_idx);
void store_all_parameters(void);
void load_all_parameters(void);

#endif // KEYBOARD_H