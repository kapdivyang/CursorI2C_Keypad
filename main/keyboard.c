#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/i2c.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#include "keyboard.h"
#include "lcd.h"

#define FORMAT_NONE 0
#define FORMAT_DECIMAL 1
#define FORMAT_DATE 2
#define FORMAT_TIME 3
#define FORMAT_ENABLE_DISABLE 4
#define FORMAT_MULTIPLE 5 // Added for multiple choice parameters

#define MAX_PASSWORD_RETRIES 3 // Maximum number of password retry attempts

// Forward declarations for static functions
static esp_err_t ds1307_init(void);
static uint8_t binary_to_bcd(uint8_t value);
static uint8_t bcd_to_binary(uint8_t value);
static esp_err_t ds1307_write(uint8_t reg_addr, uint8_t *data, size_t data_len);
static esp_err_t ds1307_read(uint8_t reg_addr, uint8_t *data, size_t data_len);
static esp_err_t eeprom_write(uint16_t addr, uint8_t *data, size_t data_len);
static esp_err_t eeprom_read(uint16_t addr, uint8_t *data, size_t data_len);
static esp_err_t write_pcf8574(uint8_t row_mask);
static uint8_t read_pcf8574(uint8_t row_mask);
static void format_input_according_to_rules(const char *input, char *output, const param_validation_t *rules);
static bool check_password(const char *entered_password);
static bool is_valid_date(const char *date_str);

// Define keypad layout
static const char keys[4][4] = {
    {'1', '2', '3', 'A'}, // Row 1
    {'4', '5', '6', 'B'}, // Row 2
    {'7', '8', '9', 'C'}, // Row 3
    {'*', '0', '#', 'D'}  // Row 4
};

// Define global variables for I2C
i2c_port_t keypad_i2c_port;
SemaphoreHandle_t i2c_semaphore;
// Add extern declaration for lcd_semaphore
extern SemaphoreHandle_t lcd_semaphore;
bool button_pressed = false;
TickType_t button_timer = 0;
char pressed_character[2] = {0};

// Add variables for inactivity timeout
static TickType_t last_activity_time = 0;
static const TickType_t INACTIVITY_TIMEOUT_MS = 15000; // 15 seconds timeout
static TickType_t lockout_start = 0;
static bool is_locked_out = false;

// Add this with the other global constants
// static const int MAX_PASSWORD_RETRIES = 3;

// Parameters array - must be defined before functions that use it
static parameter_t parameters[] = {
    // Time parameters
    {
        .name = "01.Time:",
        .type = PARAM_TYPE_TIME,
        .group = GROUP_DATE_TIME,
        .storage = STORAGE_RTC,
        .address = PARAM_ADDRESS_TIME,
        .value = NULL,
        .default_value = "0000",
        .validate = validate_time,
        .validation = {
            .min_length = 4,
            .max_length = 4,
            .format = FORMAT_TIME,
            .min_value = 0,
            .max_value = 2359,
            .decimal_places = 0,
            .allow_negative = false}},
    // Date parameters
    {.name = "02.Date:", .type = PARAM_TYPE_DATE, .group = GROUP_DATE_TIME, .storage = STORAGE_RTC, .address = PARAM_ADDRESS_DATE, .value = NULL, .default_value = "010123", .validate = validate_date, .validation = {.min_length = 6, .max_length = 6, .format = FORMAT_DATE, .min_value = 0, .max_value = 311299, .decimal_places = 0, .allow_negative = false}},
    // High Voltage parameter
    {.name = "03.Hi Volt:", .type = PARAM_TYPE_DECIMAL, .group = GROUP_PROTECTION, .storage = STORAGE_EEPROM, .address = PARAM_ADDRESS_3, .value = NULL, .default_value = "280.0", .validate = validate_decimal, .validation = {.min_length = 3, .max_length = 5, .format = FORMAT_DECIMAL, .min_value = 0.0, .max_value = 999.9, .decimal_places = 1, .allow_negative = false}},
    // Low Voltage parameter
    {.name = "04.Lo Volt:", .type = PARAM_TYPE_DECIMAL, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_4, .value = NULL, .default_value = "180.0", .validate = validate_decimal, .validation = {.min_length = 3, .max_length = 5, .format = FORMAT_DECIMAL, .min_value = 0.0, .max_value = 999.9, .decimal_places = 1, .allow_negative = false}},
    // R-Low A parameter
    {.name = "05.R-Low A:", .type = PARAM_TYPE_DECIMAL, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_5, .value = NULL, .default_value = "1.0", .validate = validate_decimal, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_DECIMAL, .min_value = 0.0, .max_value = 9.9, .decimal_places = 1, .allow_negative = false}},
    // Y-Low A parameter
    {.name = "06.Y-Low A:", .type = PARAM_TYPE_DECIMAL, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_6, .value = NULL, .default_value = "1.0", .validate = validate_decimal, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_DECIMAL, .min_value = 0.0, .max_value = 9.9, .decimal_places = 1, .allow_negative = false}},
    // B-Low A parameter
    {.name = "07.B-Low A:", .type = PARAM_TYPE_DECIMAL, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_7, .value = NULL, .default_value = "1.0", .validate = validate_decimal, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_DECIMAL, .min_value = 0.0, .max_value = 9.9, .decimal_places = 1, .allow_negative = false}},
    // OC % parameter
    {.name = "08.OC %:", .type = PARAM_TYPE_NUMBER, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_8, .value = NULL, .default_value = "25", .validate = validate_number, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_NONE, .min_value = 0, .max_value = 999, .decimal_places = 0, .allow_negative = false}},
    // Alarm parameter
    {.name = "09.Alarm:", .type = PARAM_TYPE_ENABLE_DISABLE, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_9, .value = NULL, .default_value = "0", .validate = validate_enable_disable, .validation = {.min_length = 1, .max_length = 1, .format = FORMAT_ENABLE_DISABLE, .min_value = 0, .max_value = 1, .decimal_places = 0, .allow_negative = false}},
    // Protection parameter
    {.name = "10.Protect:", .type = PARAM_TYPE_MULTIPLE, .group = GROUP_PROTECTION, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_10, .value = NULL, .default_value = "0", .validate = validate_multiple, .validation = {.min_length = 1, .max_length = 1, .format = FORMAT_MULTIPLE, .min_value = 0, .max_value = 3, .decimal_places = 0, .allow_negative = false}},
    // Rotate parameter
    {.name = "11.Rotate:", .type = PARAM_TYPE_ENABLE_DISABLE, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_11, .value = NULL, .default_value = "0", .validate = validate_enable_disable, .validation = {.min_length = 1, .max_length = 1, .format = FORMAT_ENABLE_DISABLE, .min_value = 0, .max_value = 1, .decimal_places = 0, .allow_negative = false}},
    // R On Time parameter
    {.name = "12.R On Tm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_12, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // Y On Time parameter
    {.name = "13.Y On Tm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_13, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // B On Time parameter
    {.name = "14.B On Tm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_14, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // R Off Time parameter
    {.name = "15.R OffTm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_15, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // Y Off Time parameter
    {.name = "16.Y OffTm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_16, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // B Off Time parameter
    {.name = "17.B OffTm:", .type = PARAM_TYPE_TIME, .group = GROUP_STAGGERING, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_17, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 2359, .decimal_places = 0, .allow_negative = false}},
    // Back Set parameter
    {.name = "18.BackSet:", .type = PARAM_TYPE_NUMBER, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_18, .value = NULL, .default_value = "0", .validate = validate_number, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_NONE, .min_value = -99, .max_value = 99, .decimal_places = 0, .allow_negative = true}},
    // Back Rise parameter
    {.name = "19.BackRise:", .type = PARAM_TYPE_NUMBER, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_19, .value = NULL, .default_value = "0", .validate = validate_number, .validation = {.min_length = 1, .max_length = 3, .format = FORMAT_NONE, .min_value = -99, .max_value = 99, .decimal_places = 0, .allow_negative = true}},
    // January Dusk parameter
    {.name = "20.JanDusk:", .type = PARAM_TYPE_TIME, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_20, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 99, .decimal_places = 0, .allow_negative = false}},
    // January Dawn parameter
    {.name = "21.JanDawn:", .type = PARAM_TYPE_TIME, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_21, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 99, .decimal_places = 0, .allow_negative = false}},
    // December Dusk parameter
    {.name = "22.DecDusk:", .type = PARAM_TYPE_TIME, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_22, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 99, .decimal_places = 0, .allow_negative = false}},
    // December Dawn parameter
    {.name = "23.DecDawn:", .type = PARAM_TYPE_TIME, .group = GROUP_CIVIL_TWILIGHT, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_23, .value = NULL, .default_value = "0000", .validate = validate_time, .validation = {.min_length = 4, .max_length = 4, .format = FORMAT_TIME, .min_value = 0, .max_value = 99, .decimal_places = 0, .allow_negative = false}},
    // Password parameter
    {.name = "24.Password:", .type = PARAM_TYPE_PASSWORD, .group = GROUP_SYSTEM, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_24, .value = NULL, .default_value = "00000000", .validate = validate_password, .validation = {.min_length = 8, .max_length = 8, .format = FORMAT_NONE, .min_value = 0, .max_value = 0, .decimal_places = 0, .allow_negative = false, .max_retries = 3, .lockout_time = 15}},
    // Password Enable/Disable parameter
    {.name = "25.PassED:", .type = PARAM_TYPE_ENABLE_DISABLE, .group = GROUP_SYSTEM, .storage = STORAGE_NVS, .address = PARAM_ADDRESS_25, .value = NULL, .default_value = "0", .validate = validate_enable_disable, .validation = {.min_length = 1, .max_length = 1, .format = FORMAT_ENABLE_DISABLE, .min_value = 0, .max_value = 1, .decimal_places = 0, .allow_negative = false}}};

#define NVS_NAMESPACE "params"

