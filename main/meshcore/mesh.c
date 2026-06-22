#include "mesh.h"
#include "lora.h"
#include "meshcore.h"
#include "meshcore_packet.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora.h"
#include <sys/time.h>

static const char *TAG = "mesh";


// Number of seen hashes to store
#define SEEN_HASHES_SIZE 16
#define MAX_BUCKETS 4
#define MAX_CONTACTS_PER_BUCKET 16

// Stores the hashes of the packets we have seen
static uint32_t seen_hashes[SEEN_HASHES_SIZE] = {0};
static uint8_t seen_hashes_write_index = 0;


// Stores the contacts we have seen depending on the number of hops
static meshcore_contact_t* contact_buckets[MAX_BUCKETS][MAX_CONTACTS_PER_BUCKET] = {0};
static uint8_t contact_storage[MAX_BUCKETS * MAX_CONTACTS_PER_BUCKET * sizeof(meshcore_contact_t)] = {0};
static uint64_t contact_storage_occupied_flag = 0;

// Stores the number of messages of each type this node has seen since last reset
static uint16_t message_type_count[16] = {0};

// Private
// Gets the number of hops and transforms it into an index for the repeater buckets
// We have a total of 4 buckets
static uint8_t hops_to_index(uint8_t num_hops){
    if(num_hops <= 1){ // Direct and neighbours
        return num_hops;
    }else if(num_hops > 1 && num_hops <= 4){ // Near range
        return 2;
    }else if(num_hops > 4 && num_hops <= 8){ // Medium range
        return 3;
    }
    return 4; // Long range
}

void meshcore_store_contact_from_advert(meshcore_packet_t *pkt){
    if(!pkt) return;
    if(pkt->payload_type != MESHCORE_PAYLOAD_ADVERT) return;
    if(contact_storage_occupied_flag == UINT64_MAX) return;

    // Get the first free storage spot
    uint8_t contact_storage_index = __builtin_clzll(~contact_storage_occupied_flag);
    contact_storage_occupied_flag |= (1ULL << contact_storage_index);
    meshcore_contact_t* new_contact = (meshcore_contact_t*)&contact_storage[contact_storage_index * sizeof(meshcore_contact_t)];

    memcpy(new_contact, pkt->payload.advert.public_key, 32);
    memcpy(new_contact->name, pkt->payload.advert.name, pkt->payload.advert.name_len);
    new_contact->name[pkt->payload.advert.name_len] = '\0';
    new_contact->last_seen = xTaskGetTickCount(); // FIXME: Use a more accurate timestamp

    // Add the contact to the bucket
    contact_buckets[hops_to_index(pkt->hop_count)][contact_storage_index] = new_contact;
}

// Private
bool handle_encrypted_packet(meshcore_packet_t *pkt){
    if(!pkt->payload_raw) return false;
    if(!pkt->payload.encrypted.src_hash) return false;
    if(!pkt->payload.encrypted.dest_hash) return false;


    // Far packets should be repeated because spoof protection should be done by first degree and second degree repeaters
    if(pkt->hop_count >= 1) return true;
    
    return true;
}

bool save_advert_contact(meshcore_packet_t *pkt){
    if(!pkt) return false;
    if(pkt->payload_type != MESHCORE_PAYLOAD_ADVERT) return false;

    



    return true;
}


// Private
bool prepare_for_repeat(meshcore_packet_t *pkt, meshcore_identity_t *identity){
    // Check max hops
    if((pkt->hop_count + 1) * pkt->hash_size > MESHCORE_MAX_PATH_SIZE) return false;

    // Check if we are already in the path
    uint8_t self_seen = 0;
    for(uint8_t i = 0; i < pkt->hop_count; i++){
        if(memcmp(&pkt->path[i * pkt->hash_size], identity->key_pair.pub_key, pkt->hash_size) == 0){
            self_seen++;
            if(self_seen >= (4 - pkt->hash_size)){ // Depending on the hash size we allow a few collisions
                ESP_LOGI(TAG, "We are already in the path too many times, skipping");
                return false;
            }
        }
    }

    // Add ourself to the path and increment hop count
    memcpy(&pkt->path[pkt->hop_count * pkt->hash_size], identity->key_pair.pub_key, pkt->hash_size);
    pkt->hop_count++;

    return true;
}


