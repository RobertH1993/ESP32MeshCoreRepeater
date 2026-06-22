#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "meshcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

 #define MESHCORE_PACKET_HASH_SIZE 8u

#define MESHCORE_MAX_PATH_SIZE        64u
#define MESHCORE_MAX_PACKET_PAYLOAD   184u
#define MESHCORE_ADVERT_PUBKEY_SIZE   32u
#define MESHCORE_ADVERT_SIG_SIZE      64u
#define MESHCORE_ANON_PUBKEY_SIZE     32u

#define MESHCORE_ADVERT_FLAG_HAS_LOCATION  0x10u
#define MESHCORE_ADVERT_FLAG_HAS_FEATURE1  0x20u
#define MESHCORE_ADVERT_FLAG_HAS_FEATURE2  0x40u
#define MESHCORE_ADVERT_FLAG_HAS_NAME      0x80u

/* =========================================================================
 * Control sub-types  (upper nibble of the flags byte)
 * ========================================================================= */

typedef enum {
    MESHCORE_CTRL_DISCOVER_REQ  = 0x8,
    MESHCORE_CTRL_DISCOVER_RESP = 0x9,
} meshcore_ctrl_sub_type_t;

/* =========================================================================
 * TXT_MSG txt_type  (upper 6 bits of the txt_type+attempt byte)
 * ========================================================================= */

typedef enum {
    MESHCORE_TXT_PLAIN  = 0x00,  /* plain text */
    MESHCORE_TXT_CLI    = 0x01,  /* CLI command */
    MESHCORE_TXT_SIGNED = 0x02,  /* signed: 4-byte pubkey prefix + text */
} meshcore_txt_type_t;

/* =========================================================================
 * Payload structs
 *
 * NOTE: Every pointer field references memory inside the raw buffer that
 * was passed to meshcore_packet_parse().  The raw buffer must stay valid
 * for as long as the parsed struct is in use.
 * ========================================================================= */

/* REQ (0x00), RESPONSE (0x01), TXT_MSG (0x02), PATH (0x08) —
 * all share this outer encrypted envelope.                                   */
typedef struct {
    uint8_t        dest_hash;
    uint8_t        src_hash;
    uint16_t       cipher_mac;
    const uint8_t *ciphertext;       /* NULL when ciphertext_len == 0 */
    uint16_t       ciphertext_len;
} meshcore_payload_encrypted_t;

/* MESHCORE_PAYLOAD_TXT_MSG (0x02)                                             */
typedef struct {
    meshcore_payload_encrypted_t encrypted;
    uint32_t timestamp;
    meshcore_txt_type_t txt_type;
    uint8_t attempt;
    const uint8_t *text;
} meshcore_payload_txt_msg_t;

/* ANON_REQ (0x07)                                                            */
typedef struct {
    uint8_t        dest_hash;
    const uint8_t *sender_public_key; /* 32 bytes */
    uint16_t       cipher_mac;
    const uint8_t *ciphertext;
    uint16_t       ciphertext_len;
} meshcore_payload_anon_req_t;

/* ACK (0x03)                                                                 */
typedef struct {
    uint32_t checksum;               /* CRC of msg timestamp, text, pubkey */
} meshcore_payload_ack_t;

/* ADVERT (0x04)                                                              */
typedef struct {
    const uint8_t      *public_key;  /* 32 bytes (Ed25519) */
    uint32_t            timestamp;   /* unix timestamp */
    const uint8_t      *signature;   /* 64 bytes (Ed25519) */
    /* appdata — only meaningful when has_appdata is true */
    bool                has_appdata;
    uint8_t             flags;       /* raw flags byte */
    meshcore_node_type_t node_type;  /* lower nibble of flags */
    bool                has_location;
    int32_t             latitude;    /* decimal * 1 000 000 */
    int32_t             longitude;   /* decimal * 1 000 000 */
    bool                has_feature1;
    uint16_t            feature1;
    bool                has_feature2;
    uint16_t            feature2;
    bool                has_name;
    const uint8_t      *name;        /* NOT null-terminated */
    uint8_t             name_len;
} meshcore_payload_advert_t;

/* GRP_TXT (0x05), GRP_DATA (0x06)                                           */
typedef struct {
    uint8_t        channel_hash;
    uint16_t       cipher_mac;
    const uint8_t *ciphertext;
    uint16_t       ciphertext_len;
} meshcore_payload_group_t;

/* CONTROL (0x0B)                                                             */
typedef struct {
    uint8_t                  flags;
    meshcore_ctrl_sub_type_t sub_type;  /* upper nibble of flags */
    const uint8_t           *data;
    uint16_t                 data_len;
} meshcore_payload_control_t;

/* TRACE (0x09), MULTIPART (0x0A), RAW_CUSTOM (0x0F), unknown               */
typedef struct {
    const uint8_t *data;
    uint16_t       data_len;
} meshcore_payload_raw_t;