// I2C defines and flags
#define I2C_PORT I2C_NUM_0
#define PCF8574_ADDR 0x23 // Updated to match your test program
#define LCD_ADDR 0x27     // Matches your test program
#define LCD_ROWS 2
#define LCD_COLS 16

extern SemaphoreHandle_t lcd_semaphore;
extern bool in_keypad_mode;
extern bool in_keyboard_mode;

#define I2C_MASTER_SCL_IO 21      // Default ESP32 SDA
#define I2C_MASTER_SDA_IO 22      // Default ESP32 SCL
#define I2C_MASTER_FREQ_HZ 100000 // Reduce speed to 100kHz for better compatibility with DS1307

// Add a specific timeout for RTC operations
#define RTC_TIMEOUT_MS 50       // Shorter timeout for RTC detection
#define RTC_READ_TIMEOUT_MS 250 // Longer timeout for RTC operations

// static my_nvs_handle_t my_nvs_handle;
static nvs_handle_t my_nvs_handle;

// Add this global variable to indicate if the RTC is actually present
static bool rtc_present = false;
static uint8_t simulated_rtc_registers[8] = {0}; // Simulated RTC registers when hardware isn't available

// Add these global variables for password protection
static int password_retries = 0;
static bool is_authenticated = false;

// Add these global variables at the top of the file with other globals
// static TickType_t cursor_last_toggle_time = 0;
// static bool cursor_visible = true;
// static const TickType_t CURSOR_BLINK_INTERVAL_MS = 500; // Blink every 500ms

// Add a global flag to track validation status
static bool validation_failed = false;
static char validation_error_message[64] = {0};

// This new function will handle the validation logic
static bool is_valid_date(const char *date_str)
{
    if (strlen(date_str) != 6)
    {
        ESP_LOGE("Validation", "Invalid date length: %s", date_str);
        return false;
    }

    char day_str[3] = {date_str[0], date_str[1], '\0'};
    char month_str[3] = {date_str[2], date_str[3], '\0'};
    char year_str[3] = {date_str[4], date_str[5], '\0'};

    int day = atoi(day_str);
    int month = atoi(month_str);
    int year = atoi(year_str);

    // Basic range checks
    if (month < 1 || month > 12)
    {
        ESP_LOGE("Validation", "Invalid month: %d", month);
        return false;
    }
    if (day < 1)
    {
        ESP_LOGE("Validation", "Invalid day: %d", day);
        return false;
    }

    // Check days in month
    int max_days = 31;
    if (month == 4 || month == 6 || month == 9 || month == 11)
    {
        max_days = 30;
    }
    else if (month == 2)
    {
        // Simple leap year check for 20xx
        max_days = (year % 4 == 0) ? 29 : 28;
    }

    if (day > max_days)
    {
        ESP_LOGE("Validation", "Invalid day %d for month %d", day, month);
        return false;
    }

    return true;
}

// This matches the declaration in keyboard.h
void validate_date(void *value)
{
    char *date_str = (char *)value;
    if (!date_str)
        return;

    // Reset validation status
    validation_failed = false;
    validation_error_message[0] = '\0';

    // Special case for empty value
    if (strlen(date_str) == 0)
    {
        strcpy(date_str, "010123"); // Default to 01/01/23
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Empty date - using default");
        return;
    }

    // If not in DDMMYY format, assume raw input and try to parse it
    if (!is_valid_date(date_str))
    {
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Invalid date format");
                
        // Reset to default (01/01/23)
        strcpy(date_str, "010123");
        return;
    }

    // The date is valid, extract and check parts
    char day_str[3] = {date_str[0], date_str[1], '\0'};
    char month_str[3] = {date_str[2], date_str[3], '\0'};
    char year_str[3] = {date_str[4], date_str[5], '\0'};

    int day = atoi(day_str);
    int month = atoi(month_str);
    int year = atoi(year_str);

    // Additional validation for day/month ranges
    if (month < 1 || month > 12 || day < 1 || day > 31)
    {
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Day/month out of range");
                
        // Reset to default (01/01/23)
        strcpy(date_str, "010123");
        return;
    }

    // Specific month validation
    if ((month == 4 || month == 6 || month == 9 || month == 11) && day > 30)
    {
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Month %d has 30 days max", month);
                
        // Reset to default (01/01/23)
        strcpy(date_str, "010123");
        return;
    }
    else if (month == 2)
    {
        // February validation with leap year check
        bool leap_year = (year % 4 == 0); // Simplified leap year check
        if ((leap_year && day > 29) || (!leap_year && day > 28))
        {
            validation_failed = true;
            snprintf(validation_error_message, sizeof(validation_error_message), 
                    "Feb has %d days in 20%02d", leap_year ? 29 : 28, year);
                    
            // Reset to default (01/01/23)
            strcpy(date_str, "010123");
            return;
        }
    }

    // Date is valid, keep the original format
}

void format_date(const char *input, char *output, size_t output_size)
{
    if (!input || !output || output_size < 9)
    {
        ESP_LOGE("Format", "Invalid parameters for format_date");
        if (output && output_size > 0)
        {
            output[0] = '\0';
        }
        return;
    }

    // For partial inputs during editing, show as much as we have
    if (strlen(input) < 6)
    {
        // Format what we have so far in DD/MM/YY format
        char temp[7] = {0}; // DDMMYY + null terminator
        strncpy(temp, input, 6);

        if (strlen(input) >= 2)
        {
            snprintf(output, output_size, "%2.2s/", temp);
            if (strlen(input) >= 4)
            {
                snprintf(output + 3, output_size - 3, "%2.2s/", temp + 2);
                if (strlen(input) >= 5)
                {
                    snprintf(output + 6, output_size - 6, "%2.2s", temp + 4);
                }
            }
        }
        else if (strlen(input) > 0)
        {
            // Just show the digits entered so far
            snprintf(output, output_size, "%s", input);
        }
        else
        {
            output[0] = '\0';
        }
        return;
    }

    // Input is complete with DDMMYY format (6 digits)
    char day[3] = {input[0], input[1], '\0'};
    char month[3] = {input[2], input[3], '\0'};
    char year[3] = {input[4], input[5], '\0'};

    // Format as DD/MM/YY
    snprintf(output, output_size, "%s/%s/%s", day, month, year);
}

// Completely rewrite format_time to handle all cases
void format_time(char *input, char *output)
{
    // Handle empty input
    if (!input || strlen(input) == 0)
    {
        strcpy(output, "");
        return;
    }

    // Check if input already has the correct format with colon
    if (strchr(input, ':') != NULL)
    {
        strcpy(output, input);
        return;
    }

    // Format based on input length
    size_t len = strlen(input);
    
    if (len == 1)
    {
        // Single digit - just copy
        strcpy(output, input);
    }
    else if (len == 2)
    {
        // Two digits (hours only) - add colon
        output[0] = input[0];
        output[1] = input[1];
        output[2] = ':';
        output[3] = '\0';
    }
    else if (len == 3)
    {
        // Three digits (hours + one minute digit)
        output[0] = input[0];
        output[1] = input[1];
        output[2] = ':';
        output[3] = input[2];
        output[4] = '\0';
    }
    else if (len == 4)
    {
        // Four digits (HHMM format)
        output[0] = input[0];
        output[1] = input[1];
        output[2] = ':';
        output[3] = input[2];
        output[4] = input[3];
        output[5] = '\0';
    }
    else
    {
        // Unexpected format - just copy
        strcpy(output, input);
    }
}

void validate_time(void *value)
{
    char *time_str = (char *)value;
    if (!time_str)
        return;

    // Reset validation status
    validation_failed = false;
    validation_error_message[0] = '\0';

    int hour = 0, minute = 0;
    bool valid_format = false;

    // Check for HH:MM format
    if (strlen(time_str) == 5 && time_str[2] == ':')
    {
        if (sscanf(time_str, "%d:%d", &hour, &minute) == 2)
        {
            valid_format = true;
        }
    }
    // Check for HHMM format
    else if (strlen(time_str) == 4)
    {
        char hours[3] = {time_str[0], time_str[1], '\0'};
        char mins[3] = {time_str[2], time_str[3], '\0'};

        hour = atoi(hours);
        minute = atoi(mins);
        valid_format = true;
    }

    if (!valid_format || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        // Set validation error flag and message
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Invalid time format");
        
        // Reset to default
        strcpy(time_str, "00:00");
    }
}

void validate_number(void *value)
{
    char *num_str = (char *)value;
    if (!num_str)
        return;

    // Reset validation status
    validation_failed = false;
    validation_error_message[0] = '\0';

    // Find the parameter this value belongs to
    parameter_t *param = NULL;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].value == value)
        {
            param = &parameters[i];
            break;
        }
    }
    
    if (!param)
        return;

    int val = atoi(num_str);
    
    // Check range based on parameter validation rules
    if (val < param->validation.min_value || val > param->validation.max_value)
    {
        // Set validation error
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Range %d to %d", (int)param->validation.min_value, (int)param->validation.max_value);
                
        // Reset to default value
        strcpy(num_str, param->default_value);
    }
}

// Modified validate_enable_disable to show "Enable"/"Disable" instead of 0/1
void validate_enable_disable(void *value)
{
    char *val = (char *)value;
    if (!val)
        return;

    // If we're in edit mode and the value is a single digit
    if (strlen(val) == 1 && (val[0] == '0' || val[0] == '1'))
    {
        // Convert to text format
        strcpy(val, val[0] == '1' ? "Enable" : "Disable");
    }
    else if (strcmp(val, "Enable") != 0 && strcmp(val, "Disable") != 0)
    {
        // Invalid value, set to default
        strcpy(val, "Disable");
    }
}

void validate_multiple(void *value)
{
    char *val = (char *)value;
    int selection = atoi(val);
    switch (selection)
    {
    case 0:
        strcpy(val, "ALL ");
        break;
    case 1:
        strcpy(val, "VOLT");
        break;
    case 2:
        strcpy(val, "CURR");
        break;
    case 3:
        strcpy(val, "None");
        break;
    default:
        strcpy(val, "ALL ");
        break;
    }
}

