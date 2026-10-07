#include "wifi_ieee80211_codec.h"

#include <string.h>

_Static_assert(sizeof(wifi_ieee80211_mgmt_hdr_t) ==
                   WIFI_IEEE80211_MGMT_HEADER_LEN,
               "Unexpected IEEE 802.11 management header size");
_Static_assert(sizeof(wifi_ieee80211_reason_frame_t) ==
                   WIFI_IEEE80211_REASON_FRAME_LEN,
               "Unexpected IEEE 802.11 reason frame size");

static void write_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFU);
    destination[1] = (uint8_t)(value >> 8U);
}

static uint16_t load_packed_u16(const void *base, size_t offset)
{
    uint16_t value;
    memcpy(&value, (const uint8_t *)base + offset, sizeof(value));
    return value;
}

size_t serialize_reason_frame(
    const wifi_ieee80211_reason_frame_t *frame,
    uint8_t *out_buf,
    size_t max_len
)
{
    if (frame == NULL || out_buf == NULL ||
        max_len < WIFI_IEEE80211_REASON_FRAME_LEN) {
        return 0U;
    }

    write_le16(out_buf + 0U,
               load_packed_u16(frame, offsetof(
                   wifi_ieee80211_reason_frame_t,
                   header.frame_control)));
    write_le16(out_buf + 2U,
               load_packed_u16(frame, offsetof(
                   wifi_ieee80211_reason_frame_t,
                   header.duration_id)));
    memcpy(out_buf + 4U, frame->header.addr1, WIFI_IEEE80211_MAC_ADDR_LEN);
    memcpy(out_buf + 10U, frame->header.addr2, WIFI_IEEE80211_MAC_ADDR_LEN);
    memcpy(out_buf + 16U, frame->header.addr3, WIFI_IEEE80211_MAC_ADDR_LEN);
    write_le16(out_buf + 22U,
               load_packed_u16(frame, offsetof(
                   wifi_ieee80211_reason_frame_t,
                   header.sequence_control)));
    write_le16(out_buf + 24U, load_packed_u16(
        frame,
        offsetof(wifi_ieee80211_reason_frame_t, reason_code)
    ));

    return WIFI_IEEE80211_REASON_FRAME_LEN;
}

bool wifi_ieee80211_reason_frame_length_valid(size_t frame_len)
{
    return frame_len == WIFI_IEEE80211_REASON_FRAME_LEN;
}

bool wifi_ieee80211_mgmt_subtype_valid(
    uint16_t frame_control,
    uint16_t allowed_subtype_mask
)
{
    if ((frame_control & WIFI_IEEE80211_FC_VERSION_MASK) != 0U ||
        (frame_control & WIFI_IEEE80211_FC_TYPE_MASK) !=
            WIFI_IEEE80211_FC_TYPE_MGMT) {
        return false;
    }

    const uint16_t subtype =
        (frame_control & WIFI_IEEE80211_FC_SUBTYPE_MASK) >>
        WIFI_IEEE80211_FC_SUBTYPE_SHIFT;
    return (allowed_subtype_mask & WIFI_IEEE80211_SUBTYPE_BIT(subtype)) != 0U;
}

bool wifi_ieee80211_reason_frame_valid(
    const wifi_ieee80211_reason_frame_t *frame,
    size_t frame_len
)
{
    if (frame == NULL ||
        !wifi_ieee80211_reason_frame_length_valid(frame_len)) {
        return false;
    }

    const uint16_t frame_control =
        load_packed_u16(frame, offsetof(
            wifi_ieee80211_reason_frame_t,
            header.frame_control));
    return wifi_ieee80211_mgmt_subtype_valid(
        frame_control,
        WIFI_IEEE80211_REASON_SUBTYPES
    );
}
