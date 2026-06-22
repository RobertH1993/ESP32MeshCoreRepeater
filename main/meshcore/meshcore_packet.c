#include "meshcore_packet.h"
#include "mbedtls/md.h"
#include <string.h>
#include "esp_log.h"
#include "ed25519.h"

static const char *TAG = "meshcore_packet";

/* =========================================================================
 * Little-endian helpers (MeshCore uses LE for all multi-byte integers)
 * ========================================================================= */

inline uint16_t meshcore_packet_rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

inline uint32_t meshcore_packet_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

__attribute__((unused))
static inline void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* =========================================================================
 * Per-type payload parsers
 * ========================================================================= */

/* REQ, RESPONSE, TXT_MSG, PATH — identical outer envelope:
 *   dest_hash(1) + src_hash(1) + cipher_mac(2) + ciphertext(rest)           */
static bool parse_encrypted(const uint8_t *p, uint16_t len,
                             meshcore_payload_encrypted_t *out)
{
    if (len < 4) return false;
    out->dest_hash      = p[0];
    out->src_hash       = p[1];
    out->cipher_mac     = meshcore_packet_rd_le16(&p[2]);
    out->ciphertext     = (len > 4) ? &p[4] : NULL;
    out->ciphertext_len = (len > 4) ? (uint16_t)(len - 4) : 0;
    return true;
}


/* ANON_REQ:
 *   dest_hash(1) + sender_public_key(32) + cipher_mac(2) + ciphertext(rest) */
static bool parse_anon_req(const uint8_t *p, uint16_t len,
                           meshcore_payload_anon_req_t *out)
{
    if (len < 35) return false;  /* 1 + 32 + 2 = 35 */
    out->dest_hash          = p[0];
    out->sender_public_key  = &p[1];
    out->cipher_mac         = meshcore_packet_rd_le16(&p[33]);
    out->ciphertext         = (len > 35) ? &p[35] : NULL;
    out->ciphertext_len     = (len > 35) ? (uint16_t)(len - 35) : 0;
    return true;
}

/* ACK:
 *   checksum(4)                                                              */
static bool parse_ack(const uint8_t *p, uint16_t len,
                      meshcore_payload_ack_t *out)
{
    if (len < 4) return false;
    out->checksum = meshcore_packet_rd_le32(p);
    return true;
}

/* ADVERT:
 *   public_key(32) + timestamp(4) + signature(64) = 100 bytes fixed
 *   + optional appdata: flags(1) [lat(4) lon(4)] [feat1(2)] [feat2(2)] [name]*/
static bool parse_advert(const uint8_t *p, uint16_t len,
                         meshcore_payload_advert_t *out)
{
    if (len < 100) return false;
    memset(out, 0, sizeof(*out));

    out->public_key = &p[0];
    out->timestamp  = meshcore_packet_rd_le32(&p[32]);
    out->signature  = &p[36];   /* 36 = 32 + 4 */

    if (len <= 100) {
        out->has_appdata = false;
        return true;
    }

    out->has_appdata = true;
    const uint8_t *ap   = &p[100];
    uint16_t       alen = (uint16_t)(len - 100);
    uint16_t       ai   = 0;

    out->flags     = ap[ai++];
    out->node_type = (meshcore_node_type_t)(out->flags & 0x0Fu);

    if ((out->flags & MESHCORE_ADVERT_FLAG_HAS_LOCATION) &&
        (alen - ai) >= 8) {
        out->has_location = true;
        out->latitude     = (int32_t)meshcore_packet_rd_le32(&ap[ai]); ai += 4;
        out->longitude    = (int32_t)meshcore_packet_rd_le32(&ap[ai]); ai += 4;
    }

    if ((out->flags & MESHCORE_ADVERT_FLAG_HAS_FEATURE1) &&
        (alen - ai) >= 2) {
        out->has_feature1 = true;
        out->feature1     = meshcore_packet_rd_le16(&ap[ai]); ai += 2;
    }

    if ((out->flags & MESHCORE_ADVERT_FLAG_HAS_FEATURE2) &&
        (alen - ai) >= 2) {
        out->has_feature2 = true;
        out->feature2     = meshcore_packet_rd_le16(&ap[ai]); ai += 2;
    }

    if ((out->flags & MESHCORE_ADVERT_FLAG_HAS_NAME) && ai < alen) {
        out->has_name  = true;
        out->name      = &ap[ai];
        out->name_len  = (uint8_t)(alen - ai);
    }

    return true;
}

/* GRP_TXT, GRP_DATA:
 *   channel_hash(1) + cipher_mac(2) + ciphertext(rest)                      */
