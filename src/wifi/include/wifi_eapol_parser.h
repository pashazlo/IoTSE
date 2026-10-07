#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_frame_parser.h"

typedef enum {
    WIFI_EAPOL_PARSE_OK = 0,
    WIFI_EAPOL_PARSE_MALFORMED,
} wifi_eapol_parse_status_t;

typedef enum {
    WIFI_EAPOL_MESSAGE_UNKNOWN = 0,
    WIFI_EAPOL_MESSAGE_M1,
    WIFI_EAPOL_MESSAGE_M2,
    WIFI_EAPOL_MESSAGE_M3,
    WIFI_EAPOL_MESSAGE_M4,
} wifi_eapol_message_t;

typedef struct {
    wifi_eapol_parse_status_t status;
    uint8_t protocol_version;
    uint8_t packet_type;
    uint16_t body_length;
    bool is_key;
    bool key_fields_valid;
    bool descriptor_supported;
    uint8_t descriptor_type;
    uint16_t key_info;
    uint16_t key_length;
    uint64_t replay_counter;
    uint8_t nonce[32];
    uint16_t key_data_length;
    uint8_t descriptor_version;
    bool pairwise;
    bool install;
    bool key_ack;
    bool key_mic;
    bool secure;
    bool error;
    bool request;
    bool encrypted_key_data;
} wifi_eapol_info_t;

wifi_eapol_parse_status_t wifi_eapol_parse(const uint8_t *bytes, size_t length,
                                           wifi_eapol_info_t *out);
wifi_eapol_message_t wifi_eapol_classify(
    const wifi_eapol_info_t *info, wifi_frame_direction_t direction);
