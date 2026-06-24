#include "lora.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "hal/gpio_types.h"
#include "sx126x_driver/src/sx126x.h"
#include "sx126x_driver/src/sx126x_status.h"
#include "board.h"
#include "heltecv4.h"

#define LORA_CAD_MAX_TRIES 3
#define LORA_CAD_PEAK_THRESHOLD 22
#define LORA_CAD_MIN_THRESHOLD 10

const static char TAG[] = "meshcore";

TaskHandle_t lora_rx_task_handle = NULL;
TaskHandle_t lora_tx_task_handle = NULL;

typedef enum {
    LORA_MODE_RX = 0,
    LORA_MODE_CAD = 1,
    LORA_MODE_TX = 2,
} lora_radio_mode_t;


// DIO1 ISR: minimal work — wake the RX or TX task and yield if a higher-priority
// task became ready. The task handle is passed as the ISR argument so no
// global state is needed.
static void IRAM_ATTR meshcore_dio1_isr(void *arg)
{
    TaskHandle_t task = (TaskHandle_t) arg;
    BaseType_t higher_prio_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

// DMA aligned buffers for the RX and TX tasks
DMA_ATTR static uint8_t task_rx_buffer[256];


static const uint32_t band_frequencies[LORA_BAND_MAX] = {
    [LORA_BAND_EU868] = 869618000,
    [LORA_BAND_US915] = 915000000,
    [LORA_BAND_AU915] = 915000000,
    [LORA_BAND_CN779] = 779000000,
    [LORA_BAND_EU433] = 433000000,
    [LORA_BAND_RU864] = 864000000,
    [LORA_BAND_KR920] = 920000000,
    [LORA_BAND_IN865] = 865000000,
};

static const uint16_t image_calibration_parameters[LORA_BAND_MAX] = {
    [LORA_BAND_EU868] = 0xD7DB,
    [LORA_BAND_US915] = 0xE1E9,
    [LORA_BAND_AU915] = 0xE1E9,
    [LORA_BAND_CN779] = 0xC1C5,
    [LORA_BAND_EU433] = 0x6B6F,
    [LORA_BAND_RU864] = 0xD7DB,
    [LORA_BAND_KR920] = 0xE1E9,
    [LORA_BAND_IN865] = 0xD7DB,
};


esp_err_t lora_init_radio(sx126x_device_t *device, lora_band_t band, sx126x_lora_sf_t sf, sx126x_lora_bw_t bw, sx126x_lora_cr_t cr){
    ESP_LOGI(TAG, "Init Radio");

    // Set LoRa packet type
    if(sx126x_set_pkt_type(device, SX126X_PKT_TYPE_LORA) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa packet type");
        return ESP_FAIL;
    }

    // Set meshcore packet parameters
    sx126x_pkt_params_lora_t pkt_params = {
        .preamble_len_in_symb = 16,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = 255,
        .crc_is_on            = true,
        .invert_iq_is_on      = false,
    };
    if(sx126x_set_lora_pkt_params(device, &pkt_params) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa packet parameters");
        return ESP_FAIL;
    }

    // Set RF frequency
    if(sx126x_set_rf_freq(device, band_frequencies[band]) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set RF frequency: %d", band_frequencies[band]);
        return ESP_FAIL;
    }

    // Set LoRa modulation parameters
    sx126x_mod_params_lora_t mod_params = {
        .sf = sf,
        .bw = bw,
        .cr = cr,
        .ldro = 0,
    };
    if(sx126x_set_lora_mod_params(device, &mod_params) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa modulation parameters");
        return ESP_FAIL;
    }

    // Set LoRa sync word
    if(sx126x_set_lora_sync_word(device, 0x12) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set LoRa sync word");
        return ESP_FAIL;
    }
    
    // Image calibration
    uint8_t freq1 = image_calibration_parameters[band] >> 8;
    uint8_t freq2 = image_calibration_parameters[band] & 0xFF;
    if(sx126x_cal_img(device, freq1, freq2) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to calibrate image");
        return ESP_FAIL;
    }

    // Set default TX power to a conservative level
    sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x04,
        .hp_max        = 0x07,
        .device_sel    = 0x00,
        .pa_lut        = 0x01,
    };
    if(sx126x_set_pa_cfg(device, &pa_cfg) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set PA configuration");
        return ESP_FAIL;
    }
    if(sx126x_set_tx_params(device, 12, SX126X_RAMP_200_US) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set TX parameters");
        return ESP_FAIL;
    }
    if(sx126x_set_ocp_value(device, SX126X_OCP_PARAM_VALUE_140_MA) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set OCP value");
        return ESP_FAIL;
    }

    // Allow interrupts and wakeup on DIO1
    if(gpio_wakeup_enable(P_LORA_DIO_1, GPIO_INTR_HIGH_LEVEL) != ESP_OK){
        ESP_LOGE(TAG, "Failed to enable GPIO wakeup on DIO1");
        return ESP_FAIL;
    }
    if(gpio_sleep_sel_dis(P_LORA_DIO_1) != ESP_OK){
        ESP_LOGE(TAG, "Failed to disable GPIO sleep select on DIO1");
        return ESP_FAIL;
    }
    if(esp_sleep_enable_gpio_wakeup() != ESP_OK){
        ESP_LOGE(TAG, "Failed to enable GPIO wakeup");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_set_intr_type(P_LORA_DIO_1, GPIO_INTR_POSEDGE));

    return ESP_OK;
}