static bool parse_group(const uint8_t *p, uint16_t len,
                        meshcore_payload_group_t *out)
{
    ESP_LOGI("parse_group", "parse_group: len=%d", len);
    if (len < 3) return false;
    out->channel_hash   = p[0];
    out->cipher_mac     = meshcore_packet_rd_le16(&p[1]);
    out->ciphertext     = (len > 3) ? &p[3] : NULL;
    out->ciphertext_len = (len > 3) ? (uint16_t)(len - 3) : 0;
    return true;
}

/* CONTROL:
 *   flags(1) + data(rest)   — sub_type is the upper nibble of flags          */
static bool parse_control(const uint8_t *p, uint16_t len,
                          meshcore_payload_control_t *out)
{
    if (len < 1) return false;
    out->flags    = p[0];
    out->sub_type = (meshcore_ctrl_sub_type_t)((p[0] >> 4) & 0x0Fu);
    out->data     = (len > 1) ? &p[1] : NULL;
    out->data_len = (len > 1) ? (uint16_t)(len - 1) : 0;
    return true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

bool meshcore_packet_parse_header(const uint8_t *raw, uint16_t len,
                           meshcore_packet_t *out)
{
    if (!raw || !out || len < 3) return false;
    memset(out, 0, sizeof(*out));

    uint16_t i = 0;

    /* ----- Header (1 byte): 0bVVPPPPRR ------------------------------------- */
    uint8_t hdr          = raw[i++];
    out->route_type      = (meshcore_route_type_t)(hdr & 0x03u);
    out->payload_type    = (meshcore_payload_type_t)((hdr >> 2) & 0x0Fu);
    out->payload_version = (meshcore_payload_ver_t)((hdr >> 6) & 0x03u);

    /* ----- Transport codes (4 bytes, optional) ------------------------------ */
    out->has_transport_codes =
        (out->route_type == MESHCORE_ROUTE_TRANSPORT_FLOOD ||
         out->route_type == MESHCORE_ROUTE_TRANSPORT_DIRECT);

    if (out->has_transport_codes) {
        if (i + 4u > len) return false;
        out->transport_code_1 = meshcore_packet_rd_le16(&raw[i]); i += 2;
        out->transport_code_2 = meshcore_packet_rd_le16(&raw[i]); i += 2;
    }

    /* ----- path_length (1 byte) --------------------------------------------- */
    if (i >= len) return false;
    uint8_t pl_byte = raw[i++];

    out->hop_count          = pl_byte & 63;
    out->hash_size          = (pl_byte >> 6) + 1u;
    if(out->hash_size > 3u) return false; // Reserved / Invalid

    /* ----- Path bytes ------------------------------------------------------- */
    uint16_t path_bytes = (uint16_t)out->hop_count * out->hash_size;
    if (path_bytes > MESHCORE_MAX_PATH_SIZE) return false;
    if (i + path_bytes > len) return false;
    memcpy(out->path, &raw[i], path_bytes);
    i += path_bytes;

    /* ----- Payload ---------------------------------------------------------- */
    if (i > len) return false;
    const uint8_t *payload     = &raw[i];
    uint16_t       payload_len = (uint16_t)(len - i);
    out->payload_raw = (payload_len > 0u) ? (uint8_t*)payload : NULL;
    out->payload_raw_len = payload_len;
    if (payload_len > MESHCORE_MAX_PACKET_PAYLOAD) return false;

    meshcore_packet_calculate_hash(out);
    return true;
}

bool meshcore_packet_parse_payload(meshcore_packet_t *out)
{
    if (!out || !out->payload_raw || !out->payload_raw_len) return false;

    /* Dispatch to the per-type parser */
    switch (out->payload_type) {
        case MESHCORE_PAYLOAD_REQ:
            return parse_encrypted(out->payload_raw, out->payload_raw_len, 
                                    &out->payload.encrypted);
        case MESHCORE_PAYLOAD_RESPONSE:
            return parse_encrypted(out->payload_raw, out->payload_raw_len, 
                                    &out->payload.encrypted);
        case MESHCORE_PAYLOAD_TXT_MSG:
            return parse_encrypted(out->payload_raw, out->payload_raw_len, 
                                    &out->payload.encrypted);
        case MESHCORE_PAYLOAD_PATH:
            return parse_encrypted(out->payload_raw, out->payload_raw_len,
                                   &out->payload.encrypted);
        case MESHCORE_PAYLOAD_ACK:
            return parse_ack(out->payload_raw, out->payload_raw_len, &out->payload.ack);

        case MESHCORE_PAYLOAD_ADVERT:
            return parse_advert(out->payload_raw, out->payload_raw_len, &out->payload.advert);

        case MESHCORE_PAYLOAD_GRP_TXT:
            return parse_group(out->payload_raw, out->payload_raw_len, &out->payload.group);
        case MESHCORE_PAYLOAD_GRP_DATA:
            return parse_group(out->payload_raw, out->payload_raw_len, &out->payload.group);

        case MESHCORE_PAYLOAD_ANON_REQ:
            return parse_anon_req(out->payload_raw, out->payload_raw_len,
                                  &out->payload.anon_req);

        case MESHCORE_PAYLOAD_CONTROL:
            return parse_control(out->payload_raw, out->payload_raw_len,
                                 &out->payload.control);

        case MESHCORE_PAYLOAD_TRACE:
        case MESHCORE_PAYLOAD_MULTIPART:
        case MESHCORE_PAYLOAD_RAW_CUSTOM:
    }

    return false;
}

bool meshcore_packet_to_bytes(meshcore_packet_t *pkt, uint8_t *out, uint16_t *out_len){
    if (!pkt || !out || !out_len) return false;

    uint16_t i = 0;
    // Copy header
    out[0] = meshcore_packet_create_header(pkt->route_type, pkt->payload_type, pkt->payload_version); i++;
    if(pkt->has_transport_codes){
        wr_le16(&out[i], pkt->transport_code_1); i += 2;
        wr_le16(&out[i], pkt->transport_code_2); i += 2;
    }

    // Copy path length and path
    out[i] = (uint8_t)(pkt->hop_count & 0x3F) | ((uint8_t)(pkt->hash_size - 1) << 6); i++;
    memcpy(&out[i], pkt->path, pkt->hop_count * pkt->hash_size); i += pkt->hop_count * pkt->hash_size;

    // Copy payload
    if(pkt->payload_raw != NULL && pkt->payload_raw_len > 0){
        memcpy(&out[i], pkt->payload_raw, pkt->payload_raw_len); i += pkt->payload_raw_len;
    }

    *out_len = i;
    return true;
}

uint8_t meshcore_packet_create_header(meshcore_route_type_t route_type, meshcore_payload_type_t payload_type, meshcore_payload_ver_t payload_version)
{
    return (uint8_t)((payload_version << 6) | (payload_type << 2) | (route_type & 0x03u));
}

bool meshcore_packet_calculate_hash(meshcore_packet_t *pkt)
{
    if (!pkt || !pkt->payload_raw || !pkt->payload_raw_len) return false;

    uint8_t t = (uint8_t)pkt->payload_type;
    uint8_t full_hash[32] = {0};

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, &t, sizeof(t));
    // FIXME add path_size to hash for trace packets
    mbedtls_md_update(&ctx, pkt->payload_raw, pkt->payload_raw_len);
    mbedtls_md_finish(&ctx, full_hash);
    mbedtls_md_free(&ctx);

    memcpy(pkt->hash, full_hash, MESHCORE_PACKET_HASH_SIZE);
    return true;
}

