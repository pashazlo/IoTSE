# Wi-Fi RX memory ownership and lifecycle contract

This document describes enforceable ownership in the current RX pipeline. It is a developer contract, not an API overview.

## Ingress pool

`wifi_promiscuous_init()` allocates 32 fixed PSRAM slots once. The pool, free queue, and ready queue have application lifetime because the analyzer task also has application lifetime. No public deinit API exists. A published queue handle and every `packet.payload` pointer therefore remain backed by allocated storage for the process lifetime. Partial initialization frees only resources that were never published.

Each slot has exactly one logical owner:

| State | Owner | Transition and synchronization |
|---|---|---|
| `FREE` | free-index queue | RX callback removes one index with nonblocking `xQueueReceive()` |
| `RX_OWNED` | ESP Wi-Fi callback invocation | callback copies metadata and MPDU, then sends the index to the ready queue; on send failure it returns the index to the free queue |
| `READY` | ready-index queue | analyzer removes one index with `xQueueReceive()` |
| `ANALYZER_OWNED` | analyzer task | analyzer may borrow `packet.payload` until exactly one `wifi_promiscuous_release()`, which returns the index to the free queue |

The payload pointer must not be retained by parsers, trackers, snapshots, or capture storage. Capture storage copies record metadata and frame bytes before release. AP and STA trackers store value-owned fields only.

## Capture session reset barrier

The worker is the sole normal owner of producer enable/disable. Before enabling the promiscuous callback it calls synchronous `wifi_analyzer_reset()` while worker state is not `PROMISCUOUS`.

Reset sets `pause_requested` and waits for the analyzer's `quiesced` semaphore. The analyzer acknowledges only between packets, after releasing any `ANALYZER_OWNED` slot. While paused, reset clears analyzer/channel snapshots, capture state, READY ingress slots, AP state, and STA state, then signals `resume`.

Postcondition after successful reset: no frame received before the reset boundary remains READY or ANALYZER_OWNED, and no such frame can later recreate AP/STA/capture state. Reset returns `ESP_ERR_INVALID_STATE` if the radio producer is still in `PROMISCUOUS` state.

READY slots are drained back to the free queue while the producer is stopped and analyzer is quiesced. There can be no RX-owned slot under that precondition. Queue objects are never deleted after publication.

## Capture bank and writer ownership

The analyzer copies ingress data into the active capture bank under the capture-store mutex. Save seals the active bank and switches capture to the other free bank atomically under that mutex. The writer owns the sealed bank until file output completes; analyzer append never mutates a sealed/writer-owned bank. When no free bank exists, save reports BUSY and does not overwrite either bank.

## Snapshots

Analyzer, AP, and STA snapshot APIs copy state while holding their respective mutex. The caller owns the copied value after return. Snapshot structs contain no ingress or capture-bank pointers. Full AP and STA snapshots are about 20 KiB and 16 KiB respectively and must use static, heap, or PSRAM storage rather than a small FreeRTOS task stack.

## Shutdown scope

The current firmware has session stop/reset but no application shutdown/join sequence. Radio stop unregisters/disables the producer; it does not destroy ingress resources or the permanent analyzer. Adding true subsystem deinitialization requires an explicit order: stop and unregister producer, join/quiesce analyzer, drain owned slots, stop/join writer, then delete queues and free pools/banks. No caller may implement only part of that sequence.