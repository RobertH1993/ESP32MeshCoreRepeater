#include "sx126x_driver/src/sx126x_hal.h"
#include "heltecv4.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// Max time to wait for the SX126x BUSY line to drop before a transaction.
#define SX126X_BUSY_TIMEOUT_US 100000

static const char *TAG = "sx126x_hal";

// The SX126x keeps BUSY high while it processes the previous command. A new
// SPI access must not start until BUSY has dropped again.
static sx126x_hal_status_t sx126x_hal_wait_on_busy(const sx126x_device_t *device)
{
    int waited_us = 0;
    while (gpio_get_level(device->busy)) {
        if (waited_us >= SX126X_BUSY_TIMEOUT_US) {
            ESP_LOGE(TAG, "BUSY stuck high after %d us", waited_us);
            return SX126X_HAL_STATUS_ERROR;
        }
        esp_rom_delay_us(10);
        waited_us += 10;
    }
    return SX126X_HAL_STATUS_OK;
}

// Public
sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    sx126x_device_t *device = (sx126x_device_t *) context;

    gpio_set_level(device->reset, 0);
    esp_rom_delay_us(200); // 100us specified, 200us is a safe margin
    gpio_set_level(device->reset, 1);

    // Wait until the radio has booted and released BUSY.
    return sx126x_hal_wait_on_busy(device);
}

// Public
sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    sx126x_device_t *device = (sx126x_device_t *) context;

    // Any SPI access drives NSS low, and that falling edge wakes the chip from
    // sleep. A harmless GetStatus (0xC0) opcode is enough to generate it.
    uint8_t opcode = 0xC0;
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &opcode,
    };
    if (spi_device_polling_transmit(device->spi_device, &t) != ESP_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    // The radio is ready once it has returned to STDBY and dropped BUSY.
    return sx126x_hal_wait_on_busy(device);
}

// Public
sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command, const uint16_t command_length,
                                     const uint8_t *data, const uint16_t data_length)
{
    sx126x_device_t *device = (sx126x_device_t *) context;

    if (sx126x_hal_wait_on_busy(device) != SX126X_HAL_STATUS_OK) {
        ESP_LOGI(TAG, "Bus still busy");
        return SX126X_HAL_STATUS_ERROR;
    }

    // Acquire the bus: keeps CS asserted atomically across the command + data
    // phases so no other device can interleave between them.
    // portMAX_DELAY is required — ESP-IDF does not support finite timeouts here.
    if (spi_device_acquire_bus(device->spi_device, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire bus");
        return SX126X_HAL_STATUS_ERROR;
    }

    esp_err_t err = ESP_OK;

    if (command_length > 0) {
        spi_transaction_t cmd_trans = {
            .length = (size_t) command_length * 8,
            .tx_buffer = command,
            // Keep CS low so the data phase is part of the same SPI frame.
            .flags = (data_length > 0) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(device->spi_device, &cmd_trans);
    }

    if (err == ESP_OK && data_length > 0) {
        spi_transaction_t data_trans = {
            .length = (size_t) data_length * 8,
            .tx_buffer = data,
        };
        err = spi_device_polling_transmit(device->spi_device, &data_trans);
    }

    spi_device_release_bus(device->spi_device);
    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

// Public
sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command, const uint16_t command_length,
                                    uint8_t *data, const uint16_t data_length)
{
    sx126x_device_t *device = (sx126x_device_t *) context;

    if (sx126x_hal_wait_on_busy(device) != SX126X_HAL_STATUS_OK) {
        ESP_LOGI(TAG, "Bus still busy");
        return SX126X_HAL_STATUS_ERROR;
    }

    // portMAX_DELAY is required — ESP-IDF does not support finite timeouts here.
    if (spi_device_acquire_bus(device->spi_device, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire bus");
        return SX126X_HAL_STATUS_ERROR;
    }

    esp_err_t err = ESP_OK;

    if (command_length > 0) {
        // Command phase: clock out the opcode/offset, keep CS asserted so the
        // read phase belongs to the same SPI frame.
        spi_transaction_t cmd_trans = {
            .length = (size_t) command_length * 8,
            .tx_buffer = command,
            .flags = (data_length > 0) ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
        };
        err = spi_device_polling_transmit(device->spi_device, &cmd_trans);
    }

    if (err == ESP_OK && data_length > 0) {
        // Read phase: tx_buffer NULL → driver clocks out 0x00 (SX126X_NOP)
        // while incoming bytes are captured into data.
        spi_transaction_t data_trans = {
            .length = (size_t) data_length * 8,
            .rxlength = (size_t) data_length * 8,
            .tx_buffer = NULL,
            .rx_buffer = data,
        };
        err = spi_device_polling_transmit(device->spi_device, &data_trans);
    }

    spi_device_release_bus(device->spi_device);

    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}