esp_err_t lora_start_rx_polling(sx126x_device_t *device, uint8_t * rx_buffer, uint16_t rx_buffer_size){
    if(!rx_buffer || rx_buffer_size == 0){
        ESP_LOGE(TAG, "RX buffer is NULL or size is 0");
        return ESP_FAIL;
    }

    // SX126X_RX_CONTINUOUS is an RTC-step value (0xFFFFFF), not milliseconds.
    // sx126x_set_rx() takes ms and rejects values > SX126X_MAX_TIMEOUT_IN_MS,
    // so the RTC-step variant must be used for continuous receive.
    sx126x_chip_status_t radio_status;
    if(sx126x_set_rx_with_timeout_in_rtc_step(device, SX126X_RX_CONTINUOUS) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to set RX mode");
    }
    sx126x_get_status(device, &radio_status);
    ESP_LOGI(TAG, "rx cmd_status=%d chip_mode=%d", radio_status.cmd_status, radio_status.chip_mode);

    while(radio_status.cmd_status != SX126X_CMD_STATUS_DATA_AVAILABLE) {
        vTaskDelay(pdMS_TO_TICKS(200));
        sx126x_get_status(device, &radio_status);
    }

    sx126x_rx_buffer_status_t rx_buffer_status;
    sx126x_get_rx_buffer_status(device, &rx_buffer_status);
    ESP_LOGI(TAG, "rx_buffer_status: pld_len_in_bytes=%d, buffer_start_pointer=%d", rx_buffer_status.pld_len_in_bytes, rx_buffer_status.buffer_start_pointer);

    sx126x_read_buffer(device, rx_buffer_status.buffer_start_pointer, rx_buffer, rx_buffer_status.pld_len_in_bytes);
    ESP_LOG_BUFFER_HEX(TAG, rx_buffer, rx_buffer_status.pld_len_in_bytes);

    sx126x_pkt_status_lora_t pkt_status;
    sx126x_get_lora_pkt_status(device, &pkt_status);
    ESP_LOGI(TAG, "pkt_status: rssi_pkt_in_dbm=%d, snr_pkt_in_db=%d, signal_rssi_pkt_in_dbm=%d", pkt_status.rssi_pkt_in_dbm, pkt_status.snr_pkt_in_db, pkt_status.signal_rssi_pkt_in_dbm);


    return ESP_OK;
}

