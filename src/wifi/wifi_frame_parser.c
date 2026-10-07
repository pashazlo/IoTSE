#include "wifi_frame_parser.h"
#include <stddef.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool wifi_frame_parse(const uint8_t *payload, uint16_t length,
                     int8_t rssi, uint8_t channel, wifi_frame_info_t *out_info)
{
    if (out_info == NULL) return false;
    memset(out_info, 0, sizeof(*out_info));
    if (payload == NULL || length < 2) return false;
    const uint16_t fc = read_le16(payload);
    if ((fc & 3U) != 0) return false;
    wifi_frame_info_t f = {0};
    f.type = (fc >> 2) & 3U;
    f.subtype = (fc >> 4) & 15U;
    f.rssi = rssi;
    f.channel = channel;
    f.to_ds = (fc & 0x0100U) != 0;
    f.from_ds = (fc & 0x0200U) != 0;
    f.protected_frame = (fc & 0x4000U) != 0;

    if (f.type == WIFI_FRAME_TYPE_CTRL) {
        if (f.to_ds || f.from_ds) return false;
        switch (f.subtype) {
            case 12: case 13: /* CTS, ACK */
                f.header_length = 10;
                break;
            case 10: case 11: case 14: case 15:
                f.header_length = 16; /* PS-Poll, RTS, CF-End variants */
                f.has_addr2 = true;
                break;
            default: return false; /* BAR/BA/modern control: not implemented */
        }
        if (length < f.header_length) return false;
        memcpy(f.addr1, payload + 4, 6);
        if (f.has_addr2) memcpy(f.addr2, payload + 10, 6);
        if (f.subtype == 10 || f.subtype == 14 || f.subtype == 15) {
            memcpy(f.bssid, f.subtype == 10 ? f.addr1 : f.addr2, 6);
            f.has_bssid = true;
        }
        *out_info = f;
        return true;
    }
    if (f.type != WIFI_FRAME_TYPE_MGMT && f.type != WIFI_FRAME_TYPE_DATA)
        return false;
    if (length < 24) return false;
    if (f.type == WIFI_FRAME_TYPE_MGMT && (f.to_ds || f.from_ds)) return false;
    size_t header_len = 24;
    memcpy(f.addr1, payload + 4, 6);
    memcpy(f.addr2, payload + 10, 6);
    memcpy(f.addr3, payload + 16, 6);
    f.has_addr2 = f.has_addr3 = true;
    f.fragmented = (fc & 0x0400U) != 0 || (read_le16(payload + 22) & 15U) != 0;
    if (f.type == WIFI_FRAME_TYPE_DATA && f.to_ds && f.from_ds) {
        if (length < 30) return false;
        memcpy(f.addr4, payload + 24, 6);
        f.has_addr4 = true;
        header_len += 6;
    } else {
        const uint8_t *bssid = f.to_ds ? f.addr1 : f.from_ds ? f.addr2 : f.addr3;
        memcpy(f.bssid, bssid, 6);
        f.has_bssid = true;
    }
    if (f.type == WIFI_FRAME_TYPE_DATA && (f.subtype & 8U) != 0) {
        if (length < header_len + 2) return false;
        f.qos = true;
        f.amsdu = (payload[header_len] & 0x80U) != 0;
        header_len += 2;
        if (fc & 0x8000U) header_len += 4; /* HT Control */
    } else if (f.type == WIFI_FRAME_TYPE_MGMT && (fc & 0x8000U)) {
        header_len += 4;
    }
    if (length < header_len) return false;
    f.header_length = (uint16_t)header_len;
    /* Mesh data needs another header and address mapping; metadata only. */
    bool mesh = f.qos && f.to_ds && f.from_ds &&
                (payload[30 + 1] & 1U) != 0;
    static const uint8_t eapol_snap[8] = {0xAA,0xAA,0x03,0,0,0,0x88,0x8E};
    if (f.type == WIFI_FRAME_TYPE_DATA && (f.subtype & 4U) == 0 &&
        !f.protected_frame && !f.fragmented && !f.amsdu && !mesh &&
        length >= header_len + sizeof(eapol_snap)) {
        f.is_eapol = memcmp(payload + header_len, eapol_snap, sizeof(eapol_snap)) == 0;
        if (f.is_eapol) {
            f.eapol_offset = (uint16_t)(header_len + sizeof(eapol_snap));
            f.eapol_length = (uint16_t)(length - f.eapol_offset);
        }
    }
    *out_info = f;
    return true;
}

static bool unicast_mac(const uint8_t mac[6])
{
    bool nonzero = false;
    if (mac == NULL || (mac[0] & 1U) != 0U) return false;
    for (unsigned i = 0; i < 6; ++i) nonzero |= mac[i] != 0U;
    return nonzero;
}

bool wifi_frame_get_infrastructure_roles(const wifi_frame_info_t *frame,
                                         uint8_t bssid[6], uint8_t sta[6],
                                         wifi_frame_direction_t *direction)
{
    if (frame == NULL || bssid == NULL || sta == NULL || direction == NULL ||
        frame->type != WIFI_FRAME_TYPE_DATA || frame->to_ds == frame->from_ds)
        return false;
    const uint8_t *b = frame->to_ds ? frame->addr1 : frame->addr2;
    const uint8_t *s = frame->to_ds ? frame->addr2 : frame->addr1;
    if (!unicast_mac(b) || !unicast_mac(s)) return false;
    memcpy(bssid, b, 6);
    memcpy(sta, s, 6);
    *direction = frame->to_ds ? WIFI_FRAME_DIRECTION_STA_TO_AP
                              : WIFI_FRAME_DIRECTION_AP_TO_STA;
    return true;
}
