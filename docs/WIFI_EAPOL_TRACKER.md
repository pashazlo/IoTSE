# Passive EAPOL parser and tracker v1

## Actual RX path and ownership

The ESP-IDF promiscuous callback `wifi_promiscuous_rx_cb()` copies each
FCS-stripped MPDU and RX metadata into one of 32 fixed PSRAM ingress slots and
queues only its slot index. `wifi_promiscuous_receive_wait()` transfers that
slot to the analyzer task. The analyzer then performs, in order:

1. `wifi_frame_parse()`;
2. bounded EAPOL parsing/tracking when `frame.is_eapol` is set;
3. management parsing and AP tracking for supported management frames;
4. STA tracking for every supported frame;
5. `wifi_capture_store_append()`;
6. analyzer/channel statistics;
7. `wifi_promiscuous_release()`.

`wifi_raw_packet_t.payload` is borrowed and remains valid only until the final
release. The EAPOL parser returns a value object and the tracker copies all
persistent fields. No EAPOL session or snapshot contains an ingress pointer.
Capture Store independently copies the available raw frame into its active
PSRAM bank and preserves its existing EAPOL flag and global `correlation_id`.

## Parser boundary

`wifi_frame_parser` remains authoritative for 802.11 header length, DS bits,
QoS/HT Control, supported payload layout, LLC/SNAP and infrastructure roles.
For an unprotected, unfragmented, non-A-MSDU, non-mesh DATA frame with EAPOL
EtherType, it publishes `eapol_offset` and `eapol_length`. `is_eapol` means only
that the LLC payload has EtherType `0x888E`; it does not claim an EAPOL-Key or a
valid/complete handshake.

`wifi_eapol_parse()` is allocation-free and uses explicit big-endian reads. It
validates the EAPOL body length and the complete 95-byte fixed EAPOL-Key body
before exposing Key fields. It also validates that `key_data_length` fits in
the declared EAPOL body.

## Classification and session semantics

All supported messages must be pairwise and have neither ERROR nor REQUEST.
Direction and flags must agree:

- M1: AP to STA, ACK, no MIC/INSTALL/SECURE;
- M2: STA to AP, MIC, no ACK/INSTALL/SECURE;
- M3: AP to STA, ACK+MIC+INSTALL+SECURE;
- M4: STA to AP, MIC+SECURE, no ACK/INSTALL, zero key data.

Everything else is `UNKNOWN`. This is an observation classifier; it never
claims cryptographic MIC validity.

The tracker has 64 fixed BSSID+STA slots and deterministic oldest-last-seen
eviction. The PSRAM state is exactly 13,952 bytes with the current ABI. A full
snapshot is 8,840 bytes and must not live on a small task stack. A current
exchange is `COMPLETE` only when M1/M2 share replay counter R and M3/M4 share
R+1. Missing or incoherent observations remain `PARTIAL`. Equal replay counter
plus equal nonce identifies M1/M3 retransmissions. A distinct M1 starts a new
bounded exchange for the same pair. Full packets remain exclusively in Capture
Store.

## Reset lifecycle

`wifi_eapol_tracker_reset()` runs inside the existing synchronous
`wifi_analyzer_reset()` quiescence barrier beside Capture Store, ingress, AP
and STA resets. The producer must already be stopped, and the analyzer
acknowledges quiescence only between packets, after releasing its ingress slot.
No second reset protocol is introduced.