// Initialize DS1307 RTC and set up simulated mode if needed
static esp_err_t ds1307_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t control_reg = 0;

    ESP_LOGI("RTC", "Initializing DS1307 RTC");

    // Start with assumption that RTC is not present
    rtc_present = false;

    // Initialize simulated registers with default values
    memset(simulated_rtc_registers, 0, sizeof(simulated_rtc_registers));
    // Set default time: 12:00:00, Monday, 01/01/2023
    simulated_rtc_registers[0] = 0;                 // seconds
    simulated_rtc_registers[1] = binary_to_bcd(0);  // minutes
    simulated_rtc_registers[2] = binary_to_bcd(12); // hours (12pm)
    simulated_rtc_registers[3] = 1;                 // day of week (1=Sunday)
    simulated_rtc_registers[4] = binary_to_bcd(1);  // date
    simulated_rtc_registers[5] = binary_to_bcd(1);  // month
    simulated_rtc_registers[6] = binary_to_bcd(23); // year (2023)

    // Try to take I2C semaphore with extended timeout
    if (xSemaphoreTake(i2c_semaphore, 200 / portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGE("RTC", "Failed to take I2C semaphore during initialization (timeout)");
        goto use_simulated;
    }

    do
    {
        // First check if we can communicate with the RTC at all
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        if (cmd == NULL)
        {
            ESP_LOGE("RTC", "Failed to create I2C command");
            ret = ESP_FAIL;
            break;
        }

        // Try to read the control register at address 0x07
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, 0x07, true); // Control register address
        i2c_master_stop(cmd);

        ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, RTC_TIMEOUT_MS / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret != ESP_OK)
        {
            ESP_LOGW("RTC", "DS1307 not detected on I2C bus: %s", esp_err_to_name(ret));
            break;
        }

        // Add a small delay to give the RTC time to process
        vTaskDelay(10 / portTICK_PERIOD_MS);

        // Now read the control register
        cmd = i2c_cmd_link_create();
        if (cmd == NULL)
        {
            ESP_LOGE("RTC", "Failed to create I2C read command");
            ret = ESP_FAIL;
            break;
        }

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_READ, true);
        i2c_master_read_byte(cmd, &control_reg, I2C_MASTER_NACK);
        i2c_master_stop(cmd);

        ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, RTC_TIMEOUT_MS / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret != ESP_OK)
        {
            ESP_LOGW("RTC", "Failed to read DS1307 control register: %s", esp_err_to_name(ret));
            break;
        }

        ESP_LOGI("RTC", "DS1307 detected on I2C bus, control register: 0x%02x", control_reg);

        // Now check if the clock is running by reading register 0 (seconds)
        uint8_t seconds_reg = 0;
        ret = ds1307_read(0x00, &seconds_reg, 1);

        if (ret != ESP_OK)
        {
            ESP_LOGW("RTC", "Failed to read DS1307 seconds register: %s", esp_err_to_name(ret));
            break;
        }

        // Check the clock halt (CH) bit
        if (seconds_reg & 0x80)
        {
            ESP_LOGW("RTC", "DS1307 clock is halted (CH bit set). Clearing bit and starting clock.");
            // Clear the CH bit
            seconds_reg &= ~0x80;
            ret = ds1307_write(0x00, &seconds_reg, 1);

            if (ret != ESP_OK)
            {
                ESP_LOGE("RTC", "Failed to start DS1307 clock: %s", esp_err_to_name(ret));
                break;
            }

            ESP_LOGI("RTC", "DS1307 clock started successfully");
        }
        else
        {
            ESP_LOGI("RTC", "DS1307 clock is running normally");
        }

        // Try to read all time/date registers to make sure everything is working
        uint8_t rtc_registers[7];
        ret = ds1307_read(0x00, rtc_registers, 7);

        if (ret != ESP_OK)
        {
            ESP_LOGW("RTC", "Failed to read all DS1307 registers: %s", esp_err_to_name(ret));
            break;
        }

        // If we got here, RTC is working
        rtc_present = true;
        ESP_LOGI("RTC", "DS1307 RTC initialized successfully, using hardware RTC");

        // Log current time and date from RTC
        if (rtc_present)
        {
            ESP_LOGI("RTC", "Current time: %02d:%02d:%02d, Date: %02d/%02d/20%02d, Day: %d",
                     bcd_to_binary(rtc_registers[2]),        // Hours
                     bcd_to_binary(rtc_registers[1]),        // Minutes
                     bcd_to_binary(rtc_registers[0] & 0x7F), // Seconds (mask out CH bit)
                     bcd_to_binary(rtc_registers[5]),        // Month
                     bcd_to_binary(rtc_registers[4]),        // Day
                     bcd_to_binary(rtc_registers[6]),        // Year
                     rtc_registers[3]);                      // Day of week
        }

    } while (0);

    // Release the I2C semaphore
    xSemaphoreGive(i2c_semaphore);

use_simulated:
    // If RTC not present or any operation failed, use simulated mode
    if (!rtc_present)
    {
        ESP_LOGW("RTC", "Using simulated RTC mode");
    }

    return ESP_OK; // Always return OK as we fall back to simulated mode
}

// Helper functions for BCD conversion used by the DS1307 RTC
static uint8_t binary_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0F);
}

// Function to write data to DS1307 RTC - improved error handling
static esp_err_t ds1307_write(uint8_t reg_addr, uint8_t *data, size_t data_len)
{
    // If RTC is not present, just update our simulated registers and return success
    if (!rtc_present)
    {
        ESP_LOGD("RTC", "Using simulated RTC (write)");
        for (size_t i = 0; i < data_len && (reg_addr + i) < sizeof(simulated_rtc_registers); i++)
        {
            simulated_rtc_registers[reg_addr + i] = data[i];
        }
        return ESP_OK;
    }

    ESP_LOGD("RTC", "Writing to DS1307 reg 0x%02x, length %d", reg_addr, data_len);

    if (xSemaphoreTake(i2c_semaphore, 200 / portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGE("RTC", "Failed to take I2C semaphore (timeout)");
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL)
    {
        ESP_LOGE("RTC", "Failed to create I2C command");
        xSemaphoreGive(i2c_semaphore);
        return ESP_FAIL;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write(cmd, data, data_len, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, RTC_READ_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_semaphore);

    if (ret != ESP_OK)
    {
        ESP_LOGE("RTC", "Failed to write to DS1307 (reg 0x%02x): %s", reg_addr, esp_err_to_name(ret));
        // If we get a timeout or other error, assume RTC is not working properly
        // and switch to simulated mode for future operations
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_FAIL)
        {
            ESP_LOGW("RTC", "Switching to simulated RTC mode due to write failure");
            rtc_present = false;
        }
    }
    else
    {
        ESP_LOGD("RTC", "Successfully wrote to DS1307 reg 0x%02x", reg_addr);
    }
    return ret;
}

// Function to read data from DS1307 RTC - improved error handling
static esp_err_t ds1307_read(uint8_t reg_addr, uint8_t *data, size_t data_len)
{
    // If RTC is not present, just return our simulated register values
    if (!rtc_present)
    {
        ESP_LOGD("RTC", "Using simulated RTC (read)");
        for (size_t i = 0; i < data_len && (reg_addr + i) < sizeof(simulated_rtc_registers); i++)
        {
            data[i] = simulated_rtc_registers[reg_addr + i];
        }
        // Initialize simulated registers with default values if they're reading default zeros
        if (reg_addr == 0 && data_len >= 7)
        {
            // If time registers are all zeros, initialize with a default time/date
            bool all_zero = true;
            for (int i = 0; i < 7; i++)
            {
                if (data[i] != 0)
                {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero)
            {
                // Set default time: 12:00:00, Monday, 01/01/2023
                data[0] = 0;                 // seconds
                data[1] = binary_to_bcd(0);  // minutes
                data[2] = binary_to_bcd(12); // hours (12pm)
                data[3] = 1;                 // day of week (1=Sunday)
                data[4] = binary_to_bcd(1);  // date
                data[5] = binary_to_bcd(1);  // month
                data[6] = binary_to_bcd(23); // year (2023)

                // Update the simulated registers
                for (int i = 0; i < 7; i++)
                {
                    simulated_rtc_registers[i] = data[i];
                }

                ESP_LOGD("RTC", "Initialized simulated RTC with default values");
            }
        }
        return ESP_OK;
    }

    ESP_LOGD("RTC", "Reading from DS1307 reg 0x%02x, length %d", reg_addr, data_len);

    if (xSemaphoreTake(i2c_semaphore, 200 / portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGE("RTC", "Failed to take I2C semaphore (timeout)");
        return ESP_ERR_TIMEOUT;
    }

    // First set the register address
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL)
    {
        ESP_LOGE("RTC", "Failed to create I2C command");
        xSemaphoreGive(i2c_semaphore);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;

    do
    {
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_WRITE, true);
        i2c_master_write_byte(cmd, reg_addr, true);
        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, RTC_READ_TIMEOUT_MS / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret != ESP_OK)
        {
            ESP_LOGE("RTC", "Failed to set register address 0x%02x: %s", reg_addr, esp_err_to_name(ret));
            break;
        }

        // Add a small delay before reading
        vTaskDelay(10 / portTICK_PERIOD_MS);

        // Then read the data
        cmd = i2c_cmd_link_create();
        if (cmd == NULL)
        {
            ESP_LOGE("RTC", "Failed to create I2C read command");
            ret = ESP_FAIL;
            break;
        }

        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (DS1307_ADDR << 1) | I2C_MASTER_READ, true);

        if (data_len > 1)
        {
            i2c_master_read(cmd, data, data_len - 1, I2C_MASTER_ACK);
        }
        i2c_master_read_byte(cmd, data + data_len - 1, I2C_MASTER_NACK);

        i2c_master_stop(cmd);
        ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, RTC_READ_TIMEOUT_MS / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);

        if (ret != ESP_OK)
        {
            ESP_LOGE("RTC", "Failed to read from DS1307 (reg 0x%02x): %s", reg_addr, esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGD("RTC", "Successfully read from DS1307 reg 0x%02x", reg_addr);
            if (data_len == 1)
            {
                ESP_LOGD("RTC", "Data: 0x%02x", data[0]);
            }
        }
    } while (0);

    xSemaphoreGive(i2c_semaphore);

    // If we get a timeout or other error, assume RTC is not working properly
    // and switch to simulated mode for future operations
    if (ret == ESP_ERR_TIMEOUT || ret == ESP_FAIL)
    {
        ESP_LOGW("RTC", "Switching to simulated RTC mode due to read failure");
        rtc_present = false;

        // Provide simulated data anyway
        for (size_t i = 0; i < data_len && (reg_addr + i) < sizeof(simulated_rtc_registers); i++)
        {
            data[i] = simulated_rtc_registers[reg_addr + i];
        }

        // Initialize default values if needed
        if (reg_addr == 0 && data_len >= 7)
        {
            data[0] = 0;                 // seconds
            data[1] = binary_to_bcd(0);  // minutes
            data[2] = binary_to_bcd(12); // hours (12pm)
            data[3] = 1;                 // day of week (1=Sunday)
            data[4] = binary_to_bcd(1);  // date
            data[5] = binary_to_bcd(1);  // month
            data[6] = binary_to_bcd(23); // year (2023)

            // Update the simulated registers
            for (int i = 0; i < 7; i++)
            {
                simulated_rtc_registers[i] = data[i];
            }
        }

        return ESP_OK; // Return OK even though we failed, as we're now in simulated mode
    }

    return ret;
}

