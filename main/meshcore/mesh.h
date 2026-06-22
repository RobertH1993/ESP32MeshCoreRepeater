#ifndef MESHCORE_MESH_H
#define MESHCORE_MESH_H

#include <stdint.h>
#include "meshcore.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif



typedef struct {
    QueueHandle_t input_queue; // LoRa Packet INPUT queue
    QueueHandle_t output_queue; // LoRa Packet OUTPUT queue, NOT meshcore packets!
    meshcore_identity_t *identity;
} meshcore_decoder_task_data_t;


typedef struct {
    QueueHandle_t output_queue; // LoRa Packet OUTPUT queue, NOT meshcore packets!
    meshcore_identity_t *identity;
} meshcore_repeater_advertise_task_data_t;


/**
 * @brief Receives LoRa packets and decodes them into meshcore packets.
 * @param pvParameters meshcore_decoder_task_data_t, input queue with lora packets, output queue also with lora packets
 */
void meshcore_decoder_task(void *pvParameters);

/**
 * @brief Advertises the repeater's identity to the network.
 * @param pvParameters meshcore_repeater_advertise_task_data_t, output queue with lora packets, identity of the repeater
 */
void meshcore_repeater_advertise_task(void *pvParameters);



#ifdef __cplusplus
}
#endif

#endif