// Private
// Switches the radio between TX and RX mode
bool lora_set_radio_mode(sx126x_device_t *device, lora_radio_mode_t mode){
    if(mode == LORA_MODE_RX){
        // Turn FEM to RX mode
        if(set_fem_mode(true) != ESP_OK){
            ESP_LOGE(TAG, "Failed to set FEM mode to RX");
            return false;
        }

        // Enable RX interrupt and thus disable tx interrupts
        if(sx126x_set_dio_irq_params(device, SX126X_IRQ_RX_DONE, SX126X_IRQ_RX_DONE, 0, 0) != SX126X_STATUS_OK) {
            ESP_LOGE(TAG, "Failed to set RX interrupt");
            return false;
        }

        // Set meshcore packet parameters
        sx126x_pkt_params_lora_t pkt_params = {
            .preamble_len_in_symb = 16,
            .header_type          = SX126X_LORA_PKT_EXPLICIT,
            .pld_len_in_bytes     = 255, // Set max packet size
            .crc_is_on            = true,
            .invert_iq_is_on      = false,
        };
        if(sx126x_set_lora_pkt_params(device, &pkt_params) != SX126X_STATUS_OK) {
            ESP_LOGE(TAG, "Failed to set LoRa packet parameters");
            return false;
        }

        // Hook the RX ISR to the DIO1 interrupt
        gpio_isr_handler_remove(P_LORA_DIO_1); // Remove any existing handler
        ESP_ERROR_CHECK(gpio_isr_handler_add(P_LORA_DIO_1, meshcore_dio1_isr,
                             (void *) lora_rx_task_handle));

        // Set extra sensitive receive
        if(sx126x_cfg_rx_boosted(device, true) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set extra sensitive receive");
        }

        // Put the radio in continous receive mode
        if(sx126x_set_rx_with_timeout_in_rtc_step(device, SX126X_RX_CONTINUOUS) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set RX mode");
            return false;
        }
        return true;
    }else if(mode == LORA_MODE_TX){
        // Enable TX interrupt and thus disable rx interrupts
        if(sx126x_set_dio_irq_params(device, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT, 0, 0) != SX126X_STATUS_OK) {
            ESP_LOGE(TAG, "Failed to set TX interrupt");
            return false;
        }

        // Hook the TX ISR to the DIO1 interrupt
        gpio_isr_handler_remove(P_LORA_DIO_1); // Remove any existing handler
        ESP_ERROR_CHECK(gpio_isr_handler_add(P_LORA_DIO_1, meshcore_dio1_isr,
                             (void *) lora_tx_task_handle));


        // Turn FEM to TX mode
        set_fem_mode(false);
        if(set_fem_mode(false) != ESP_OK){
            ESP_LOGE(TAG, "Failed to set FEM mode to TX");
            return false;
        }

        // Put the radio in TX mode
        if(sx126x_set_tx(device, 3000) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set TX mode");
            return false;
        }
        return true;
    }else if(mode == LORA_MODE_CAD){
        // Switch to standby mode first with TXCO on because we want to go to CAD directly after
        if(sx126x_set_standby(device, SX126X_STANDBY_CFG_XOSC) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set standby mode");
            return false;
        }

        // Turn FEM to RX mode
        set_fem_mode(true);
        if(set_fem_mode(true) != ESP_OK){
            ESP_LOGE(TAG, "Failed to set FEM mode to RX");
            return false;
        }

        sx126x_cad_params_t cad_params = {
            .cad_symb_nb = SX126X_CAD_02_SYMB,
            .cad_detect_peak = LORA_CAD_PEAK_THRESHOLD,
            .cad_detect_min = LORA_CAD_MIN_THRESHOLD,
            .cad_exit_mode = SX126X_CAD_ONLY,
            .cad_timeout = 20,
        };

        // Set CAD Done and CADDetected IRQs
        // We don't output those IRQs to DIO1 because we will poll for them.
        if(sx126x_set_dio_irq_params(device, SX126X_IRQ_CAD_DONE | SX126X_IRQ_CAD_DETECTED, 0, 0, 0) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set CAD IRQ parameters");
            return false;
        }
        
        if(sx126x_set_cad_params(device, &cad_params) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set CAD parameters");
            return false;
        }
        if(sx126x_set_cad(device) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to set CAD mode");
            return false;
        }
        return true;
    }

    return false;
}