// Store a parameter to RTC (DS1307)
esp_err_t store_parameter_to_rtc(int param_idx)
{
    if (param_idx < 0 || param_idx >= NUM_PARAMETERS || parameters[param_idx].value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI("Storage", "Storing parameter %s to %s RTC", parameters[param_idx].name, rtc_present ? "hardware" : "simulated");

    // if (strcmp(parameters[param_idx].name, "Date:") == 0)
    if (parameters[param_idx].address == PARAM_ADDRESS_DATE)
    {
        const char *date_str = (const char *)parameters[param_idx].value;

        // Validate date format first
        if (!is_valid_date(date_str))
        {
            ESP_LOGE("Storage", "Invalid date format: %s", date_str);
            return ESP_ERR_INVALID_ARG;
        }

        // Extract day, month, year from DDMMYY format
        char day_str[3] = {date_str[0], date_str[1], '\0'};
        char month_str[3] = {date_str[2], date_str[3], '\0'};
        char year_str[3] = {date_str[4], date_str[5], '\0'};

        uint8_t day = binary_to_bcd(atoi(day_str));
        uint8_t month = binary_to_bcd(atoi(month_str));
        uint8_t year = binary_to_bcd(atoi(year_str));

        // Store in RTC registers
        if (rtc_present)
        {
            esp_err_t ret;
            if ((ret = ds1307_write(4, &day, 1)) != ESP_OK ||
                (ret = ds1307_write(5, &month, 1)) != ESP_OK ||
                (ret = ds1307_write(6, &year, 1)) != ESP_OK)
            {
                ESP_LOGE("Storage", "Failed to write date to RTC: %d", ret);
                return ret;
            }
        }
        else
        {
            simulated_rtc_registers[4] = day;
            simulated_rtc_registers[5] = month;
            simulated_rtc_registers[6] = year;
        }

        ESP_LOGI("Storage", "Stored date: %02d/%02d/%02d", atoi(day_str), atoi(month_str), atoi(year_str));
        return ESP_OK;
    }
    // else if (strcmp(parameters[param_idx].name, "Time:") == 0)
    else if (parameters[param_idx].address == PARAM_ADDRESS_TIME)
    {
        // Parse time string (could be either HH:MM or HHMM format)
        char *time_str = parameters[param_idx].value;
        int hour = 0, minute = 0;
        bool valid_format = false;

        // Try HH:MM format first
        if (strlen(time_str) == 5 && time_str[2] == ':')
        {
            if (sscanf(time_str, "%d:%d", &hour, &minute) == 2)
            {
                valid_format = true;
            }
        }
        // Try HHMM format (without colon)
        else if (strlen(time_str) == 4)
        {
            char hours[3] = {time_str[0], time_str[1], '\0'};
            char mins[3] = {time_str[2], time_str[3], '\0'};

            hour = atoi(hours);
            minute = atoi(mins);
            valid_format = true;
        }

        if (valid_format && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59)
        {
            // Update the time registers
            uint8_t time_data[2] = {binary_to_bcd(minute), binary_to_bcd(hour)};

            if (rtc_present)
            {
                esp_err_t ret = ds1307_write(1, time_data, 2);
                if (ret != ESP_OK)
                {
                    ESP_LOGE("RTC", "Failed to update time: %s", esp_err_to_name(ret));
                    return ret;
                }
            }
            else
            {
                simulated_rtc_registers[1] = time_data[0]; // minutes
                simulated_rtc_registers[2] = time_data[1]; // hours
            }

            // Ensure time is stored in standard HH:MM format
            if (strlen(time_str) != 5 || time_str[2] != ':')
            {
                sprintf(time_str, "%02d:%02d", hour, minute);
            }

            ESP_LOGI("RTC", "Updated time to %02d:%02d", hour, minute);
            return ESP_OK;
        }
        else
        {
            ESP_LOGE("RTC", "Invalid time format: %s", time_str);
            return ESP_ERR_INVALID_ARG;
        }
    }
    else
    {
        ESP_LOGE("RTC", "Unknown RTC parameter: %s", parameters[param_idx].name);
        return ESP_ERR_INVALID_ARG;
    }

    // This return is needed to avoid compiler warning
    return ESP_OK;
}

// Load a parameter from RTC (DS1307)
esp_err_t load_parameter_from_rtc(int param_idx)
{
    if (param_idx < 0 || param_idx >= NUM_PARAMETERS)
    {
        ESP_LOGE("Storage", "Invalid parameter index: %d", param_idx);
        return ESP_ERR_INVALID_ARG;
    }

    if (parameters[param_idx].storage != STORAGE_RTC)
    {
        ESP_LOGE("Storage", "Parameter %s is not stored in RTC", parameters[param_idx].name);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI("Storage", "Loading parameter %s from %s RTC",
             parameters[param_idx].name,
             rtc_present ? "hardware" : "simulated");

    uint8_t rtc_registers[8];
    esp_err_t ret = ds1307_read(0x00, rtc_registers, 7);
    if (ret != ESP_OK && rtc_present)
    {
        ESP_LOGE("RTC", "Failed to read from RTC: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check if clock halt bit is set
    if (rtc_registers[0] & 0x80)
    {
        ESP_LOGW("RTC", "RTC clock is halted (CH bit set)");
        // Clear the bit and restart the clock
        rtc_registers[0] &= ~0x80;
        esp_err_t write_ret = ds1307_write(0x00, rtc_registers, 1);
        if (write_ret != ESP_OK && rtc_present)
        {
            ESP_LOGE("RTC", "Failed to restart RTC clock: %s", esp_err_to_name(write_ret));
        }
        else
        {
            ESP_LOGI("RTC", "RTC clock restarted");
        }
    }

    // Free the old parameter value if it exists
    if (parameters[param_idx].value != NULL)
    {
        free(parameters[param_idx].value);
        parameters[param_idx].value = NULL;
    }
    // Load date from RTC
    if (parameters[param_idx].address == PARAM_ADDRESS_DATE)
    {
        // if (strcmp(parameters[param_idx].name, "2.Date:") == 0) {
        uint8_t day = bcd_to_binary(rtc_registers[4]);
        uint8_t month = bcd_to_binary(rtc_registers[5]);
        uint8_t year = bcd_to_binary(rtc_registers[6]);

        // Validate date values
        if (month < 1 || month > 12 || day < 1 || day > 31)
        {
            ESP_LOGW("RTC", "Invalid date values read from RTC: %02d/%02d/%02d", day, month, year);
            if (parameters[param_idx].value != NULL)
            {
                free(parameters[param_idx].value);
            }
            parameters[param_idx].value = strdup("010123");
        }
        else
        {
            // Allocate 8 bytes: 6 for DDMMYY format + 1 for null terminator + 1 extra for safety
            char *date_str = malloc(8);
            if (date_str == NULL)
            {
                ESP_LOGE("RTC", "Failed to allocate memory for date string");
                return ESP_ERR_NO_MEM;
            }

            // Store in DDMMYY format - ensure we have enough space for the format string
            snprintf(date_str, 8, "%02d%02d%02d", day, month, year);

            // Free old value if it exists
            if (parameters[param_idx].value != NULL)
            {
                free(parameters[param_idx].value);
            }
            parameters[param_idx].value = date_str;

            // Log in DD/MM/YY format for debugging
            ESP_LOGI("RTC", "Loaded date: %02d/%02d/%02d", day, month, year);
        }
    }
    // else if (strcmp(parameters[param_idx].name, "1.Time:") == 0)
    // Load time from RTC
    else if (parameters[param_idx].address == PARAM_ADDRESS_TIME)
    {
        // Get time from RTC: HH:MM format
        uint8_t hour = bcd_to_binary(rtc_registers[2]);
        uint8_t minute = bcd_to_binary(rtc_registers[1]);

        // Validate time values
        if (hour > 23 || minute > 59)
        {
            ESP_LOGW("RTC", "Invalid time values read from RTC: %02d:%02d", hour, minute);
            parameters[param_idx].value = strdup("00:00"); // Default time
        }
        else
        {
            char time_str[16];
            snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
            parameters[param_idx].value = strdup(time_str);
            ESP_LOGI("RTC", "Loaded time: %s", time_str);
        }
    }
    else
    {
        ESP_LOGE("RTC", "Unknown RTC parameter: %s", parameters[param_idx].name);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

// Define EEPROM addresses for different parameters
#define EEPROM_HI_VOLT_ADDR 0

// Store a parameter to EEPROM (24C32)
esp_err_t store_parameter_to_eeprom(int param_idx)
{
    if (param_idx < 0 || param_idx >= NUM_PARAMETERS || parameters[param_idx].value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI("Storage", "Storing parameter %s to EEPROM", parameters[param_idx].name);

    int address = parameters[param_idx].address;
    const char *value = (const char *)parameters[param_idx].value;
    int value_len = strlen(value) + 1; // Include null terminator

    return eeprom_write(address, (uint8_t *)value, value_len);
}

// Load a parameter from EEPROM (24C32)
esp_err_t load_parameter_from_eeprom(int param_idx)
{
    if (param_idx < 0 || param_idx >= NUM_PARAMETERS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI("Storage", "Loading parameter %s from EEPROM", parameters[param_idx].name);

    int address = parameters[param_idx].address;

    // Allocate memory for parameter value if not already allocated
    if (parameters[param_idx].value == NULL)
    {
        parameters[param_idx].value = malloc(32);
        if (parameters[param_idx].value == NULL)
        {
            ESP_LOGE("Storage", "Failed to allocate memory for parameter value");
            return ESP_ERR_NO_MEM;
        }
    }

    // Read value from EEPROM
    esp_err_t ret = eeprom_read(address, (uint8_t *)parameters[param_idx].value, 32);

    // If read fails, use default value
    if (ret != ESP_OK)
    {
        strcpy((char *)parameters[param_idx].value, parameters[param_idx].default_value);
        ESP_LOGW("Storage", "Using default value for parameter %s", parameters[param_idx].name);
    }

    // Validate the parameter value
    if (parameters[param_idx].validate != NULL)
    {
        parameters[param_idx].validate(parameters[param_idx].value);
    }

    return ESP_OK;
}

// Store a parameter to its designated storage
void store_parameter(int param_idx)
{
    switch (parameters[param_idx].storage)
    {
    case STORAGE_NVS:
        // In this case, we'll store it later in batch
        break;
    case STORAGE_RTC:
        store_parameter_to_rtc(param_idx);
        break;
    case STORAGE_EEPROM:
        store_parameter_to_eeprom(param_idx);
        break;
    }
}

// Load a parameter from its designated storage
void load_parameter(int param_idx)
{
    esp_err_t ret = ESP_OK;

    switch (parameters[param_idx].storage)
    {
    case STORAGE_NVS:
        // Will be loaded in batch later
        break;
    case STORAGE_RTC:
        ret = load_parameter_from_rtc(param_idx);
        if (ret != ESP_OK)
        {
            ESP_LOGE("Storage", "Failed to load from RTC, using default");
            parameters[param_idx].value = strdup((char *)parameters[param_idx].default_value);
            parameters[param_idx].validate(parameters[param_idx].value);
        }
        break;
    case STORAGE_EEPROM:
        ret = load_parameter_from_eeprom(param_idx);
        if (ret != ESP_OK)
        {
            ESP_LOGE("Storage", "Failed to load from EEPROM, using default");
            parameters[param_idx].value = strdup((char *)parameters[param_idx].default_value);
            parameters[param_idx].validate(parameters[param_idx].value);
        }
        break;
    }
}

// Store all parameters to their respective storage
void store_all_parameters(void)
{
    // First, count NVS parameters so we can batch them
    int nvs_count = 0;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].storage == STORAGE_NVS)
        {
            nvs_count++;
        }
        else
        {
            // Store non-NVS parameters individually
            store_parameter(i);
        }
    }

    // If we have NVS parameters, batch store them
    if (nvs_count > 0)
    {
        nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_nvs_handle);
        for (int i = 0; i < NUM_PARAMETERS; i++)
        {
            if (parameters[i].storage == STORAGE_NVS)
            {
                nvs_set_str(my_nvs_handle, parameters[i].name, (char *)parameters[i].value);
            }
        }
        nvs_commit(my_nvs_handle);
        nvs_close(my_nvs_handle);
    }
}

// Fix the load_all_parameters function to prioritize stored values over defaults
void load_all_parameters(void)
{
    // First load parameters from special storage (RTC, EEPROM)
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].storage != STORAGE_NVS)
        {
            load_parameter(i);
        }
    }

    // Then batch load from NVS
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_nvs_handle);
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].storage == STORAGE_NVS)
        {
            size_t len = 16;
            char *value = malloc(len);
            esp_err_t err = nvs_get_str(my_nvs_handle, parameters[i].name, value, &len);
            
            if (err == ESP_OK)
            {
                // Successfully loaded from NVS
                parameters[i].value = value;
                if (parameters[i].validate != NULL) {
                    parameters[i].validate(value);
                }
                ESP_LOGI("Keypad", "Loaded %s: %s from NVS", parameters[i].name, value);
            }
            else
            {
                // Failed to load from NVS, use default and store it
                free(value); // Free the allocated memory since we're not using it
                parameters[i].value = strdup((char *)parameters[i].default_value);
                if (parameters[i].validate != NULL) {
                    parameters[i].validate(parameters[i].value);
                }
                
                // Store the default value to NVS
                nvs_set_str(my_nvs_handle, parameters[i].name, parameters[i].value);
                ESP_LOGI("Keypad", "Error loading from NVS. Default value %s: %s", 
                         parameters[i].name, (char *)parameters[i].default_value);
            }
        }
    }
    nvs_commit(my_nvs_handle);
    nvs_close(my_nvs_handle);
}

