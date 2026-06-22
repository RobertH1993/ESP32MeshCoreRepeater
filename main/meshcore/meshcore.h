#ifndef MESHCORE_MESHCORE_H
#define MESHCORE_MESHCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MESHCORE_ROUTE_TRANSPORT_FLOOD  = 0x00,  /* flood + transport codes */
    MESHCORE_ROUTE_FLOOD            = 0x01,  /* flood routing */
    MESHCORE_ROUTE_DIRECT           = 0x02,  /* direct routing */
    MESHCORE_ROUTE_TRANSPORT_DIRECT = 0x03,  /* direct + transport codes */
} meshcore_route_type_t;

typedef enum {
    MESHCORE_PAYLOAD_REQ        = 0x00,
    MESHCORE_PAYLOAD_RESPONSE   = 0x01,
    MESHCORE_PAYLOAD_TXT_MSG    = 0x02,
    MESHCORE_PAYLOAD_ACK        = 0x03,
    MESHCORE_PAYLOAD_ADVERT     = 0x04,
    MESHCORE_PAYLOAD_GRP_TXT    = 0x05,
    MESHCORE_PAYLOAD_GRP_DATA   = 0x06,
    MESHCORE_PAYLOAD_ANON_REQ   = 0x07,
    MESHCORE_PAYLOAD_PATH       = 0x08,
    MESHCORE_PAYLOAD_TRACE      = 0x09,
    MESHCORE_PAYLOAD_MULTIPART  = 0x0A,
    MESHCORE_PAYLOAD_CONTROL    = 0x0B,
    MESHCORE_PAYLOAD_RAW_CUSTOM = 0x0F,
} meshcore_payload_type_t;

typedef enum {
    MESHCORE_PAYLOAD_VER_1 = 0x00,
    MESHCORE_PAYLOAD_VER_2 = 0x01,
    MESHCORE_PAYLOAD_VER_3 = 0x02,
    MESHCORE_PAYLOAD_VER_4 = 0x03,
} meshcore_payload_ver_t;

typedef enum {
    MESHCORE_NODE_UNKNOWN  = 0x00,
    MESHCORE_NODE_CHAT     = 0x01,
    MESHCORE_NODE_REPEATER = 0x02,
    MESHCORE_NODE_ROOM     = 0x03,
    MESHCORE_NODE_SENSOR   = 0x04,
} meshcore_node_type_t;

typedef struct {
    uint8_t hash;
    uint8_t shared_secret[32];
    char name[32];
} meshcore_channel_t;

typedef struct {
    uint8_t pub_key[32];
    char name[32];
    uint32_t last_seen;
} meshcore_contact_t;

typedef struct __attribute__((packed)) {
    uint32_t pub_key[32];
    uint32_t priv_key[64];
} meshcore_key_pair_t;

typedef struct __attribute__((packed)) {
    meshcore_key_pair_t key_pair;
    char name[32];
    bool has_location;
    uint32_t latitude;
    uint32_t longitude;
} meshcore_identity_t;

#endif