// Public
void lora_rx_task(void *pvParameters){
    const char *TAG = "lora_rx_task";
    lora_rxtx_task_data_t *task_data = (lora_rxtx_task_data_t*)pvParameters;

    // Set the radio to RX mode
    if(!lora_set_radio_mode(task_data->device, LORA_MODE_RX)){
        ESP_LOGE(TAG, "Failed to set radio to RX mode");
        return;
    }

    ESP_LOGI(TAG, "RX setup complete, waiting for data...");
    while(1){
        // Block here until the DIO1 ISR (or EXT0 wakeup) delivers a notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(task_data->device->mutex, portMAX_DELAY);


        // Reset IRQ status inside the SX126X device
        if(sx126x_get_and_clear_irq_status(task_data->device, NULL) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to get and clear IRQ status");
            goto fail;
        }

        // Get the RX buffer status
        sx126x_rx_buffer_status_t rx_buffer_status;
        if(sx126x_get_rx_buffer_status(task_data->device, &rx_buffer_status) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to get RX buffer status");
            goto fail;
        }

        // Read the RX buffer into DMA aligned buffer, we dont read directly into the packet data because the packet data is not DMA aligned.
        if(sx126x_read_buffer(task_data->device, rx_buffer_status.buffer_start_pointer, task_rx_buffer, rx_buffer_status.pld_len_in_bytes) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to read RX buffer");
            goto fail;
        }
        ESP_LOG_BUFFER_HEX(TAG, task_rx_buffer, rx_buffer_status.pld_len_in_bytes);

        // Get details about the received packet
        sx126x_pkt_status_lora_t pkt_status;
        if(sx126x_get_lora_pkt_status(task_data->device, &pkt_status) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to get LoRa packet status");
            goto fail;
        }
        ESP_LOGD(TAG, "pkt_status: rssi_pkt_in_dbm=%d, snr_pkt_in_db=%d, signal_rssi_pkt_in_dbm=%d", pkt_status.rssi_pkt_in_dbm, pkt_status.snr_pkt_in_db, pkt_status.signal_rssi_pkt_in_dbm);

        // Create the packet struct
        lora_packet_t packet = {
            .data_len = rx_buffer_status.pld_len_in_bytes,
            .rssi = pkt_status.rssi_pkt_in_dbm,
            .snr = pkt_status.snr_pkt_in_db,
            .signal_rssi = pkt_status.signal_rssi_pkt_in_dbm,
        };
        if(memcpy(packet.data, task_rx_buffer, rx_buffer_status.pld_len_in_bytes) != 0){
            ESP_LOGE(TAG, "Failed to copy RX buffer to lora packet data");
            goto fail;
        }

        // Put the packet in the output queue
        if(xQueueSend(task_data->queue, &packet, portMAX_DELAY) != pdPASS){
            ESP_LOGE(TAG, "Failed to send packet to queue");
            goto fail;
        }


        fail:
        xSemaphoreGive(task_data->device->mutex);
    }

}

