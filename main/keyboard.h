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

// Parameter storage types
typedef enum {
    STORAGE_NVS,
    STORAGE_RTC,
    STORAGE_EEPROM
} storage_type_t;

// Parameter types
typedef enum {
    PARAM_TYPE_NUMBER,
    PARAM_TYPE_DECIMAL,
    PARAM_TYPE_DATE,
    PARAM_TYPE_TIME,
    PARAM_TYPE_ENABLE_DISABLE,
    PARAM_TYPE_MULTIPLE,
    PARAM_TYPE_PASSWORD
} param_type_t;

// Parameter groups
typedef enum {
    GROUP_DATE_TIME,
    GROUP_PROTECTION,
    GROUP_STAGGERING,
    GROUP_CIVIL_TWILIGHT,
    GROUP_SYSTEM
} param_group_t;

// Parameter format types
typedef enum {
    FORMAT_NONE,
    FORMAT_ENABLE_DISABLE,
    FORMAT_DATE,
    FORMAT_TIME,
    FORMAT_DECIMAL,
    FORMAT_MULTIPLE
} param_format_t;

// Parameter addresses
#define PARAM_ADDRESS_TIME 1
#define PARAM_ADDRESS_DATE 2
#define PARAM_ADDRESS_3 3
#define PARAM_ADDRESS_4 4
#define PARAM_ADDRESS_5 5
#define PARAM_ADDRESS_6 6
#define PARAM_ADDRESS_7 7
#define PARAM_ADDRESS_8 8
#define PARAM_ADDRESS_9 9
#define PARAM_ADDRESS_10 10
#define PARAM_ADDRESS_11 11
#define PARAM_ADDRESS_12 12
#define PARAM_ADDRESS_13 13
#define PARAM_ADDRESS_14 14
#define PARAM_ADDRESS_15 15
#define PARAM_ADDRESS_16 16
#define PARAM_ADDRESS_17 17
#define PARAM_ADDRESS_18 18
#define PARAM_ADDRESS_19 19
#define PARAM_ADDRESS_20 20
#define PARAM_ADDRESS_21 21
#define PARAM_ADDRESS_22 22
#define PARAM_ADDRESS_23 23
#define PARAM_ADDRESS_24 24
#define PARAM_ADDRESS_25 25
#define PARAM_ADDRESS_26 26

// Format validation rules
typedef struct {
    int min_length;           // Minimum length of input
    int max_length;           // Maximum length of input
    param_format_t format;     // Changed from char* to param_format_t
    float min_value;          // Minimum value (for numeric types)
    float max_value;          // Maximum value (for numeric types)
    int decimal_places;       // Number of decimal places (for decimal types)
    bool allow_negative;      // Whether negative numbers are allowed
    int max_retries;          // Maximum number of retries (for password)
    int lockout_time;         // Lockout time in seconds after max retries
} param_validation_t;

// Parameter structure
typedef struct {
    const char *name;
    param_type_t type;
    param_group_t group;
    storage_type_t storage;
    int address;
    void *value;
    const char *default_value;
    void (*validate)(void *value);
    param_validation_t validation;  // New validation rules
} parameter_t;

// Add these before the function prototypes
#define NUM_PARAMETERS 26
#define NVS_NAMESPACE "params"

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
void validate_password(void *value);
void validate_decimal(void *value);

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