// Replace the old store_parameters_to_nvs with store_all_parameters
void store_parameters_to_nvs(void)
{
    store_all_parameters();
}

// Replace the old load_parameters_from_nvs with load_all_parameters
void load_parameters_from_nvs(void)
{
    load_all_parameters();
}

char keypad_scan(void)
{
    uint8_t row_data[4];
    char key = '\0';

    // Only scan if no button is currently pressed (debounce)
    if (!button_pressed)
    {
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
        for (int row = 0; row < 4; row++)
        {
            if (row_data[row] == 0xFF)
                continue; // Skip if I2C read failed

            // Match the patterns from Arduino code
            switch (row_data[row])
            {
            // Row 1 (P0 low)
            case 238:
                key = keys[0][0];
                break; // 1110 1110 - '1'
            case 222:
                key = keys[0][1];
                break; // 1101 1110 - '2'
            case 190:
                key = keys[0][2];
                break; // 1011 1110 - '3'
            case 126:
                key = keys[0][3];
                break; // 0111 1110 - 'A'

            // Row 2 (P1 low)
            case 237:
                key = keys[1][0];
                break; // 1110 1101 - '4'
            case 221:
                key = keys[1][1];
                break; // 1101 1101 - '5'
            case 189:
                key = keys[1][2];
                break; // 1011 1101 - '6'
            case 125:
                key = keys[1][3];
                break; // 0111 1101 - 'B'

            // Row 3 (P2 low)
            case 235:
                key = keys[2][0];
                break; // 1110 1011 - '7'
            case 219:
                key = keys[2][1];
                break; // 1101 1011 - '8'
            case 187:
                key = keys[2][2];
                break; // 1011 1011 - '9'
            case 123:
                key = keys[2][3];
                break; // 0111 1011 - 'C'

            // Row 4 (P3 low)
            case 231:
                key = keys[3][0];
                break; // 1110 0111 - '*'
            case 215:
                key = keys[3][1];
                break; // 1101 0111 - '0'
            case 183:
                key = keys[3][2];
                break; // 1011 0111 - '#'
            case 119:
                key = keys[3][3];
                break; // 0111 0111 - 'D'
            }

            if (key != '\0')
            {
                if (!button_pressed)
                {
                    button_pressed = true;
                    button_timer = xTaskGetTickCount();
                    pressed_character[0] = key;
                    pressed_character[1] = '\0';
                    ESP_LOGI("Keypad", "Detected '%c' (Raw: 0x%02X)", key, row_data[row]);
                    return key;
                }
            }
        }
    }
    else
    {
        // Handle debounce timeout
        if ((xTaskGetTickCount() - button_timer) > (DEBOUNCE_DELAY_MS / portTICK_PERIOD_MS))
        {
            button_timer = xTaskGetTickCount();
            button_pressed = false;
            pressed_character[0] = '\0'; // Reset pressed character
        }
    }

    return '\0';
}

esp_err_t keypad_init(i2c_port_t i2c_port)
{
    keypad_i2c_port = i2c_port;
    i2c_semaphore = xSemaphoreCreateMutex();
    if (i2c_semaphore == NULL)
    {
        ESP_LOGE("Keypad", "Failed to create I2C semaphore");
        return ESP_FAIL;
    }
    ESP_LOGI("Keypad", "Initialized keypad on I2C port %d, address 0x%02X", i2c_port, PCF8574_ADDR);

    // Initialize DS1307 RTC
    esp_err_t rtc_init_result = ds1307_init();
    if (rtc_init_result != ESP_OK)
    {
        ESP_LOGW("Keypad", "Failed to initialize DS1307 RTC: %s", esp_err_to_name(rtc_init_result));
        // Continue anyway, as we'll fall back to default values
    }

    return ESP_OK;
}