// Public
void lora_tx_task(void *pvParameters){
    const char *TAG = "lora_tx_task";
    lora_rxtx_task_data_t *task_data = (lora_rxtx_task_data_t*)pvParameters;

    ESP_LOGI(TAG, "TX setup complete, waiting for data...");
    while(1){
        // Wait for data to be available in the input queue
        lora_packet_t packet;
        if(xQueueReceive(task_data->queue, &packet, portMAX_DELAY) != pdPASS){
            ESP_LOGE(TAG, "Failed to receive packet from queue");
            return;
        }
        xSemaphoreTake(task_data->device->mutex, portMAX_DELAY);

        ESP_ERROR_CHECK(set_white_led(true));

        ESP_LOGI(TAG, "Sending packet with length %d", packet.data_len);
        ESP_LOG_BUFFER_HEX(TAG, packet.data, packet.data_len);

        // Write data to TX buffer
        if(sx126x_write_buffer(task_data->device, 0, packet.data, packet.data_len) != SX126X_STATUS_OK){
            ESP_LOGE(TAG, "Failed to write data to TX buffer, throw away packet and back to RX mode");
            goto switch_rx;
        }

        // Set meshcore packet parameters
        sx126x_pkt_params_lora_t pkt_params = {
            .preamble_len_in_symb = 16,
            .header_type          = SX126X_LORA_PKT_EXPLICIT,
            .pld_len_in_bytes     = packet.data_len,
            .crc_is_on            = true,
            .invert_iq_is_on      = false,
        };
        if(sx126x_set_lora_pkt_params(task_data->device, &pkt_params) != SX126X_STATUS_OK) {
            ESP_LOGE(TAG, "Failed to set LoRa packet size to %d", packet.data_len);
            goto switch_rx;
        }

        // Go into CAD mode to check for channel activity and wait for channel to be free.
        bool channel_free = false;
        for(uint8_t i = 0; i < LORA_CAD_MAX_TRIES; i++){
            // Set the radio to CAD mode to make sure the channel is free
            if(!lora_set_radio_mode(task_data->device, LORA_MODE_CAD)){
                ESP_LOGE(TAG, "Failed to set radio to CAD mode, throw away packet and back to RX mode");
                goto switch_rx;
            }

            // Get and clear IRQ status
            uint8_t counter = 5;
            sx126x_irq_mask_t irq_status = 0;
            while(counter > 0){ // Polling wait for CAD to finish or timeout
                if(sx126x_get_and_clear_irq_status(task_data->device, &irq_status) != SX126X_STATUS_OK){
                    ESP_LOGE(TAG, "Failed to get and clear IRQ status");
                    counter--;
                    continue;
                }

                if(irq_status & SX126X_IRQ_CAD_DONE){
                    ESP_LOGI(TAG, "CAD done");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
                counter--;
            }
            if(counter > 0 && !(irq_status & SX126X_IRQ_CAD_DETECTED)){
                ESP_LOGI(TAG, "CAD not detected, channel is free");
                channel_free = true;
                break;
            }

            ESP_LOGI(TAG, "CAD not detected, channel is not free, waiting for %dms", 81 * i);
            vTaskDelay(pdMS_TO_TICKS(81 * i));
        }

        if(!channel_free){
            ESP_LOGW(TAG, "Channel is not free after %d tries, throwing away packet and back to RX mode", LORA_CAD_MAX_TRIES);
            goto switch_rx;
        }


        // Set the radio to TX mode
        if(!lora_set_radio_mode(task_data->device, LORA_MODE_TX)){
            ESP_LOGE(TAG, "Failed to set radio to TX mode, throw away packet and back to RX mode");
            goto switch_rx;
        }

        // Wait for TX to finish or timeout after 2500ms
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2500));
        ESP_LOGI(TAG, "Packet probably sent, going back to RX mode");

        switch_rx:
        while(lora_set_radio_mode(task_data->device, LORA_MODE_RX) == false){
            // We keep trying here because we will deadlock if RX mode is not restored.
            ESP_LOGI(TAG, "Failed to restore RX mode, retrying...");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ESP_ERROR_CHECK(set_white_led(false));
        ESP_LOGI(TAG, "RX mode restored");
        xSemaphoreGive(task_data->device->mutex);
    }


}