inline bool meshcore_packet_is_flood_packet(meshcore_packet_t *pkt)
{
    return pkt->route_type == MESHCORE_ROUTE_FLOOD || pkt->route_type == MESHCORE_ROUTE_TRANSPORT_FLOOD;
}

inline bool meshcore_packet_is_direct_packet(meshcore_packet_t *pkt)
{
    return pkt->route_type == MESHCORE_ROUTE_DIRECT || pkt->route_type == MESHCORE_ROUTE_TRANSPORT_DIRECT;
}

const char *meshcore_payload_type_name(meshcore_payload_type_t type)
{
    switch (type) {
        case MESHCORE_PAYLOAD_REQ:        return "REQ";
        case MESHCORE_PAYLOAD_RESPONSE:   return "RESPONSE";
        case MESHCORE_PAYLOAD_TXT_MSG:    return "TXT_MSG";
        case MESHCORE_PAYLOAD_ACK:        return "ACK";
        case MESHCORE_PAYLOAD_ADVERT:     return "ADVERT";
        case MESHCORE_PAYLOAD_GRP_TXT:    return "GRP_TXT";
        case MESHCORE_PAYLOAD_GRP_DATA:   return "GRP_DATA";
        case MESHCORE_PAYLOAD_ANON_REQ:   return "ANON_REQ";
        case MESHCORE_PAYLOAD_PATH:       return "PATH";
        case MESHCORE_PAYLOAD_TRACE:      return "TRACE";
        case MESHCORE_PAYLOAD_MULTIPART:  return "MULTIPART";
        case MESHCORE_PAYLOAD_CONTROL:    return "CONTROL";
        case MESHCORE_PAYLOAD_RAW_CUSTOM: return "RAW_CUSTOM";
        default:                          return "UNKNOWN";
    }
}

