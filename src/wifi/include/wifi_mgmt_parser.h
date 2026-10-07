#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_frame_parser.h"
#include "wifi_rsn_parser.h"

#define WIFI_MGMT_SSID_MAX_LEN 32U

typedef enum {
    WIFI_SSID_ABSENT = 0, WIFI_SSID_EMPTY, WIFI_SSID_ALL_ZERO, WIFI_SSID_NORMAL,
} wifi_ssid_kind_t;

typedef struct {
    wifi_parse_status_t status;
    uint8_t subtype;
    uint8_t rx_channel;
    bool has_advertised_channel;
    uint8_t advertised_channel;
    wifi_ssid_kind_t ssid_kind;
    uint8_t ssid_raw_length;
    char ssid[WIFI_MGMT_SSID_MAX_LEN + 1U];
    bool has_capability;
    uint16_t capability;
    bool capability_ess, capability_ibss, capability_privacy;
    bool has_timestamp;
    uint64_t timestamp;
    bool has_beacon_interval;
    uint16_t beacon_interval;
    bool has_auth_fields;
    uint16_t auth_algorithm, auth_sequence, status_code;
    bool has_association_id;
    uint16_t association_id;
    bool has_reason_code;
    uint16_t reason_code;
    bool has_action_fields;
    uint8_t action_category, action_code;
    bool ie_chain_malformed;
    bool rsn_ie_seen;
    wifi_rsn_info_t rsn;
} wifi_mgmt_info_t;

#ifdef __cplusplus
static_assert(sizeof(wifi_mgmt_info_t) <= 320U,
              "Management parse result must remain stack-bounded");
#else
_Static_assert(sizeof(wifi_mgmt_info_t) <= 320U,
               "Management parse result must remain stack-bounded");
#endif

wifi_parse_status_t wifi_mgmt_parse(const uint8_t *mpdu, size_t length,
                                    const wifi_frame_info_t *frame,
                                    wifi_mgmt_info_t *result);
