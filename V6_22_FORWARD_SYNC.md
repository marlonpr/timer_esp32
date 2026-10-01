# v6.22 forward-only START synchronization

This build removes the dominant millisecond-scale START phase error observed with the symmetric four-timestamp estimator when the reverse path is delayed.

## Wire protocol

`SYNC_REPLY` now includes the earliest matched device ingress timestamp. The classic ESP32 form is:

`FCT2|SYNC_REPLY|DEVICE|SYNC_ID|T1|T2|T3|REVERSE_HOLD_US|INGRESS_LOCAL_US`

`SYNC_SET` may include the local epoch of the supplied offset:

`FCT2|SYNC_SET|SYNC_ID|MASTER_MINUS_LOCAL_US|BEST_RTT_US|OFFSET_EPOCH_LOCAL_US`

Legacy `SYNC_SET` without the epoch remains parseable for diagnostic/benchmark workflows.

## START_AT propagation

For the production forward-only estimator the offset is defined at `OFFSET_EPOCH_LOCAL_US`. Firmware stores the disciplined-time value of that epoch. A future `T*` is propagated from that anchor in the DS3231-disciplined domain and only then converted to a raw `esp_timer` deadline.

This removes the deterministic raw-esp_timer sync-to-START propagation error while keeping the high-priority raw deadline task unchanged.

## Scope

No SQW ISR-core or residual-filter changes are included in v6.22. Those remain a separate validation step after forward-sync START phase is measured.
