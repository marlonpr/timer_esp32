# v6.23.4 — RTC qualification continuity telemetry

Firmware v6.23.4 is a telemetry-only follow-on to v6.23.3. Countdown scheduling, forward synchronization, SQW core-1 capture, network-silent RUNNING behavior, and frame-boundary display adoption are unchanged.

The extended FCT2 STATUS now appends three cumulative counters:

- `rtc_accepted_edges` — physically accepted DS3231 SQW edges since boot.
- `rtc_inferred_missing_edges` — inferred skipped SQW edges since boot.
- `rtc_holdover_entries` — transitions into RTC discipline HOLDOVER since boot.

These counters let the two-START qualification tool reject an interval that crosses a reboot, loses SQW continuity, or enters holdover. The disciplined qualification observable remains:

`MasterMinusDisciplined = source_offset + epoch_local - epoch_disciplined`

where all terms refer to the same selected synchronization sample.
