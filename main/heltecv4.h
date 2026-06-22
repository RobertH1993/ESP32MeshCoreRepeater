#ifndef SX126X_DEVICE_H
#define SX126X_DEVICE_H

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t spi_host;
    spi_device_handle_t spi_device;
    gpio_num_t nss;
    gpio_num_t reset;
    gpio_num_t busy;
    gpio_num_t dio1;

    SemaphoreHandle_t mutex;
} sx126x_device_t;


esp_err_t sx126x_device_init(sx126x_device_t *device);

/*
 * @brief Set the Vext rail, the vext rail is used to power the OLED display and external components
 * @param enabled True to enable the Vext rail, false to disable
 * @return ESP_OK if successful, otherwise ESP_FAIL
 */
esp_err_t set_vext(bool enabled);

esp_err_t white_led_init(void);
esp_err_t set_white_led(bool on);

esp_err_t set_fem_mode(bool rx);

#ifdef __cplusplus
}
#endif

#endif // SX126X_DEVICE_H