// Add a refresh_rtc_time function
static void refresh_rtc_time(void)
{
    // Find the time parameter index
    int time_param_idx = -1;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].address == PARAM_ADDRESS_TIME)
        {
            time_param_idx = i;
            break;
        }
    }

    if (time_param_idx == -1)
    {
        ESP_LOGE("RTC", "Time parameter not found");
        return;
    }

    // Free old time value if it exists
    if (parameters[time_param_idx].value != NULL)
    {
        free(parameters[time_param_idx].value);
        parameters[time_param_idx].value = NULL;
    }

    // Read current time from RTC
    uint8_t rtc_registers[8];
    esp_err_t ret = ds1307_read(0x00, rtc_registers, 7);
    if (ret != ESP_OK && rtc_present)
    {
        ESP_LOGE("RTC", "Failed to read from RTC: %s", esp_err_to_name(ret));
        parameters[time_param_idx].value = strdup("00:00"); // Default time
        return;
    }

    // Get time from RTC: HH:MM format
    uint8_t hour = bcd_to_binary(rtc_registers[2]);
    uint8_t minute = bcd_to_binary(rtc_registers[1]);

    // Validate time values
    if (hour > 23 || minute > 59)
    {
        ESP_LOGW("RTC", "Invalid time values read from RTC: %02d:%02d", hour, minute);
        parameters[time_param_idx].value = strdup("00:00"); // Default time
    }
    else
    {
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
        parameters[time_param_idx].value = strdup(time_str);
        ESP_LOGI("RTC", "Refreshed time: %s", time_str);
    }
}

