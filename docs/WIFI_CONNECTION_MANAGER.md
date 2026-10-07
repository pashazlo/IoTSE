# Wi-Fi Connection Manager: reliability audit and remediation

## Ownership and call graph

`ui_wifi_connect` and other clients submit typed commands to the bounded Wi-Fi Worker queue. The `wifi_conn_mgr` task is the only task allowed to call the `wifi_radio_*` control API. `wifi.c` is the only translation unit that calls ESP-IDF Wi-Fi control functions. ESP-IDF event callbacks copy compact scan/connection events into the bounded radio-event queue; they never run policy, NVS, UI, or retry logic.

```
UI / clients -> Wi-Fi Worker command queue -> wifi_conn_mgr -> wifi_radio_* -> ESP-IDF
                                                     ^                |
                                                     | radio events   |
                                                     +----------------+
```

Passive capture remains behind the same worker ownership gate. Connection policy does not add another promiscuous callback or radio owner.

## Audit findings fixed

- Automatic connection previously held the worker in a blocking wait, so a manual request could not preempt it promptly.
- A BSSID copied from a scan result was always applied as `bssid_set=true`. That converted discovery metadata into a persistent AP lock and broke same-SSID roaming.
- Boot auto-connect tried one stored SSID without first proving that the AP was visible.
- The old model collapsed association, DHCP, disconnect and IP loss into a boolean result and had no attempt identity. Late events could affect a newer operation.
- Credential storage contained one `last_good` record. It could not represent multiple known networks or deterministic selection.
- Disconnect reason 2 is `WIFI_REASON_AUTH_EXPIRE` in ESP-IDF 6.2. Treating every disconnect as a rejected password caused false password prompts.
- The rewritten worker initially missed auto-connect from its cold `OFF` state. The final implementation starts the radio before the automatic scan.

## Connection state machine

Policy states are `IDLE`, `ASSOCIATING`, `WAITING_IP`, `CONNECTED`, `RETRY_WAIT`, and `SUSPENDED`. Worker-only radio lifecycle states additionally cover start, scan, explicit disconnect, network switching, promiscuous mode, stop and error.

Every connection attempt receives a nonzero monotonically increasing attempt ID. Events whose attempt ID differs from the active attempt are ignored and counted. Association has a 15 s deadline; DHCP has a separate 15 s deadline. A manual request cancels an automatic scan or preempts an automatic/manual connection, crosses a disconnect barrier, and starts a new attempt. Changing auto-connect policy does not mutate an active manual attempt.

`STA_CONNECTED` means associated and moves to `WAITING_IP`. `IP_EVENT_STA_GOT_IP` alone establishes `CONNECTED`. `IP_EVENT_STA_LOST_IP` clears connected state and schedules bounded recovery. `STA_DISCONNECTED` carries the raw 802.11 reason plus a policy failure class.

## Retry policy

Automatic failures use bounded delays of 2, 5, 15, 30 and 60 seconds. After five retries the manager returns to idle and does not spin. Explicitly enabling auto-connect resets this budget. A classified authentication failure blocks automatic retries until the user changes policy or starts a manual attempt. User disconnect suppresses automatic reconnect. Manual requests remain accepted during automatic scan, association, DHCP wait and retry delay.

## Known Networks NVS schema

Namespace: `wifi_auth`. Current key: `known_v3`. The encoded blob has magic, version, entry count, four fixed-capacity records and CRC32. Maximum encoded size is 468 bytes. Each record contains SSID, password, security, auto-connect flag, last successful channel, BSSID hint and success sequence.

Loading distinguishes a missing database from malformed/CRC-invalid storage. Mutations are copy-on-write: RAM changes become visible only after the new NVS blob commits successfully. The legacy `last_good` v1/v2 blob is decoded once, converted to a v3 entry and retained through the new store; migration never logs the password.

The store is bounded to four networks. When full, insertion evicts the occupied entry with the oldest success sequence. A newly saved manual network defaults to auto-connect disabled. Reconnecting an existing network preserves its auto-connect preference.

## Selection and BSSID behavior

Auto-connect always performs a fresh scan and considers only visible entries whose per-network auto-connect flag is enabled. Selection is deterministic: latest successful network first, then RSSI/BSSID tie-breaking. Saved BSSID and channel are hints/diagnostics. Normal manual and automatic connections leave ESP-IDF BSSID locking disabled, so another AP with the same SSID can be used. The BSSID and channel observed at association are recorded after GOT_IP.

## Diagnostics

The cheap mutex-protected snapshot exposes current phase, origin, attempt ID, last raw reason, classified failure, retry count, credentials block and counters for manual attempts, automatic attempts, successful connections, retries, failures by class, transient disconnects and stale events. Logs contain attempt ID, origin, state transitions, raw reason, class, retry number and delay. Passwords are never logged.

## Verification

Host tests cover 20 required state-machine scenarios, including manual override, auto-toggle during manual connection, stale events, DHCP timeout, stable disconnect and retry exhaustion. Known-network tests cover encoding/decoding, CRC corruption, legacy-compatible BSSID mobility behavior, deterministic eviction and forget. Both suites compile with `-Wall -Wextra -Werror` and pass.

The complete ESP-IDF 6.2 build succeeds. `IoTSE.bin` is `0x10d730` bytes; the smallest app partition retains `0x2f28d0` bytes (74%).

## Hardware test plan

1. Boot with empty NVS: confirm no retry loop and a missing-database diagnostic.
2. Connect manually to AP-A: confirm ASSOCIATED then GOT_IP and normal traffic; confirm the new entry is saved with auto-connect off.
3. Enable auto-connect, reboot with AP-A visible: confirm boot radio start, fresh scan, selection and GOT_IP.
4. Disable AP-A: confirm exactly five bounded retries and no busy loop.
5. During each auto phase (scan, association, DHCP and retry wait), select AP-B manually: confirm prompt preemption and that late AP-A events increment `stale_events_ignored` without altering AP-B.
6. Use two APs with the same SSID and different BSSIDs/channels: disable the last-used AP and confirm connection to the other BSSID.
7. Force DHCP failure after successful association: confirm `WAITING_IP`, 15 s timeout and `IP_TIMEOUT`, without a false password prompt.
8. Generate an authentication failure: confirm raw reason/class diagnostics and automatic retry suppression; re-enable auto-connect and confirm the policy resets intentionally.
9. Disconnect from UI: confirm no immediate reconnect. Reboot and verify the persisted per-network setting.
10. Fill four entries, add a fifth, corrupt a copied NVS blob in a test partition, and verify deterministic eviction plus explicit corruption recovery.
