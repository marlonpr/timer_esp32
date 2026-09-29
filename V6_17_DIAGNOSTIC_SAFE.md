# v6.17 — diagnostic-safe SQW trace + ingress identity

This build is intentionally a small delta from v6.16b.

## Unchanged timing architecture

- PC master sync / gate / T* and the proven START semantics are unchanged.
- Visible START is still ISR-published as boundary 0.
- Boundaries 1..N still use the same single-index ISR frame publication.
- GPIO33 remains the publish marker.
- GPIO16 remains the refresh-adoption marker.
- The production clock estimator still uses the existing recvfrom T2 and late-patched T3. The early IP-input timestamp remains diagnostic only.

## Removed from the SQW experiment

v6.16 wrote a packed HUB75 refresh phase from the core-1 refresh loop at every row/bitplane stage and sampled refresh/network metadata from the SQW ISR. Those writes/readbacks are removed.

The SQW ISR now records only:

- its already-captured `esp_timer_get_time()` timestamp;
- ISR core id.

There is no GPIO17 marker, refresh-phase read, or network-phase read in the SQW ISR.

`SQW_TRACE` is therefore reduced to:

```
SQW_TRACE sequence=<n> local_us=<t> core=<core>
```

Application network timing can still be correlated offline from the existing `STATUS_PATH`, `STATUS_TX`, and `SYNC_REPLY_TRACE` records without adding activity to the SQW ISR.

## Early-ingress matching hardened

v6.16 matched IP-input records to recvfrom using source address, source port, destination port and UDP payload length. v6.17 additionally stores a 64-bit FNV-1a fingerprint of the exact UDP payload at IP input and recomputes the same fingerprint after recvfrom.

This prevents two same-sized packets from the same controller from being confused merely because their lengths match. The early timestamp is still diagnostic only and is not used by the production synchronization estimator.

## SQW analysis change

`tools/analyze_sqw_latency.py` no longer tries to infer the integer-second SQW alignment from the 1-Hz waveform itself.

It first aligns firmware and analyzer clocks using the GPIO33 ISR-publish sequence, including GPIO level parity, then maps physical rising SQW edges into the firmware local clock. It reports:

- physical SQW period mean/RMS/P2P;
- GPIO33 clock-anchor fit RMS;
- SQW timestamp-latency histogram;
- excursions relative to the median;
- overlap with existing application network intervals when available.

This avoids the whole-second ambiguity seen in the v6.16 analysis.
