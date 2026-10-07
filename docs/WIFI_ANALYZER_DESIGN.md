# Wi-Fi telemetry and capture backend

This document defines the staged backend for IEEE 802.11 telemetry, capture,
storage, later WPA key-exchange diagnostics, and the 320x170 UI. The backend is
general infrastructure for authorized laboratory modules. No test-specific
logic is allowed in the promiscuous callback or capture store.

## Dependency direction

```text
radio callback -> bounded ingress queue -> analyzer -> observations/snapshots
                                         -> PSRAM capture ring -> writer -> VFS

future lab test -> radio/resource manager
future lab test -> reads observations and correlates its session ID/timestamps
```

The analyzer, capture store, writer, UI, and future test modules do not own each
other. The UI reads a thread-safe analyzer snapshot. A future test module sets a
correlation ID and compares its monotonic timestamps with analyzer records.

## Radiotap/PCAP binary contract

- PCAP 2.4, little-endian, microsecond timestamps, link-layer type 127
  (`IEEE802_11_RADIO`).
- File snaplen is 4111 bytes: a maximum 4095-byte 802.11 frame plus the maximum
  16-byte Radiotap header produced by the codec.
- Legacy records contain Rate, Channel/Frequency, Channel Flags, and RSSI.
- HT records contain Channel/Frequency, Channel Flags, RSSI, MCS, bandwidth,
  and guard interval. Unknown PHY records omit an invented rate.
- Management and validated EAPOL frames retain all bytes available from the RX
  path, capped at 4095. Ordinary data frames retain at most 128 bytes.
- The current promiscuous ingress copies at most 256 bytes per frame. Therefore
  "full" currently means every byte made available to this backend, not every
  byte present over the air. Raising this ceiling is a separate radio-ingress
  decision because it directly increases internal RAM queue use.

## PSRAM ring and writer contract

### Exact budget and capacity

- Two PSRAM banks of exactly 512 KiB are allocated: 1,048,576 bytes total.
- One bank is active while the other can be sealed and written. Capture can
  continue during filesystem latency without mutating the writer's data.
- Each record uses a packed 38-byte metadata header plus captured bytes, rounded
  to a 4-byte boundary.
- A bank is additionally capped at 2048 records. With today's 256-byte maximum
  payload, a full-size record occupies 296 bytes, so byte capacity limits a bank
  to about 1771 such records. Short records may reach the 2048-record cap.
- The two bank descriptors, mutex, queue control blocks, and counters remain in
  internal RAM. Their allocator overhead is implementation-dependent and is not
  charged to the fixed 1 MiB PSRAM budget.

### Wrap, pressure, and counters

- The active bank is a byte-addressed circular buffer. A record may cross the
  physical end of a bank.
- Before an append that would exceed byte or record capacity, the oldest whole
  records are evicted until the new record fits.
- `overwritten_records` and `overwritten_bytes` count deliberate oldest-record
  eviction. `dropped_busy` counts a nonblocking append that could not acquire
  the ring mutex. `dropped_frozen` counts frames rejected while frozen.
  `dropped_oversize` counts invalid or unrepresentable records.
  `corruption_resets` counts defensive active-bank resets.
- `high_water_bytes` and `high_water_records` retain the maximum active-bank
  occupancy since reset.
- The upstream radio queue has its own drop counter. It is intentionally
  separate from ring pressure, so field diagnostics can identify the bottleneck.

### Ownership and lifetime

- The Wi-Fi callback copies driver-owned bytes into a queue item and returns.
  Driver memory is never retained.
- The analyzer owns a dequeued item only for one loop iteration. The capture
  store copies its metadata and payload into the active PSRAM bank.
- On save, the active bank becomes sealed and immutable. The second bank is
  reset and becomes active. The writer alone reads the sealed bank; after close,
  it resets and releases that bank for the next save.
- Only one save may be pending. A second request returns `ESP_ERR_INVALID_STATE`.

