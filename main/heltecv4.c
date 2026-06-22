#include "heltecv4.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "board.h"

#include "sx126x_driver/src/sx126x.h"
#include "sx126x_driver/src/sx126x_hal.h"
#include "sx126x_driver/src/sx126x_status.h"

const static char TAG[] = "heltecv4_hal";


// Uses the Heltec v4 specific settings for the SX126X device.
esp_err_t sx126x_device_setup_heltec_v4(sx126x_device_t *device){
    ESP_LOGI(TAG, "Setup SX126X device");

    // TCXO: DIO3 supplies 1.8 V to the TCXO, 5 ms startup time (320 * 15.625 us).
    if(sx126x_set_dio3_as_tcxo_ctrl(device, SX126X_TCXO_CTRL_1_8V, 320) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set DIO3 as TCXO control");
        return ESP_FAIL;
    }
    
    // Calibrate ALL blocks with the new TCXO reference. Without this the PLL
    // is still calibrated on the internal RC oscillator and set_rx will fail.
    if(sx126x_cal(device, SX126X_CAL_ALL) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to calibrate");
        return ESP_FAIL;
    }

    // Heltec V4 has an external DCDC inductor on DCC_SW — use DCDC, not LDO.
    if(sx126x_set_reg_mode(device, SX126X_REG_MODE_DCDC) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set regulator mode");
        return ESP_FAIL;
    }

    // DIO2 drives the GC1109/KCT8103L RF-switch TX/RX line.
    if(sx126x_set_dio2_as_rf_sw_ctrl(device, true) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set DIO2 as RF switch control");
        return ESP_FAIL;
    }

    // RX and TX buffer both start at address 0.
    if(sx126x_set_buffer_base_address(device, 0x00, 0x00) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set buffer base address");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Public
esp_err_t sx126x_device_init(sx126x_device_t *device){
    ESP_LOGI(TAG, "Init SX126X device");

    // Initialize SPI bus
    ESP_LOGI(TAG, "Init SPI");
    spi_bus_config_t spi_bus_config = {
        .mosi_io_num = P_LORA_MOSI,
        .miso_io_num = P_LORA_MISO,
        .sclk_io_num = P_LORA_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LORA_SPI_HOST_NUM, &spi_bus_config, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI bus initialized");

    // Initialize GPIOs
    gpio_set_direction(P_LORA_BUSY, GPIO_MODE_INPUT);
    gpio_set_direction(P_LORA_PA_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(P_LORA_PA_POWER, 1); // Enable FEM LDO power: required for the antenna switch to work in both RX and TX. Without this no signal reaches the SX1262.
    gpio_set_direction(P_LORA_DIO_1, GPIO_MODE_INPUT);
    gpio_set_direction(P_LORA_NSS, GPIO_MODE_OUTPUT);
    gpio_set_level(P_LORA_NSS, 1);
    gpio_set_direction(P_LORA_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(P_LORA_RESET, 1); // inactive (reset is active-low)

    // Detect v4.3 / v4.2
    bool v4_3 = false;
    gpio_set_direction(2, GPIO_MODE_INPUT);
    gpio_get_level(2);
    if(gpio_get_level(2) == 1){
        v4_3 = true;
    }

    // TODO change based on v4.3 or v4.2
    gpio_set_direction(P_LORA_KCT8103L_PA_CSD, GPIO_MODE_OUTPUT);
    gpio_set_level(P_LORA_KCT8103L_PA_CSD, 1); // Enable PA/FEM

    gpio_set_direction(P_LORA_KCT8103L_PA_CTX, GPIO_MODE_OUTPUT);
    gpio_set_level(P_LORA_KCT8103L_PA_CTX, 0); // Set to RX mode

    // Add the SX126X device to the SPI bus
    spi_device_interface_config_t spi_device_config = {
        .clock_speed_hz = 14000000,
        .mode = 0,
        .spics_io_num = P_LORA_NSS,
        .queue_size = 7,
        .flags = 0,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(BOARD_LORA_SPI_HOST_NUM, &spi_device_config, &device->spi_device));
    ESP_LOGI(TAG, "SX126X device added to SPI bus");

    device->spi_host = BOARD_LORA_SPI_HOST_NUM;
    device->nss = P_LORA_NSS;
    device->reset = P_LORA_RESET;
    device->busy = P_LORA_BUSY;
    device->dio1 = P_LORA_DIO_1;

    sx126x_hal_reset(device);
    return sx126x_device_setup_heltec_v4(device);
}

esp_err_t white_led_init(void){
    gpio_config_t white_led_cfg = {
        .pin_bit_mask = 1ULL << P_LORA_TX_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&white_led_cfg));
    return ESP_OK;
}


esp_err_t set_white_led(bool on){
    gpio_set_level(P_LORA_TX_LED, on ? 1 : 0);
    return ESP_OK;
}

esp_err_t set_fem_mode(bool rx){
    if(rx){
        return gpio_set_level(P_LORA_KCT8103L_PA_CTX, 0); // Set to RX mode
    }else{
        return gpio_set_level(P_LORA_KCT8103L_PA_CTX, 1); // Set to TX mode
    }
}

esp_err_t set_vext(bool enabled){
    // Power the Vext rail: on the Heltec V4 the OLED (and its I2C pull-ups)
    // are fed from Vext, so it must be enabled before any I2C traffic.
    ESP_LOGI(TAG, "Vext status: %s", enabled ? "enabled" : "disabled");
    gpio_config_t vext_cfg = {
        .pin_bit_mask = 1ULL << PIN_VEXT_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&vext_cfg));
    esp_err_t err = gpio_set_level(PIN_VEXT_EN, enabled ? 0ULL : 1ULL);
    if(enabled){
        vTaskDelay(pdMS_TO_TICKS(10)); // Delay to let the rail settle and the OLED controller boot
    }
    return err;
}