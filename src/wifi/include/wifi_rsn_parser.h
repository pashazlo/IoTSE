#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_ie_iterator.h"

#define WIFI_RSN_MAX_PAIRWISE_CIPHERS 8U
#define WIFI_RSN_MAX_AKMS             8U

typedef enum {
    WIFI_CIPHER_UNKNOWN = 0, WIFI_CIPHER_NONE_OR_USE_GROUP, WIFI_CIPHER_WEP40,
    WIFI_CIPHER_TKIP, WIFI_CIPHER_CCMP_128, WIFI_CIPHER_WEP104,
    WIFI_CIPHER_BIP_CMAC_128, WIFI_CIPHER_GCMP_128, WIFI_CIPHER_GCMP_256,
    WIFI_CIPHER_CCMP_256, WIFI_CIPHER_BIP_GMAC_128, WIFI_CIPHER_BIP_GMAC_256,
    WIFI_CIPHER_BIP_CMAC_256,
} wifi_cipher_kind_t;

typedef enum {
    WIFI_AKM_UNKNOWN = 0, WIFI_AKM_8021X, WIFI_AKM_PSK, WIFI_AKM_FT_8021X,
    WIFI_AKM_FT_PSK, WIFI_AKM_8021X_SHA256, WIFI_AKM_PSK_SHA256,
    WIFI_AKM_SAE, WIFI_AKM_FT_SAE, WIFI_AKM_OWE,
} wifi_akm_kind_t;

typedef enum {
    WIFI_PMF_UNAVAILABLE = 0, WIFI_PMF_DISABLED,
    WIFI_PMF_CAPABLE, WIFI_PMF_REQUIRED,
} wifi_pmf_mode_t;

typedef struct {
    wifi_cipher_kind_t kind;
    uint8_t raw[4];
} wifi_cipher_suite_t;

typedef struct {
    wifi_akm_kind_t kind;
    uint8_t raw[4];
} wifi_akm_suite_t;

typedef struct {
    wifi_parse_status_t status;
    bool truncated;
    uint16_t version;
    wifi_cipher_suite_t group_cipher;
    uint16_t pairwise_advertised_count;
    uint8_t pairwise_count;
    wifi_cipher_suite_t pairwise[WIFI_RSN_MAX_PAIRWISE_CIPHERS];
    uint16_t akm_advertised_count;
    uint8_t akm_count;
    wifi_akm_suite_t akms[WIFI_RSN_MAX_AKMS];
    bool has_capabilities;
    uint16_t capabilities;
    wifi_pmf_mode_t pmf;
    bool has_pmkid_count;
    uint16_t pmkid_count;
    bool has_group_management_cipher;
    wifi_cipher_suite_t group_management_cipher;
} wifi_rsn_info_t;

wifi_parse_status_t wifi_rsn_parse(const uint8_t *data, size_t length,
                                   wifi_rsn_info_t *result);
