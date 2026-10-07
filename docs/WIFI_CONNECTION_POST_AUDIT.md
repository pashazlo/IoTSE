# Wi-Fi Connection Manager — targeted post-audit remediation

## 1. Root findings

**SOURCE PROOF — real defect:** ESP-IDF events do not carry the IoTSE `attempt_id`. `wifi_radio_connect()` writes `s_connect_attempt_id`; `WIFI_EVENT_STA_CONNECTED` copies that mutable value into `s_associated_attempt_id`; the adapter then attaches it to queued application events. The old `GOT_IP` path fell back to `s_connect_attempt_id` when no association boundary existed, and the state machine accepted `GOT_IP` from `ASSOCIATING`. Thus `GOT_IP before STA_CONNECTED` could establish `CONNECTED`.

**SOURCE PROOF — provenance limit:** manual preemption waits for `WIFI_EVENT_STA_DISCONNECTED`; association/DHCP timeout uses radio stop/start; the event callback and worker queues preserve their local enqueue order. These barriers make ordinary stale events harmless. They do not turn the application ID into native ESP-IDF provenance, and a pathological delayed event around a same-SSID reconnect cannot be proven impossible from source alone. Final classification: **C** for the premature `GOT_IP` acceptance fixed here; **D** for the remaining ESP-IDF timing boundary pending hardware stress.

**SOURCE PROOF — security defect:** scan records and Known Networks v3 both contain security mode, but `wifi_known_db_select_visible()` previously compared only SSID. A saved WPA2 network could therefore select an OPEN AP with the same SSID.

## 2. Event acceptance invariants

- `WIFI_RADIO_CONN_ASSOCIATED`: accepted only in `ASSOCIATING`.
- `WIFI_RADIO_CONN_GOT_IP`: accepted only in `WAITING_IP`, after an accepted association event. The adapter no longer falls back to `s_connect_attempt_id` for `GOT_IP`.
- `WIFI_RADIO_CONN_DISCONNECTED`: accepted only in `ASSOCIATING`, `WAITING_IP`, `CONNECTED`, `SWITCHING_NETWORK`, or `DISCONNECTING`.
- `WIFI_RADIO_CONN_LOST_IP`: accepted only in `CONNECTED` and only with an association-derived ID.
- `IDLE` and `RETRY_WAIT` cannot transition to `CONNECTED` from a late IP event and cannot consume a second disconnect into another retry.
- A mismatched application ID increments `stale_events_ignored`; a valid ID in an impossible state increments `lifecycle_events_rejected`.
- Timeout accepts only `ASSOCIATING` or `WAITING_IP`. Manual preemption replaces the active state-machine attempt; old application events are rejected.

**HOST TEST:** the state machine rejects late `GOT_IP`, early `GOT_IP`, late association after timeout, events from a preempted attempt and repeated failure after entry to `RETRY_WAIT`.

## 3. Security compatibility policy

SSID remains the logical identity. BSSID and channel remain mobile and are never mandatory locks for automatic connection.

Candidate security is checked before RSSI/BSSID ordering:

- exact mode is compatible;
- saved `WPA` may move to `WPA/WPA2`;
- saved `WPA/WPA2` may move to `WPA2`;
- saved `WPA2` may move to `WPA2/WPA3`;
- saved `WPA2/WPA3` may move to `WPA3`;
- reverse/downgrade transitions are rejected;
- `OPEN`, `WEP`, `ENTERPRISE`, `WAPI`, `OWE`, and `DPP` require exact mode;
- ordinary `UNKNOWN` never auto-matches.

Legacy `last_good` had no security field. New migrations store `UNKNOWN`. Existing migrated v3 records that contain `OPEN` plus a non-empty password are recognized as legacy metadata and may match only PSK-family modes; they can never select OPEN. A successful connection replaces the legacy value with the observed scan mode. The v3 wire schema and numeric security values are unchanged.

**HOST TEST:** saved WPA2 plus visible OPEN is rejected; compatible BSSID/channel movement succeeds; deterministic selection uses RSSI then MAC; an incompatible saved BSSID cannot win; supported transition upgrades pass and downgrade transitions fail.

## 4. Tests and build

**HOST TEST — PASS:**

- `wifi_connection_manager_unit`: 20 transition scenarios;
- `wifi_known_networks_unit`: codec, corruption, mobility, eviction, forget;
- `wifi_connection_post_audit_unit`: 20 targeted lifecycle/security cases;
- `wifi_parser_unit`;
- `wifi_ap_tracker_unit`;
- `wifi_sta_tracker_unit`;
- `wifi_sta_raw_integration`;
- `wifi_sta_stress`: 50,000 deterministic observations.

All targeted tests were compiled with `-Wall -Wextra -Werror`.

**BUILD PROOF — PASS:** complete ESP-IDF 6.2 ESP32-S3 build and final incremental rebuild succeeded. `git diff --check` reports no whitespace errors; only repository-wide CRLF conversion warnings.

- `IoTSE.bin`: 1,104,128 bytes (`0x10d900`)
- SHA-256: `3B6BD5DC1AC43F17196D0A0C25F4AAC8AA1B688A8A653348A337E0E31CF51275`
- smallest app partition: `0x400000`
- free: `0x2f2700` bytes (74%)

## 5. Remaining risk

**NOT YET PROVEN:** ESP-IDF does not provide an IoTSE attempt token. The acceptance rules, disconnect barrier, radio stop/start boundary and association-derived `GOT_IP` token prevent the demonstrated invalid transitions. A rare delayed native event after a same-SSID lifecycle switch still requires hardware stress to establish ESP-IDF ordering in this exact build.

**NOT YET PROVEN:** transition-mode behavior depends on the AP and supplicant negotiating the stronger/equivalent mode as represented by the scan auth mode. Hardware tests must include controlled WPA2/WPA3 transition APs.

## 6. Hardware checklist

Watch `attempt`, state transition, raw disconnect reason, `stale_events_ignored`, `lifecycle_events_rejected`, retry number/delay, SSID, associated BSSID and channel. Never infer “wrong password” from a generic disconnect reason.

1. Repeat manual connect/disconnect for at least 100 cycles.
2. Start manual connect during auto scan, association, DHCP wait and retry delay.
3. Toggle Auto Connect during a manual attempt; verify the attempt continues unchanged.
4. Remove the AP during `ASSOCIATING`, then during `WAITING_IP`.
5. Restore the AP during `RETRY_WAIT`; verify one clean next attempt.
6. Reboot with saved Auto Connect ON, then OFF.
7. Move the same SSID to another BSSID and channel.
8. Advertise the same SSID as OPEN beside the saved WPA2 AP; verify OPEN is never selected.
9. Exercise WPA2/WPA3 transition and WPA3-only configurations in a controlled lab.
10. Repeat Air Monitor start/stop followed by reconnect, then run repeated transition cycles while watching both rejection counters.

**HARDWARE PROOF:** pending; firmware was deliberately not flashed by this milestone.
