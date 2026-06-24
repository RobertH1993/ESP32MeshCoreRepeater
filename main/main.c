/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */


 
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "meshcore/meshcore.h"
#include "nvs_flash.h"
#include "heltecv4.h"
#include "lora.h"
#include "meshcore/mesh.h"
#include <string.h>
#include "esp_random.h"
#include "ed25519/ed25519.h"
#include "driver/i2c_master.h"
#include "board.h"

static const char TAG[] = "main";

#define MESHCORE_IDENTITY_NVS_NAMESPACE "meshcore"
#define MESHCORE_IDENTITY_NVS_KEY       "identity"
#define MESHCORE_IDENTITY_NVS_VERSION   1


/*
Todo:
    - Add learned contacts to NVS storage or on an SD card
    - Find the best datastructure to store contacts and channels

*/

esp_err_t generate_identity(meshcore_identity_t *out){
    if(!out) return ESP_ERR_INVALID_ARG;

    // Generate a random identity
    uint8_t seed[32] = {0};
    esp_fill_random(seed, 32);
    ed25519_create_keypair((unsigned char*)out->key_pair.pub_key, (unsigned char*)out->key_pair.priv_key, seed);
    ESP_LOGI(TAG, "Public key:");
    ESP_LOG_BUFFER_HEX(TAG, out->key_pair.pub_key, 32);
    out->has_location = false;
    snprintf(out->name, sizeof(out->name), "ESP32MeshCoreRepeater-%u", (uint16_t)(esp_random() % 10000));


    ESP_LOGI(TAG, "Identity generated");
    return ESP_OK;
}

esp_err_t load_or_create_identity(meshcore_identity_t *out){
    if(!out) return ESP_ERR_INVALID_ARG;

    // Open the NVS handle
    nvs_handle_t identity_nvs_handle = 0;
    if(nvs_open(MESHCORE_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &identity_nvs_handle) != ESP_OK){
        ESP_LOGE(TAG, "Failed to open NVS identity handle");
        return ESP_ERR_NVS_NOT_FOUND;
    }

    // Get the identity from NVS
    esp_err_t ret = ESP_FAIL;
    size_t out_size = sizeof(meshcore_identity_t);
    ret = nvs_get_blob(identity_nvs_handle, MESHCORE_IDENTITY_NVS_KEY, out, &out_size);
    if(ret == ESP_ERR_NVS_NOT_FOUND){
        ESP_LOGE(TAG, "Failed to get NVS identity");
        ESP_ERROR_CHECK(generate_identity(out));
        ESP_ERROR_CHECK(nvs_set_blob(identity_nvs_handle, MESHCORE_IDENTITY_NVS_KEY, out, sizeof(meshcore_identity_t)));
        ESP_ERROR_CHECK(nvs_commit(identity_nvs_handle));
        ret = ESP_OK;
    }

    // Close the NVS handle
    nvs_close(identity_nvs_handle);
    return ret;
}



void app_main(void)
{
    // Initialize NVS (non-volatile storage). Erase and re-init if the
    // partition is full from a new version or otherwise unusable.
    ESP_LOGI(TAG, "Init NVS");
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), reformatting", esp_err_to_name(nvs_ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Enable dynamic power management, the ESP32-S3 is fairly overpowered.
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    ESP_ERROR_CHECK(set_vext(false));

    // Initialize I2C bus
    ESP_LOGI(TAG, "Init I2C");
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_BOARD_SDA,
        .scl_io_num = PIN_BOARD_SCL,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));
    ESP_LOGI(TAG, "I2C bus initialized");
    ESP_ERROR_CHECK(white_led_init());

    // static: these structs must outlive app_main; local variables would be
    // freed when app_main returns, leaving the rx_task with dangling pointers.
    static sx126x_device_t lora_radio = {0};
    lora_radio.mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(sx126x_device_init(&lora_radio));
    ESP_LOGI(TAG, "SX126X device initialized");

    lora_init_radio(&lora_radio, LORA_BAND_EU868, SX126X_LORA_SF7, SX126X_LORA_BW_062, SX126X_LORA_CR_4_5);
    ESP_LOGI(TAG, "Lora radio initialized");

    // Generate the identity
    static RTC_FAST_ATTR meshcore_identity_t meshcore_identity = {0};

    ESP_ERROR_CHECK(load_or_create_identity(&meshcore_identity));
    ESP_LOG_BUFFER_HEX(TAG, meshcore_identity.key_pair.pub_key, 32);
    ESP_LOG_BUFFER_HEX(TAG, meshcore_identity.key_pair.priv_key, 64);
    ESP_LOGI(TAG, "Name: %s", meshcore_identity.name);
    ESP_LOGI(TAG, "Has location: %d", meshcore_identity.has_location);

    // Start the tasks
    static lora_rxtx_task_data_t lora_rx_task_data = {0};
    lora_rx_task_data.device = &lora_radio;
    lora_rx_task_data.queue = xQueueCreate(2, sizeof(lora_packet_t));

    static lora_rxtx_task_data_t lora_tx_task_data = {0};
    lora_tx_task_data.device = &lora_radio;
    lora_tx_task_data.queue = xQueueCreate(4, sizeof(lora_packet_t));

    static meshcore_decoder_task_data_t meshcore_decoder_task_data = {0};
    meshcore_decoder_task_data.input_queue = lora_rx_task_data.queue;
    meshcore_decoder_task_data.output_queue = lora_tx_task_data.queue;
    meshcore_decoder_task_data.identity = &meshcore_identity;


    static meshcore_repeater_advertise_task_data_t meshcore_repeater_advertise_task_data = {0};
    meshcore_repeater_advertise_task_data.output_queue = lora_tx_task_data.queue;
    meshcore_repeater_advertise_task_data.identity = &meshcore_identity;


    // Start the meshcore tasks
    xTaskCreatePinnedToCore(lora_rx_task, "lora_rx_task", 4096, &lora_rx_task_data, 2, &lora_rx_task_handle, 1);
    xTaskCreatePinnedToCore(lora_tx_task, "lora_tx_task", 4096, &lora_tx_task_data, 1, &lora_tx_task_handle, 1);
    xTaskCreatePinnedToCore(meshcore_decoder_task, "meshcore_decoder_task", 8192, &meshcore_decoder_task_data, 1, NULL, 1);
    xTaskCreatePinnedToCore(meshcore_repeater_advertise_task, "meshcore_repeater_advertise_task", 4096, &meshcore_repeater_advertise_task_data, 1, NULL, 1);
}
