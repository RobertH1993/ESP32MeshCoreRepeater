#include "display.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "board.h"
#include "esp_oled_ssd1315.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

char TAG[] = "display";

void display_init(i2c_master_bus_handle_t i2c_bus){
    esp_lcd_panel_io_handle_t panel_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t panel_io_config = {
        .dev_addr = BOARD_OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &panel_io_config, &panel_io_handle));
    ESP_LOGI(TAG, "Panel IO initialized");

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_dev_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = PIN_OLED_RESET,
    };
    esp_lcd_panel_ssd1315_config_t ssd1315_config = {
        .height = BOARD_OLED_HEIGHT,
    };
    panel_dev_config.vendor_config = &ssd1315_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1315(panel_io_handle, &panel_dev_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_disp_on_off(panel_handle, false);
}
