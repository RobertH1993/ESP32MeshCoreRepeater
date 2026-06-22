#ifndef LORA_H
#define LORA_H

#include <stdint.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "sx126x_driver/src/sx126x.h"
#include "sx126x_driver/src/sx126x_status.h"
#include "heltecv4.h"

#ifdef __cplusplus
extern "C" {
#endif

// Task handles
extern TaskHandle_t lora_rx_task_handle;
extern TaskHandle_t lora_tx_task_handle;

#define LORA_BAND_MAX 8

typedef enum {
    LORA_BAND_EU868 = 0,
    LORA_BAND_US915 = 1,
    LORA_BAND_AU915 = 2,
    LORA_BAND_CN779 = 3,
    LORA_BAND_EU433 = 4,
    LORA_BAND_RU864 = 5,
    LORA_BAND_KR920 = 6,
    LORA_BAND_IN865 = 7,
} lora_band_t;


typedef struct {
    sx126x_device_t *device;
    QueueHandle_t queue;
} lora_rxtx_task_data_t;

typedef struct {
    uint8_t data[256];
    uint16_t data_len;
    uint8_t rssi;
    uint8_t snr;
    uint8_t signal_rssi;
} lora_packet_t;


/*
 * @brief Initialize the radio
 * @param device The device to initialize the radio on
 * @param band The band to use
 * @param sf The spreading factor to use
 * @param bw The bandwidth to use
 * @param cr The coding rate to use
 * @return ESP_OK if successful, otherwise ESP_FAIL
 */
esp_err_t lora_init_radio(sx126x_device_t *device, lora_band_t band, sx126x_lora_sf_t sf, sx126x_lora_bw_t bw, sx126x_lora_cr_t cr);


esp_err_t lora_start_rx_polling(sx126x_device_t *device, uint8_t * rx_buffer, uint16_t rx_buffer_size);

/*
 * @brief Start the RX task, this wait for data on the radio, validates it and puts it in the rx_queue.
 * This task uses interrupts to detect when data is available on the radio, thus making it able to put the
 * ESP32 to sleep mode when there is no data to process.
 * @param pvParameters The parameters for the task
 */
void lora_rx_task(void *pvParameters);

/*
 * @brief Start the TX task, this sends data from the tx_queue to the radio
 * @param device The device to start the TX task on
 * @param tx_queue The queue to send the data from
 */
void lora_tx_task(void *pvParameters);


#endif