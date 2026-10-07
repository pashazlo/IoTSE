#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_IEEE80211_MAC_ADDR_LEN        6U
#define WIFI_IEEE80211_MGMT_HEADER_LEN    24U
#define WIFI_IEEE80211_REASON_FRAME_LEN   26U

#define WIFI_IEEE80211_FC_VERSION_MASK    0x0003U
#define WIFI_IEEE80211_FC_TYPE_MASK       0x000CU
#define WIFI_IEEE80211_FC_SUBTYPE_MASK    0x00F0U
#define WIFI_IEEE80211_FC_SUBTYPE_SHIFT   4U
#define WIFI_IEEE80211_FC_TYPE_MGMT       0x0000U

/* A subtype mask uses bit N for management subtype N (0..15). */
#define WIFI_IEEE80211_SUBTYPE_BIT(n)     ((uint16_t)(1U << (n)))
#define WIFI_IEEE80211_SUBTYPE_DISASSOC   10U
#define WIFI_IEEE80211_SUBTYPE_DEAUTH     12U
#define WIFI_IEEE80211_REASON_SUBTYPES    \
    (WIFI_IEEE80211_SUBTYPE_BIT(WIFI_IEEE80211_SUBTYPE_DISASSOC) | \
     WIFI_IEEE80211_SUBTYPE_BIT(WIFI_IEEE80211_SUBTYPE_DEAUTH))

#if defined(__GNUC__)
#define WIFI_IEEE80211_PACKED __attribute__((packed))
#else
#define WIFI_IEEE80211_PACKED
#endif

typedef struct WIFI_IEEE80211_PACKED {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t addr1[WIFI_IEEE80211_MAC_ADDR_LEN];
    uint8_t addr2[WIFI_IEEE80211_MAC_ADDR_LEN];
    uint8_t addr3[WIFI_IEEE80211_MAC_ADDR_LEN];
    uint16_t sequence_control;
} wifi_ieee80211_mgmt_hdr_t;

typedef struct WIFI_IEEE80211_PACKED {
    wifi_ieee80211_mgmt_hdr_t header;
    uint16_t reason_code;
} wifi_ieee80211_reason_frame_t;

/** Serialize one offline frame without alignment-dependent struct casts. */
size_t serialize_reason_frame(
    const wifi_ieee80211_reason_frame_t *frame,
    uint8_t *out_buf,
    size_t max_len
);

/** A serialized reason frame has one exact fixed length (FCS excluded). */
bool wifi_ieee80211_reason_frame_length_valid(size_t frame_len);

/** Validate protocol version, management type and an allowed subtype mask. */
bool wifi_ieee80211_mgmt_subtype_valid(
    uint16_t frame_control,
    uint16_t allowed_subtype_mask
);

/** Validate the fixed length and the reason-bearing management subtypes. */
bool wifi_ieee80211_reason_frame_valid(
    const wifi_ieee80211_reason_frame_t *frame,
    size_t frame_len
);

#ifdef __cplusplus
}
#endif