// public
void meshcore_decoder_task(void *pvParameters){
    const char *TAG = "meshcore_decoder_task";

    meshcore_decoder_task_data_t *task_data = (meshcore_decoder_task_data_t*)pvParameters;
    lora_packet_t packet = {0};


    ESP_LOGI(TAG, "Meshcore decoder task started");
    while(1){
        // Wait for data to be available in the input queue
        xQueueReceive(task_data->input_queue, &packet, portMAX_DELAY);

        // Parse the packet header
        meshcore_packet_t pkt;
        if(!meshcore_packet_parse_header(packet.data, packet.data_len, &pkt)){
            ESP_LOGE(TAG, "Failed to parse packet header");
            goto next_packet;
        }

        // Show the packet hash
        ESP_LOGI(TAG, "packet hash: %02X%02X%02X%02X%02X%02X%02X%02X",
                pkt.hash[0], pkt.hash[1], pkt.hash[2], pkt.hash[3],
                pkt.hash[4], pkt.hash[5], pkt.hash[6], pkt.hash[7]);
        
        // Check if the packet hash has been seen before, if so skip it
        uint32_t hash;
        memcpy(&hash, pkt.hash, sizeof(uint32_t));
        for(uint8_t i = 0; i < SEEN_HASHES_SIZE; i++){
            if(seen_hashes[i] == hash){
                ESP_LOGI(TAG, "packet hash already seen");
                goto next_packet;
            }
        }
        seen_hashes[seen_hashes_write_index] = hash;
        seen_hashes_write_index = (seen_hashes_write_index + 1) % SEEN_HASHES_SIZE;

        message_type_count[pkt.payload_type]++;

        lora_packet_t repeat_packet_lora = {0};

        // Handle flood packets
        if(meshcore_packet_is_flood_packet(&pkt)){
            ESP_LOGI(TAG, "Got flood packet");

            // Some packet types have extra validation steps
            if(pkt.payload_type == MESHCORE_PAYLOAD_ADVERT){
                ESP_LOGI(TAG, "Got advert packet");
                if(!meshcore_packet_parse_payload(&pkt)){
                    ESP_LOGE(TAG, "Failed to parse advert packet body");
                    goto next_packet;
                }
                if(!meshcore_packet_validate_advert(&pkt)) goto next_packet;
            }

            ESP_LOGI(TAG, "Got %s packet", meshcore_payload_type_name(pkt.payload_type));
            if(!prepare_for_repeat(&pkt, task_data->identity)){
                ESP_LOGE(TAG, "Failed to prepare advert packet for repeat");
                goto next_packet;
            }

            if(!meshcore_packet_to_bytes(&pkt, repeat_packet_lora.data, &repeat_packet_lora.data_len)){
                ESP_LOGE(TAG, "Failed to convert advert packet to bytes");
                goto next_packet;
            }
            if(xQueueSend(task_data->output_queue, &repeat_packet_lora, portMAX_DELAY) != pdPASS){
                ESP_LOGE(TAG, "Failed to send repeat packet to output queue");
                goto next_packet;
            }
            ESP_LOGI(TAG, "Sent repeat packet");

        }else if(meshcore_packet_is_direct_packet(&pkt)){
            ESP_LOGI(TAG, "Got direct packet");
        }else{
            ESP_LOGI(TAG, "Got unknown packet");
        }


            // Send the packet 1 : 1 to the output queue
            //if(xQueueSend(task_data->output_queue, &packet, portMAX_DELAY) != pdPASS){
            //    ESP_LOGE(TAG, "Failed to send packet to output queue");
            //    goto next_packet;
            //}


            /*
            if (pkt.payload_type == MESHCORE_PAYLOAD_ADVERT && pkt.payload.advert.has_name) {
                ESP_LOGI(TAG, "node name: %.*s",
                        pkt.payload.advert.name_len,
                        pkt.payload.advert.name);
                
                if(pkt.payload.advert.has_location){
                    ESP_LOGI(TAG, "latitude: %d, longitude: %d", pkt.payload.advert.latitude, pkt.payload.advert.longitude);
                }

                if(pkt.payload.advert.node_type == MESHCORE_NODE_UNKNOWN){
                    ESP_LOGI(TAG, "node type: unknown");
                }else if(pkt.payload.advert.node_type == MESHCORE_NODE_CHAT){
                    ESP_LOGI(TAG, "node type: chat");
                }else if(pkt.payload.advert.node_type == MESHCORE_NODE_REPEATER){
                    ESP_LOGI(TAG, "node type: repeater");
                }else if(pkt.payload.advert.node_type == MESHCORE_NODE_ROOM){
                    ESP_LOGI(TAG, "node type: room");
                }else if(pkt.payload.advert.node_type == MESHCORE_NODE_SENSOR){
                    ESP_LOGI(TAG, "node type: sensor");
                }

            }
            //if (pkt.payload_type == MESHCORE_PAYLOAD_TXT_MSG){
            //    ESP_LOGI(TAG, "-=== Private message ===-");
            //    ESP_LOGI(TAG, "src_hash: %02X", pkt.payload.encrypted.src_hash);
           //     ESP_LOGI(TAG, "dest_hash: %02X", pkt.payload.encrypted.dest_hash);
            //}
            if(pkt.payload_type == MESHCORE_PAYLOAD_GRP_TXT){
                ESP_LOGI(TAG, "-=== Group message ===-");
                ESP_LOGI(TAG, "channel_hash: %02X", pkt.payload.group.channel_hash);
                ESP_LOGI(TAG, "cipher_mac: %04X", pkt.payload.group.cipher_mac);
                ESP_LOGI(TAG, "ciphertext_len: %d", pkt.payload.group.ciphertext_len);
                ESP_LOG_BUFFER_HEX(TAG, pkt.payload.group.ciphertext, pkt.payload.group.ciphertext_len);
            }
            */
            next_packet:
    }

}