void keyboard_task(void *pvParameters)
{
    load_all_parameters();
    char input[16] = {0};
    int param_idx = 0;
    int input_pos = 0;
    bool password_mode = false;

    // Initialize last activity time
    last_activity_time = xTaskGetTickCount();
    // cursor_last_toggle_time = xTaskGetTickCount(); // Not needed anymore

    // This variable is used throughout the function to track semaphore state
    volatile bool semaphore_taken __attribute__((unused)) = false;

    while (1)
    {
        char key = keypad_scan();
        TickType_t current_time = xTaskGetTickCount();

        // Remove the software cursor blink timing check
        // Check for inactivity timeout
        if (in_keyboard_mode &&
            ((current_time - last_activity_time) * portTICK_PERIOD_MS >= INACTIVITY_TIMEOUT_MS))
        {
            // Timeout occurred - exit keyboard mode
            in_keyboard_mode = false;
            is_authenticated = false;
            password_mode = false;
            is_locked_out = false;

            if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
            {
                semaphore_taken = true;
                lcd_cursor_show(false); // Turn off cursor when exiting
                lcd_clear();
                lcd_set_cursor(0, 0);
                lcd_print("Timeout");
                lcd_set_cursor(1, 0);
                lcd_print("Returning to main");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                xSemaphoreGive(lcd_semaphore);
                semaphore_taken = false;
            }

            input_pos = 0;
            memset(input, 0, sizeof(input));

            // Reset last activity time
            last_activity_time = xTaskGetTickCount();
            continue;
        }

        // Handle lockout timer display updates
        if (in_keyboard_mode && password_mode && is_locked_out)
        {
            TickType_t elapsed_seconds = ((current_time - lockout_start) * portTICK_PERIOD_MS) / 1000;
            int remaining = parameters[23].validation.lockout_time - elapsed_seconds;

            if (remaining <= 0)
            {
                // Lockout period is over
                is_locked_out = false;
                password_retries = 0;

                if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
                {
                    semaphore_taken = true;
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("Lockout ended");
                    vTaskDelay(1000 / portTICK_PERIOD_MS);

                    // Return to password entry screen
                    lcd_clear();
                    lcd_set_cursor(0, 0);
                    lcd_print("Enter Password:");
                    lcd_set_cursor(1, 0);
                    lcd_print(">");
                    xSemaphoreGive(lcd_semaphore);
                    semaphore_taken = false;
                }
            }
            else if (remaining % 1 == 0)
            { // Update every second
                // Update the countdown timer
                if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
                {
                    semaphore_taken = true;
                    // Only update the remaining time display
                    lcd_set_cursor(0, 0);
                    lcd_print("Locked: %ds     ", remaining);
                    xSemaphoreGive(lcd_semaphore);
                    semaphore_taken = false;
                }
            }
        }

        if (key != '\0')
        {
            // Update last activity time when a key is pressed
            last_activity_time = xTaskGetTickCount();

            if (!in_keyboard_mode && key == 'A')
            {
                // Check if password protection is enabled - fixed to use parameter 24 (PassED)
                bool password_enabled = false;

                // Find parameter 24.PassED
                for (int i = 0; i < NUM_PARAMETERS; i++)
                {
                    if (strstr(parameters[i].name, "PassED") != NULL)
                    {
                        // Check if password is enabled (value is "1" or "Enable")
                        if (parameters[i].value != NULL &&
                            (strcmp(parameters[i].value, "1") == 0 ||
                             strcmp(parameters[i].value, "Enable") == 0))
                        {
                            password_enabled = true;
                        }
                        break;
                    }
                }

                in_keyboard_mode = true;
                password_mode = password_enabled;

                if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
                {
                    semaphore_taken = true;
                    lcd_clear();

                    if (password_enabled)
                    {
                        lcd_set_cursor(0, 0);
                        lcd_print("Enter Password:");
                        lcd_set_cursor(1, 0);
                        lcd_print(">");
                        lcd_set_cursor(1, 1); // Position cursor after ">"
                        lcd_cursor_show(true); // Show cursor for password entry
                        lcd_cursor_blink(true); // Enable blinking
                    }
                    else
                    {
                        is_authenticated = true;
                        param_idx = 0;
                        
                        // Refresh RTC time if showing time parameter (when first entering)
                        if (parameters[param_idx].address == PARAM_ADDRESS_TIME)
                        {
                            refresh_rtc_time();
                        }
                        
                        lcd_set_cursor(0, 0);
                        lcd_print("%s", parameters[param_idx].name);
                        lcd_set_cursor(1, 0);

                        // Format and display the current value
                        if (parameters[param_idx].value != NULL)
                        {
                            char formatted_output[32] = {0};
                            format_input_according_to_rules(
                                (char *)parameters[param_idx].value,
                                formatted_output,
                                &parameters[param_idx].validation);
                            lcd_print("Val: %s", formatted_output);
                            
                            // Don't show cursor yet as we're not editing
                            lcd_cursor_show(false);
                        }
                        else
                        {
                            lcd_print("Val: <none>");
                            lcd_cursor_show(false);
                        }
                    }

                    xSemaphoreGive(lcd_semaphore);
                    semaphore_taken = false;
                }
            }
            else if (in_keyboard_mode)
            {
                if (password_mode && !is_authenticated)
                {
                    // Handle password mode
                    if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
                    {
                        semaphore_taken = true;

                        if (is_locked_out)
                        {
                            // Turn off cursor during lockout
                            lcd_cursor_show(false);
                            
                            // Display lockout message and countdown
                            TickType_t current_time = xTaskGetTickCount();
                            int elapsed_seconds = ((current_time - lockout_start) * portTICK_PERIOD_MS) / 1000;
                            int remaining = parameters[23].validation.lockout_time - elapsed_seconds;

                            lcd_clear();
                            lcd_set_cursor(0, 0);
                            lcd_print("Locked: %ds", remaining);
                            lcd_set_cursor(1, 0);
                            lcd_print("Please wait...");
                        }
                        else if (key >= '0' && key <= '9')
                        {
                            // Handle digit entry for password
                            if (input_pos < 8) // Assume max password length is 8
                            {
                                input[input_pos++] = key;
                                input[input_pos] = '\0';

                                // Update display with asterisks for password
                                lcd_set_cursor(1, 0);
                                lcd_print(">%s", input);
                                
                                // Position cursor for next input
                                lcd_set_cursor(1, input_pos + 1); // +1 for the '>' character
                            }
                        }
                        else if (key == 'D') // Delete
                        {
                            if (input_pos > 0)
                            {
                                input[--input_pos] = '\0';
                                
                                // Update display
                                lcd_set_cursor(1, 0);
                                lcd_print(">%s ", input); // Space to clear last character
                                
                                // Position cursor for next input
                                lcd_set_cursor(1, input_pos + 1); // +1 for the '>' character
                            }
                        }
                        else if (key == '#') // Submit password
                        {
                            // Hide cursor during processing
                            lcd_cursor_show(false);
                            
                            if (check_password(input))
                            {
                                // Password correct
                                is_authenticated = true;
                                password_mode = false;
                                password_retries = 0;
                                
                                lcd_clear();
                                lcd_set_cursor(0, 0);
                                lcd_print("Access Granted");
                                vTaskDelay(1000 / portTICK_PERIOD_MS);
                                
                                // Show first parameter
                                param_idx = 0;
                                lcd_clear();
                                lcd_set_cursor(0, 0);
                                lcd_print("%s", parameters[param_idx].name);
                                lcd_set_cursor(1, 0);
                                
                                // Format and display current value
                                if (parameters[param_idx].value != NULL)
                                {
                                    char formatted_output[32] = {0};
                                    format_input_according_to_rules(
                                        (char *)parameters[param_idx].value,
                                        formatted_output,
                                        &parameters[param_idx].validation);
                                    lcd_print("Val: %s", formatted_output);
                                }
                                else
                                {
                                    lcd_print("Val: <none>");
                                }
                            }
                            else
                            {
                                // Password incorrect
                                password_retries++;
                                
                                if (password_retries >= MAX_PASSWORD_RETRIES)
                                {
                                    // Max retries reached, activate lockout
                                    is_locked_out = true;
                                    lockout_start = xTaskGetTickCount();
                                    
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("Max retries");
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Locked for %ds", parameters[23].validation.lockout_time);
                                }
                                else
                                {
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("Wrong Password!");
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Retry %d/%d", password_retries, MAX_PASSWORD_RETRIES);
                                    vTaskDelay(1500 / portTICK_PERIOD_MS);
                                    
                                    // Reset for next attempt
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("Enter Password:");
                                    lcd_set_cursor(1, 0);
                                    lcd_print(">");
                                    lcd_set_cursor(1, 1); // Position cursor after ">"
                                    lcd_cursor_show(true); // Show cursor for password entry
                                }
                                
                                // Clear input for next attempt
                                memset(input, 0, sizeof(input));
                                input_pos = 0;
                            }
                        }
                        else if (key == 'A')
                        {
                            // Exit password mode
                            in_keyboard_mode = false;
                            password_mode = false;
                            
                            // Hide cursor when exiting
                            lcd_cursor_show(false);
                            lcd_clear();
                        }

                        xSemaphoreGive(lcd_semaphore);
                        semaphore_taken = false;
                    }
                }
                else if (is_authenticated)
                {
                    // Regular parameter editing mode
                    if (xSemaphoreTake(lcd_semaphore, portMAX_DELAY) == pdTRUE)
                    {
                        semaphore_taken = true;
                        
                        if (key == 'A') // Exit keyboard mode
                        {
                            // Turn off cursor when exiting
                            lcd_cursor_show(false);
                            
                            // Exit keyboard mode
                            in_keyboard_mode = false;
                            is_authenticated = false;
                            lcd_clear();
                        }
                        else if (key == 'B') // Previous parameter
                        {
                            if (param_idx > 0)
                                param_idx--;
                            else
                                param_idx = NUM_PARAMETERS - 1;
                                
                            // Refresh RTC time if showing time parameter
                            if (parameters[param_idx].address == PARAM_ADDRESS_TIME)
                            {
                                refresh_rtc_time();
                            }
                                
                            // Display new parameter
                            lcd_clear();
                            lcd_set_cursor(0, 0);
                            lcd_print("%s", parameters[param_idx].name);
                            lcd_set_cursor(1, 0);
                            
                            // Format and display current value
                            if (parameters[param_idx].value != NULL)
                            {
                                char formatted_output[32] = {0};
                                format_input_according_to_rules(
                                    (char *)parameters[param_idx].value, 
                                    formatted_output,
                                    &parameters[param_idx].validation);
                                lcd_print("Val: %s", formatted_output);
                            }
                            else
                            {
                                lcd_print("Val: <none>");
                            }
                            
                            // Hide cursor when just viewing
                            lcd_cursor_show(false);
                            
                            // Reset input
                            memset(input, 0, sizeof(input));
                            input_pos = 0;
                        }
                        else if (key == 'C') // Next parameter
                        {
                            param_idx = (param_idx + 1) % NUM_PARAMETERS;
                            
                            // Refresh RTC time if showing time parameter
                            if (parameters[param_idx].address == PARAM_ADDRESS_TIME)
                            {
                                refresh_rtc_time();
                            }
                            
                            // Display new parameter
                            lcd_clear();
                            lcd_set_cursor(0, 0);
                            lcd_print("%s", parameters[param_idx].name);
                            lcd_set_cursor(1, 0);
                            
                            // Format and display current value
                            if (parameters[param_idx].value != NULL)
                            {
                                char formatted_output[32] = {0};
                                format_input_according_to_rules(
                                    (char *)parameters[param_idx].value, 
                                    formatted_output,
                                    &parameters[param_idx].validation);
                                lcd_print("Val: %s", formatted_output);
                            }
                            else
                            {
                                lcd_print("Val: <none>");
                            }
                            
                            // Hide cursor when just viewing
                            lcd_cursor_show(false);
                            
                            // Reset input
                            memset(input, 0, sizeof(input));
                            input_pos = 0;
                        }
                        else if (key == 'D') // Delete character
                        {
                            if (input_pos > 0)
                            {
                                input[--input_pos] = '\0';
                                
                                // Update display
                                char formatted_output[32] = {0};
                                if (input_pos > 0) {
                                    format_input_according_to_rules(
                                        input, 
                                        formatted_output,
                                        &parameters[param_idx].validation);
                                }
                                
                                lcd_set_cursor(1, 0);
                                lcd_print("Val: %s ", formatted_output); // Space to clear last character
                                
                                // Set cursor position for editing
                                int cursor_pos = 5; // "Val: " is 5 characters
                                
                                // Handle formatted display with different cursor positions
                                if (parameters[param_idx].validation.format == FORMAT_TIME) {
                                    // For time format (HH:MM), calculate cursor position
                                    cursor_pos += (input_pos < 2) ? input_pos : input_pos + 1; // +1 for the colon
                                } 
                                else if (parameters[param_idx].validation.format == FORMAT_DATE) {
                                    // For date format (DD/MM/YY), calculate cursor position
                                    if (input_pos < 2) cursor_pos += input_pos;
                                    else if (input_pos < 4) cursor_pos += input_pos + 1; // +1 for first slash
                                    else cursor_pos += input_pos + 2; // +2 for two slashes
                                }
                                else {
                                    cursor_pos += input_pos;
                                }
                                
                                lcd_set_cursor(1, cursor_pos);
                                lcd_cursor_show(true);
                                lcd_cursor_blink(true);
                            }
                        }
                        else if (key == '*') // Decimal point or negative sign
                        {
                            if (parameters[param_idx].validation.format == FORMAT_DECIMAL)
                            {
                                // If this is the first character entered, clear the previous value
                                if (input_pos == 0)
                                {
                                    // Clear the display first
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Val:                "); // Clear the entire line
                                    
                                    // Enable cursor
                                    lcd_cursor_show(true);
                                    lcd_cursor_blink(true);
                                }
                                
                                // Only add decimal if we haven't already added one
                                bool has_decimal = false;
                                for (int i = 0; i < input_pos; i++)
                                {
                                    if (input[i] == '.')
                                    {
                                        has_decimal = true;
                                        break;
                                    }
                                }
                                
                                if (!has_decimal && input_pos < parameters[param_idx].validation.max_length)
                                {
                                    input[input_pos++] = '.';
                                    input[input_pos] = '\0';
                                    
                                    // Update display
                                    char formatted_output[32] = {0};
                                    format_input_according_to_rules(
                                        input, 
                                        formatted_output,
                                        &parameters[param_idx].validation);
                                    
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Val: %s", formatted_output);
                                    
                                    // Set cursor position for editing
                                    lcd_set_cursor(1, 5 + input_pos); // "Val: " is 5 characters
                                    lcd_cursor_show(true);
                                    lcd_cursor_blink(true);
                                }
                            }
                            else if (parameters[param_idx].validation.allow_negative)
                            {
                                // For parameters that allow negative values, use '*' as a sign toggle
                                
                                // If no input yet, start with a minus sign
                                if (input_pos == 0)
                                {
                                    // Clear the display first
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Val:                "); // Clear the entire line
                                    
                                    input[input_pos++] = '-';
                                    input[input_pos] = '\0';
                                    
                                    // Display the minus sign
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Val: -");
                                    lcd_set_cursor(1, 6); // Position after the minus
                                    lcd_cursor_show(true);
                                    lcd_cursor_blink(true);
                                }
                                else if (input_pos > 0)
                                {
                                    // Toggle the sign if there's already input
                                    if (input[0] == '-')
                                    {
                                        // Remove the minus sign
                                        for (int i = 0; i < input_pos; i++)
                                        {
                                            input[i] = input[i+1];
                                        }
                                        input_pos--;
                                        
                                        // Update display
                                        lcd_set_cursor(1, 0);
                                        lcd_print("Val:                "); // Clear line
                                        lcd_set_cursor(1, 0);
                                        lcd_print("Val: %s", input);
                                        lcd_set_cursor(1, 5 + input_pos);
                                    }
                                    else
                                    {
                                        // Add minus sign at the beginning
                                        for (int i = input_pos; i > 0; i--)
                                        {
                                            input[i] = input[i-1];
                                        }
                                        input[0] = '-';
                                        input_pos++;
                                        input[input_pos] = '\0';
                                        
                                        // Update display
                                        lcd_set_cursor(1, 0);
                                        lcd_print("Val:                "); // Clear line
                                        lcd_set_cursor(1, 0);
                                        lcd_print("Val: %s", input);
                                        lcd_set_cursor(1, 5 + input_pos);
                                    }
                                }
                            }
                        }
                        else if (key >= '0' && key <= '9') // Number input
                        {
                            // If this is the first digit entered, clear the previous value
                            if (input_pos == 0)
                            {
                                // Clear the display first
                                lcd_set_cursor(1, 0);
                                lcd_print("Val:                "); // Clear the entire line
                                
                                // Enable cursor for editing
                                lcd_cursor_show(true);
                                lcd_cursor_blink(true);
                            }
                            
                            if (input_pos < parameters[param_idx].validation.max_length)
                            {
                                input[input_pos++] = key;
                                input[input_pos] = '\0';
                                
                                // Format and display input
                                char formatted_output[32] = {0};
                                format_input_according_to_rules(
                                    input, 
                                    formatted_output,
                                    &parameters[param_idx].validation);
                                
                                lcd_set_cursor(1, 0);
                                lcd_print("Val: %s", formatted_output);
                                
                                // Set cursor position for editing based on format type
                                int cursor_pos = 5; // "Val: " is 5 characters
                                
                                // Handle formatted display with different cursor positions
                                if (parameters[param_idx].validation.format == FORMAT_TIME) {
                                    // For time format (HH:MM), calculate cursor position
                                    cursor_pos += (input_pos < 2) ? input_pos : input_pos + 1; // +1 for the colon
                                } 
                                else if (parameters[param_idx].validation.format == FORMAT_DATE) {
                                    // For date format (DD/MM/YY), calculate cursor position
                                    if (input_pos < 2) cursor_pos += input_pos;
                                    else if (input_pos < 4) cursor_pos += input_pos + 1; // +1 for first slash
                                    else cursor_pos += input_pos + 2; // +2 for two slashes
                                }
                                else {
                                    cursor_pos += input_pos;
                                }
                                
                                lcd_set_cursor(1, cursor_pos);
                            }
                        }
                        else if (key == '#') // Submit value
                        {
                            // Hide cursor during processing
                            lcd_cursor_show(false);
                            
                            if (input_pos > 0)
                            {
                                // After saving time, refresh from RTC to ensure accurate display
                                bool was_time_param = (parameters[param_idx].address == PARAM_ADDRESS_TIME);
                                
                                // For time parameters, convert HHMM to standard format
                                if (parameters[param_idx].type == PARAM_TYPE_TIME && strlen(input) == 4)
                                {
                                    // Convert from HHMM to HH:MM - fixed to avoid double colon
                                    char formatted_time[6]; // HH:MM + null
                                    snprintf(formatted_time, 6, "%c%c:%c%c", 
                                             input[0], input[1], input[2], input[3]);
                                    strcpy(input, formatted_time);
                                }
                                
                                // Free old value if exists
                                if (parameters[param_idx].value != NULL)
                                {
                                    free(parameters[param_idx].value);
                                }
                                
                                // Set new value
                                parameters[param_idx].value = strdup(input);
                                
                                // Reset validation status before validating
                                validation_failed = false;
                                validation_error_message[0] = '\0';
                                
                                // Validate and store
                                if (parameters[param_idx].validate != NULL)
                                {
                                    parameters[param_idx].validate(parameters[param_idx].value);
                                }
                                
                                // Check if validation failed and show error message
                                if (validation_failed)
                                {
                                    // Show error message
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("Invalid input!");
                                    lcd_set_cursor(1, 0);
                                    lcd_print("%s", validation_error_message);
                                    vTaskDelay(2000 / portTICK_PERIOD_MS); // Show error for 2 seconds
                                    
                                    // Return to parameter display
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("%s", parameters[param_idx].name);
                                    lcd_set_cursor(1, 0);
                                    
                                    // Format and display the current (corrected) value
                                    char formatted_output[32] = {0};
                                    format_input_according_to_rules(
                                        (char *)parameters[param_idx].value, 
                                        formatted_output,
                                        &parameters[param_idx].validation);
                                    lcd_print("Val: %s", formatted_output);
                                }
                                else
                                {
                                    // Value is valid, proceed with storing
                                    store_parameter(param_idx);
                                    
                                    // Format and display the updated value
                                    char formatted_output[32] = {0};
                                    format_input_according_to_rules(
                                        (char *)parameters[param_idx].value, 
                                        formatted_output,
                                        &parameters[param_idx].validation);
                                    
                                    // Show confirmation
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("Value saved!");
                                    lcd_set_cursor(1, 0);
                                    lcd_print("%s", formatted_output);
                                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                                    
                                    // Show parameter again
                                    lcd_clear();
                                    lcd_set_cursor(0, 0);
                                    lcd_print("%s", parameters[param_idx].name);
                                    lcd_set_cursor(1, 0);
                                    lcd_print("Val: %s", formatted_output);
                                    
                                    // After saving time, refresh the RTC value
                                    if (was_time_param)
                                    {
                                        // Wait a moment for RTC to update
                                        vTaskDelay(100 / portTICK_PERIOD_MS);
                                        refresh_rtc_time();
                                    }
                                }
                            }
                            
                            // Reset input and keep cursor hidden after saving
                            memset(input, 0, sizeof(input));
                            input_pos = 0;
                        }

                        xSemaphoreGive(lcd_semaphore);
                        semaphore_taken = false;
                    }
                }
            }
        }
        
        // Small delay to prevent high CPU usage
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// Function to write data to 24C32 EEPROM
static esp_err_t eeprom_write(uint16_t addr, uint8_t *data, size_t data_len)
{
    if (xSemaphoreTake(i2c_semaphore, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE("EEPROM", "Failed to take I2C semaphore");
        return ESP_FAIL;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EEPROM_24C32_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (addr >> 8) & 0xFF, true); // High byte of address
    i2c_master_write_byte(cmd, addr & 0xFF, true);        // Low byte of address
    i2c_master_write(cmd, data, data_len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_semaphore);

    if (ret != ESP_OK)
    {
        ESP_LOGE("EEPROM", "Failed to write to 24C32: %s", esp_err_to_name(ret));
    }
    else
    {
        // EEPROM write cycle time - typically 5ms
        vTaskDelay(5 / portTICK_PERIOD_MS);
    }
    return ret;
}

// Function to read data from 24C32 EEPROM
static esp_err_t eeprom_read(uint16_t addr, uint8_t *data, size_t data_len)
{
    if (xSemaphoreTake(i2c_semaphore, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE("EEPROM", "Failed to take I2C semaphore");
        return ESP_FAIL;
    }

    // First set the address
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EEPROM_24C32_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (addr >> 8) & 0xFF, true); // High byte of address
    i2c_master_write_byte(cmd, addr & 0xFF, true);        // Low byte of address
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK)
    {
        ESP_LOGE("EEPROM", "Failed to set address: %s", esp_err_to_name(ret));
        xSemaphoreGive(i2c_semaphore);
        return ret;
    }

    // Then read the data
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (EEPROM_24C32_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, data_len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    xSemaphoreGive(i2c_semaphore);

    if (ret != ESP_OK)
    {
        ESP_LOGE("EEPROM", "Failed to read from 24C32: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t write_pcf8574(uint8_t row_mask)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCF8574_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, row_mask, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(keypad_i2c_port, cmd, I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK)
    {
        ESP_LOGE("Keypad", "Failed to write row mask 0x%02X: %s", row_mask, esp_err_to_name(ret));
    }
    return ret;
}

static uint8_t read_pcf8574(uint8_t row_mask)
{
    if (xSemaphoreTake(i2c_semaphore, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE("Keypad", "Failed to take I2C semaphore");
        return 0xFF;
    }

    // Write the row mask
    esp_err_t write_ret = write_pcf8574(row_mask);
    if (write_ret != ESP_OK)
    {
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

    if (ret != ESP_OK)
    {
        ESP_LOGE("Keypad", "Failed to read PCF8574 with mask 0x%02X: %s", row_mask, esp_err_to_name(ret));
        return 0xFF;
    }

    // ESP_LOGI("Keypad", "Row mask 0x%02X, data: 0x%02X (%d)", row_mask, data, data);
    return data;
}

// Update format_input_according_to_rules to properly handle time formatting
static void format_input_according_to_rules(const char *input, char *output, const param_validation_t *rules)
{
    if (!input || !output || !rules)
    {
        if (output)
            output[0] = '\0';
        return;
    }

    // Reset output
    output[0] = '\0';

    // Format according to type
    switch (rules->format)
    {
        case FORMAT_ENABLE_DISABLE:
            if (strcmp(input, "1") == 0 || strcmp(input, "Enable") == 0)
                strcpy(output, "Enable");
            else if (strcmp(input, "0") == 0 || strcmp(input, "Disable") == 0)
                strcpy(output, "Disable");
            else
                strcpy(output, input);
            break;

        case FORMAT_MULTIPLE:
            // Format for multiple choice parameters
            if (strcmp(input, "0") == 0)
                strcpy(output, "ALL");
            else if (strcmp(input, "1") == 0)
                strcpy(output, "Volt");
            else if (strcmp(input, "2") == 0)
                strcpy(output, "Curr");
            else if (strcmp(input, "3") == 0)
                strcpy(output, "None");
            else
                strcpy(output, input); // Keep original for unknown values
            break;

        case FORMAT_DATE:
            if (strlen(input) > 0)
            {
                format_date(input, output, 32);
            }
            else
            {
                strcpy(output, input);
            }
            break;

        case FORMAT_TIME:
            if (strlen(input) > 0)
            {
                // Directly use our new format_time function
                char formatted_time[10] = {0};
                format_time((char *)input, formatted_time);
                strcpy(output, formatted_time);
            }
            else
            {
                strcpy(output, "");
            }
            break;

        case FORMAT_DECIMAL:
            // This is handled by validate_decimal
            strcpy(output, input);
            break;

        default:
            // For other formats, just copy
            strcpy(output, input);
            break;
    }
}

// Keep only one definition of validate_enable_disable (the original one)
// Remove the duplicate definition

// Keep only the static version of check_password
static bool check_password(const char *entered_password)
{
    if (entered_password == NULL)
    {
        return false;
    }

    // Find the password parameter
    const char *stored_password = NULL;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (strstr(parameters[i].name, "Password") != NULL)
        {
            stored_password = (const char *)parameters[i].value;
            break;
        }
    }

    // If no password is set, or password doesn't match
    if (stored_password == NULL || strcmp(entered_password, stored_password) != 0)
    {
        return false;
    }

    return true;
}

void validate_decimal(void *value)
{
    char *val_str = (char *)value;
    if (!val_str)
        return;

    // Reset validation status
    validation_failed = false;
    validation_error_message[0] = '\0';

    // Find the parameter this value belongs to
    parameter_t *param = NULL;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].value == value)
        {
            param = &parameters[i];
            break;
        }
    }
    if (!param)
        return;

    // Parse the decimal value
    double val = atof(val_str);

    // Check range
    if (val < param->validation.min_value || val > param->validation.max_value)
    {
        // Set validation error flag and message
        validation_failed = true;
        snprintf(validation_error_message, sizeof(validation_error_message), 
                "Range %.1f-%.1f", param->validation.min_value, param->validation.max_value);
        
        // Reset to default value
        strcpy(val_str, param->default_value);
        return;
    }

    // Format according to decimal places
    char format[10];
    snprintf(format, sizeof(format), "%%.%df", param->validation.decimal_places);
    char temp[32];
    snprintf(temp, sizeof(temp), format, val);
    strcpy(val_str, temp);
}

void validate_password(void *value)
{
    char *password = (char *)value;
    if (!password)
        return;

    // Find the password parameter
    parameter_t *param = NULL;
    for (int i = 0; i < NUM_PARAMETERS; i++)
    {
        if (parameters[i].type == PARAM_TYPE_PASSWORD)
        {
            param = &parameters[i];
            break;
        }
    }
    if (!param)
        return;

    // Check length
    if (strlen(password) != param->validation.max_length)
    {
        strcpy(password, param->default_value);
        return;
    }

    // Check if all characters are digits
    for (int i = 0; i < param->validation.max_length; i++)
    {
        if (!isdigit((unsigned char)password[i]))
        { // Cast to unsigned char to fix warning
            strcpy(password, param->default_value);
            return;
        }
    }
}