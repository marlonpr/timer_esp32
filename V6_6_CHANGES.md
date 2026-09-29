# v6.6 DS3231 qualification/timing fix

Changes relative to the first DS3231 pilot:

1. **Fixes per-SQW re-anchor quantization bias**
   - internal disciplined anchor is now stored in nanoseconds
   - rate conversion rounds below 1 ns instead of truncating at 1 us
   - public timing API remains in microseconds
   - deterministic +4.245 ppm regression: old = -4530 us/6000 s (-0.755 ppm), new = 0 ns

2. **Presentation analyzer edge on every visible flip**
   - START commit toggles the diagnostic output
   - every later one-second commit toggles it again, including 00:00
   - classic ESP32 GPIO33, ESP32-S3 GPIO14 in supplied profiles

3. **Reduces classic ESP32 final-spin contention**
   - disciplined deadline is re-resolved during coarse waiting
   - final <=2 ms spin freezes that resolved deadline
   - removes an RTC critical-section entry from every spin iteration

4. **Adds RTC qualification logging and analyzer**
   - `RTC_QUAL`, `RTC_SYNC_QUAL`, `RTC_START_QUAL`
   - `tools/rtc_qualification.py`
   - q and r are evaluated over the same SYNC_SET-to-SYNC_SET interval
   - batch comparison is relative to the common master/batch median, not absolute zero

5. **Adds clean DS3231 OFF baselines for ESP01/ESP02**
   - `sdkconfig.esp01.no_rtc`
   - `sdkconfig.esp02.no_rtc`
   - see `RTC_AB_VALIDATION.md`

The raw master synchronization, gate, T*, ARM, and first visible START deadline are unchanged.
