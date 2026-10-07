#include "wifi_eapol_parser.h"

#include <string.h>

#define EAPOL_HEADER_LEN 4U
#define EAPOL_TYPE_KEY 3U
#define EAPOL_KEY_FIXED_BODY_LEN 95U

static uint16_t read_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint64_t read_be64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

wifi_eapol_parse_status_t wifi_eapol_parse(const uint8_t *bytes, size_t length,
                                           wifi_eapol_info_t *out)
{
    if (out == NULL) return WIFI_EAPOL_PARSE_MALFORMED;
    memset(out, 0, sizeof(*out));
    out->status = WIFI_EAPOL_PARSE_MALFORMED;
    if (bytes == NULL || length < EAPOL_HEADER_LEN) return out->status;

    out->protocol_version = bytes[0];
    out->packet_type = bytes[1];
    out->body_length = read_be16(bytes + 2);
    if ((size_t)out->body_length > length - EAPOL_HEADER_LEN)
        return out->status;

    out->is_key = out->packet_type == EAPOL_TYPE_KEY;
    if (!out->is_key) {
        out->status = WIFI_EAPOL_PARSE_OK;
        return out->status;
    }
    if (out->body_length < EAPOL_KEY_FIXED_BODY_LEN) return out->status;

    const uint8_t *key = bytes + EAPOL_HEADER_LEN;
    out->descriptor_type = key[0];
    /* Descriptor 1 is the older RC4 layout and is not decoded as WPA/RSN. */
    out->descriptor_supported = key[0] == 2U || key[0] == 254U;
    out->key_info = read_be16(key + 1);
    out->key_length = read_be16(key + 3);
    out->replay_counter = read_be64(key + 5);
    memcpy(out->nonce, key + 13, sizeof(out->nonce));
    out->key_data_length = read_be16(key + 93);
    if ((size_t)out->body_length !=
        EAPOL_KEY_FIXED_BODY_LEN + (size_t)out->key_data_length)
        return out->status;

    out->descriptor_version = (uint8_t)(out->key_info & 0x0007U);
    out->pairwise = (out->key_info & 0x0008U) != 0U;
    out->install = (out->key_info & 0x0040U) != 0U;
    out->key_ack = (out->key_info & 0x0080U) != 0U;
    out->key_mic = (out->key_info & 0x0100U) != 0U;
    out->secure = (out->key_info & 0x0200U) != 0U;
    out->error = (out->key_info & 0x0400U) != 0U;
    out->request = (out->key_info & 0x0800U) != 0U;
    out->encrypted_key_data = (out->key_info & 0x1000U) != 0U;
    out->key_fields_valid = true;
    out->status = WIFI_EAPOL_PARSE_OK;
    return out->status;
}

wifi_eapol_message_t wifi_eapol_classify(
    const wifi_eapol_info_t *k, wifi_frame_direction_t direction)
{
    if (k == NULL || k->status != WIFI_EAPOL_PARSE_OK || !k->is_key ||
        !k->key_fields_valid || !k->descriptor_supported || !k->pairwise ||
        (k->protocol_version != 1U && k->protocol_version != 2U) ||
        k->descriptor_version < 1U || k->descriptor_version > 3U ||
        k->request || k->error)
        return WIFI_EAPOL_MESSAGE_UNKNOWN;

    /* Direction and key-info flags are both required. Classification records
     * an observation only; it does not validate the MIC cryptographically. */
    if (direction == WIFI_FRAME_DIRECTION_AP_TO_STA && k->key_ack &&
        !k->key_mic && !k->install && !k->secure && !k->encrypted_key_data)
        return WIFI_EAPOL_MESSAGE_M1;
    if (direction == WIFI_FRAME_DIRECTION_STA_TO_AP && !k->key_ack &&
        k->key_mic && !k->install && !k->secure && !k->encrypted_key_data)
        return WIFI_EAPOL_MESSAGE_M2;
    if (direction == WIFI_FRAME_DIRECTION_AP_TO_STA && k->key_ack &&
        k->key_mic && k->install && k->secure)
        return WIFI_EAPOL_MESSAGE_M3;
    if (direction == WIFI_FRAME_DIRECTION_STA_TO_AP && !k->key_ack &&
        k->key_mic && !k->install && k->secure && !k->encrypted_key_data &&
        k->key_data_length == 0U)
        return WIFI_EAPOL_MESSAGE_M4;
    return WIFI_EAPOL_MESSAGE_UNKNOWN;
}