/* Discriminated union — check payload_type in meshcore_packet_t             */
typedef union {
    meshcore_payload_encrypted_t encrypted; /* REQ, RESPONSE, TXT_MSG, PATH */
    meshcore_payload_anon_req_t  anon_req;
    meshcore_payload_advert_t    advert;
    meshcore_payload_ack_t       ack;
    meshcore_payload_group_t     group;     /* GRP_TXT, GRP_DATA */
    meshcore_payload_control_t   control;
    meshcore_payload_raw_t       raw;
} meshcore_payload_data_t;

/* =========================================================================
 * Top-level parsed packet
 * ========================================================================= */

typedef struct {
    /* Outer frame */
    meshcore_route_type_t    route_type;
    meshcore_payload_type_t  payload_type;
    meshcore_payload_ver_t   payload_version;
    /* Transport codes — valid only when has_transport_codes is true */
    bool                     has_transport_codes;
    uint16_t                 transport_code_1;
    uint16_t                 transport_code_2;
    /* Path */
    uint8_t                  hop_count;
    uint8_t                  hash_size;   /* bytes per hop hash: 1, 2, or 3 */
    uint8_t                  path[MESHCORE_MAX_PATH_SIZE]; /* hop_count * hash_size bytes */
    /* Parsed payload — read via payload.<type> matching payload_type */
    uint8_t                  hash[MESHCORE_PACKET_HASH_SIZE];
    uint8_t*                 payload_raw;
    uint16_t                 payload_raw_len;
    meshcore_payload_data_t  payload;
} meshcore_packet_t;

/* =========================================================================
 * API
 * ========================================================================= */

/**
 * Parse a raw MeshCore wire-format packet (as received over LoRa).
 *
 * @param raw  Pointer to raw bytes.
 * @param len  Number of valid bytes in @p raw.
 * @param out  Output struct, filled on success.
 * @return true on success, false when the buffer is too short or malformed.
 *
 * Pointer fields inside @p out reference memory within @p raw.
 * Keep @p raw valid for as long as @p out is used.
 */
bool meshcore_packet_parse_header(const uint8_t *raw, uint16_t len,
                           meshcore_packet_t *out);

/**
 * Parse the payload of a packet.
 * @param raw Pointer to the raw payload.
 * @param len Number of valid bytes in @p raw.
 * @param out Pointer to the output struct.
 * @return true on success, false when the buffer is too short or malformed.
 */
bool meshcore_packet_parse_payload(meshcore_packet_t *out);

/**
 * Create a repeat packet.
 * @param pkt Pointer to the packet.
 * @param identity Pointer to the identity.
 * @return true on success, false when the packet is malformed.
 */
bool meshcore_packet_to_bytes(meshcore_packet_t *pkt, uint8_t *out, uint16_t *out_len);

/**
 * Create the header of a packet.
 * @param route_type The route type of the packet.
 * @param payload_type The payload type of the packet.
 * @param payload_version The payload version of the packet.
 * @return The header of the packet.
 */
uint8_t meshcore_packet_create_header(meshcore_route_type_t route_type, meshcore_payload_type_t payload_type, meshcore_payload_ver_t payload_version);

/**
 * Calculate the hash of the packet and store it in the packet's payload_hash field.
 * @param pkt Pointer to the packet.
 * @return true on success, false when the packet is malformed.
 */
bool meshcore_packet_calculate_hash(meshcore_packet_t *pkt);

/**
 * Check if the packet is a flood packet.
 * @param pkt Pointer to the packet.
 * @return true if the packet is a flood packet, false otherwise.
 */
bool meshcore_packet_is_flood_packet(meshcore_packet_t *pkt);

/**
 * Check if the packet is a direct packet.
 * @param pkt Pointer to the packet.
 * @return true if the packet is a direct packet, false otherwise.
 */
bool meshcore_packet_is_direct_packet(meshcore_packet_t *pkt);

/**
 * Return a human-readable name for a payload type (useful for logging).
 */
const char *meshcore_payload_type_name(meshcore_payload_type_t type);

/**
 * Validate an advert packet.
 * @param pkt Pointer to the packet.
 * @return true on success, false when the packet is malformed.
 */
 bool meshcore_packet_validate_advert(meshcore_packet_t *pkt);

/**
 * Create an advert packet.
 * @param route_type The route type of the packet.
 * @param node_type The node type of the packet.
 * @param identity The identity to advertise.
 * @param packet Pointer to the packet buffer.
 * @param packet_len Pointer to the packet length.
 * @return true on success, false when the input is invalid.
 */
bool meshcore_packet_create_advert(meshcore_route_type_t route_type, meshcore_node_type_t node_type, meshcore_identity_t *identity, uint32_t timestamp, uint8_t *packet, uint16_t *packet_len);





uint32_t meshcore_packet_rd_le32(const uint8_t *p);
uint16_t meshcore_packet_rd_le16(const uint8_t *p);
#ifdef __cplusplus
}
#endif