// Public
// This task is responsible for advertising the repeater's identity to the network.
// It only generates an advert packet and puts it into the output queue.
void meshcore_repeater_advertise_task(void *pvParameters){
    const char *TAG = "meshcore_repeater_advertise_task";

    meshcore_repeater_advertise_task_data_t *task_data = (meshcore_repeater_advertise_task_data_t*)pvParameters;

    TickType_t last_advertise_interval = xTaskGetTickCount();
    const TickType_t advertise_interval = pdMS_TO_TICKS(30 * 60 * 1000); // 30 minutes

    ESP_LOGI(TAG, "Meshcore repeater advertise task started");
    while(1){
        ESP_LOGI(TAG, "Advertising repeater identity");
        // On first startup we want to send an advert packet but wait for the TX task to be ready
        vTaskDelay(pdMS_TO_TICKS(5000));

        lora_packet_t advert_packet_lora = {0};
        if(!meshcore_packet_create_advert(MESHCORE_ROUTE_FLOOD, MESHCORE_NODE_REPEATER, task_data->identity, xTaskGetTickCount(), advert_packet_lora.data, &advert_packet_lora.data_len)){
            ESP_LOGE(TAG, "Failed to create advert packet");
        }
        ESP_LOGI(TAG, "Sending out advert packet");

        if(xQueueSend(task_data->output_queue, &advert_packet_lora, portMAX_DELAY) != pdPASS){
            ESP_LOGE(TAG, "Failed to send advert packet to output queue");
        }

        last_advertise_interval = xTaskGetTickCount();
        vTaskDelayUntil(&last_advertise_interval, advertise_interval);
    }




}