### Thread safety and failure behavior

- Ring metadata is protected by a mutex. Append never waits for it; this keeps
  analyzer latency bounded and increments `dropped_busy` on contention.
- Bank swap, freeze, reset, counter snapshots, and save completion are atomic
  with respect to ring metadata. The writer performs slow VFS I/O without the
  mutex because its bank is sealed.
- Initialization is transactional. If either exact-size PSRAM allocation, the
  mutex, queue, or writer task cannot be created, all partial allocations are
  released and analyzer startup reports an error. The rest of firmware remains
  operational with telemetry capture unavailable.
- Capture can be frozen after an interesting event. Freeze preserves the active
  bank and rejects later appends until unfreeze; unfreeze may retain or reset it.

### Correlation timestamps

- Every stored record has a monotonically increasing 64-bit sequence number.
- Every stored record has `esp_timer_get_time()` microseconds since boot and a
  32-bit correlation ID.
- Analyzer snapshots expose their latest monotonic timestamp and correlation
  ID. Future lab modules can mark a session without adding special branches to
  the callback. Wall-clock synchronization can be stored as a separate event;
  it must not replace the monotonic correlation clock.

### Current storage sink

The writer currently creates Radiotap PCAP files under `/storage/captures` on
the internal VFS. SD routing remains a later storage-policy step because the SD
card shares the SPI bus with the display and must be validated independently.

## Runtime tasks and internal RAM

- Analyzer task: 4096-byte configured stack, priority 3.
- Writer task: 4096-byte configured stack, priority 2.
- Promiscuous ingress: 32 queue items of `sizeof(wifi_raw_packet_t)` plus the
  FreeRTOS queue control block. The exact value is compiler/ABI dependent and is
  logged or measured on target through heap snapshots during validation.
- Analyzer and capture snapshots are small fixed structures in internal RAM.

## Milestone boundary

This stage implements the PSRAM ring, analyzer ownership boundary, independent
writer, counters, freeze, and timestamp/session correlation foundation. It does
not implement active test modules, EAPOL state tracking, or new UI screens.

## Channel Engine and runtime diagnostics

The former hopper now acts as an independent Channel Engine. It controls only
the radio channel through the Wi-Fi worker and never parses frames or owns the
analyzer.

- `FIXED` holds the user-selected channel until another command is applied.
- `SURVEY` walks the runtime range returned by `wifi_radio_get_channel_range()`.
- Survey dwell is configurable from 50 to 60000 ms and defaults to 250 ms.
- Dwell accounting uses monotonic microseconds and survives FIXED/SURVEY mode
  changes within one capture session. A new promiscuous session resets it.
- Frame attribution always uses `rx_ctrl.channel` copied into each queue item.
  It never infers a frame channel from the engine's current state.

The analyzer maintains one fixed observation slot for each possible 2.4 GHz
channel. The snapshot marks the regulatory first/last range, and callers ignore
slots outside that runtime range. Per-channel data includes dwell time, total
frames, frames/second, original on-air length (including reported FCS), frame
class counts, EAPOL, RSSI aggregation, and last-seen monotonic timestamp.

Runtime diagnostics are refreshed once per second and copied under the analyzer
mutex. UI reads therefore perform one small bounded structure copy and do not
query heap, queues, or writer state themselves. The same refresh produces one
aggregated `WIFI_PIPE` log line; packets are never logged individually.

### Save state contract

`wifi_capture_store_save_request()` returns an explicit state:

- `QUEUED`: the active bank was atomically sealed, the other bank became active,
  and writer ownership was queued;
- `EMPTY`: the active bank has no records;
- `BUSY`: writer still owns the other bank, so no free bank exists;
- `ERROR`: the store or writer queue is unavailable.

A BUSY request increments `save_busy_count`. A sealed bank is immutable until
the writer closes its file and resets that exact bank. Capture continues in the
other bank, and neither analyzer nor a second save request may overwrite the
writer-owned bank.