bool meshcore_packet_validate_advert(meshcore_packet_t *pkt){
    // Check if this is a direct message and validate lat/lon
    // Prevent spoofing by hashing the public key and rate limiting advertisements
    // Allow max number of advertisements per time window
    if(!pkt->payload_raw) return false;
    if(pkt->payload_raw_len < 100) return false;
    if(!pkt->payload.advert.public_key) return false;
    if(!pkt->payload.advert.signature) return false;

    // Verify the signature (signed: pubkey + timestamp + appdata)
    uint8_t to_verify[255] = {0};
    uint16_t appdata_len = (uint16_t)(pkt->payload_raw_len - 100);
    if((uint16_t)(36 + appdata_len) > sizeof(to_verify)) return false;

    memcpy(to_verify, pkt->payload.advert.public_key, 32);
    memcpy(to_verify + 32, pkt->payload_raw + 32, 4);
    if(appdata_len > 0){
        memcpy(to_verify + 36, pkt->payload_raw + 100, appdata_len);
    }
    if(!ed25519_verify(pkt->payload.advert.signature, to_verify, 32 + 4 + appdata_len, pkt->payload.advert.public_key)){
        ESP_LOGW(TAG, "Failed to verify signature of advert packet, ignoring");
        return false;
    }

    // Dont allow empty advert packets
    if(!pkt->payload.advert.has_appdata){
        ESP_LOGW(TAG, "Advert packet has no appdata, skipping");
        return false;
    }

    // Check if the packet has valid location data
    if(pkt->payload.advert.flags & MESHCORE_ADVERT_FLAG_HAS_LOCATION){
        float latitude = pkt->payload.advert.latitude / 1000000.0f;
        float longitude = pkt->payload.advert.longitude / 1000000.0f;
        if(latitude < -90.0f || latitude > 90.0f || longitude < -180.0f || longitude > 180.0f){
            ESP_LOGW(TAG, "Advert packet has invalid location data, skipping");
            ESP_LOGI(TAG, "latitude: %f, longitude: %f", latitude, longitude);
            return false;
        }
    }

    // Check if the packet has valid name data
    if(pkt->payload.advert.flags & MESHCORE_ADVERT_FLAG_HAS_NAME){
        if(pkt->payload.advert.name_len < 3 || pkt->payload.advert.name_len > 32){
            ESP_LOGW(TAG, "Advert packet has invalid name data, skipping");
            ESP_LOGI(TAG, "name: %.*s", pkt->payload.advert.name_len, pkt->payload.advert.name);
            return false;
        }
    }

    return true;  
}

bool meshcore_packet_create_advert(meshcore_route_type_t route_type, meshcore_node_type_t node_type, meshcore_identity_t *identity, uint32_t timestamp, uint8_t *packet, uint16_t *packet_len)
{
    if (!packet || !identity) return false;
    if(route_type != MESHCORE_ROUTE_FLOOD) return false; // FIXME only flood packets are supported for now

    uint16_t i = 0;
    packet[i] = meshcore_packet_create_header(route_type, MESHCORE_PAYLOAD_ADVERT, MESHCORE_PAYLOAD_VER_1); i++;
    packet[i] = 0x00; i++; // Path length
    memcpy(&packet[i], identity->key_pair.pub_key, 32); i += 32;
    memcpy(&packet[i], &timestamp, 4); i += 4;
    uint8_t sig_offset = i;
    i+=64; // Signature
    uint8_t appdata_offset = i;
    // Flags
    uint8_t* flags = &packet[i];i++;
    *flags |= node_type;

    // Add location if present
    if(identity->has_location){
        *flags |= MESHCORE_ADVERT_FLAG_HAS_LOCATION;
        memcpy(&packet[i], &identity->latitude, 4); i += 4;
        memcpy(&packet[i], &identity->longitude, 4); i += 4;
    }

    // Add name
    if(strlen(identity->name) > 0){
        *flags |= MESHCORE_ADVERT_FLAG_HAS_NAME;
        memcpy(&packet[i], identity->name, strlen(identity->name)); i += strlen(identity->name); // name
    }

    // Sign message sadly we have to copy the message to a buffer to sign
    // i Tried tough: https://github.com/meshcore-dev/MeshCore/issues/2792
    uint8_t sign_message[32 + 4 + (i - appdata_offset)];
    memcpy(sign_message, identity->key_pair.pub_key, 32);
    memcpy(sign_message + 32, &timestamp, 4);
    memcpy(sign_message + 36, &packet[appdata_offset], i - appdata_offset);

    ed25519_sign(&packet[i], sign_message, sizeof(sign_message), (const unsigned char*)identity->key_pair.pub_key, (const unsigned char*)identity->key_pair.priv_key);
    memcpy(&packet[sig_offset], &packet[i], 64);

    *packet_len = i;

    return true;
}
