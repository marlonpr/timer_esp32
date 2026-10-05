#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "main/main.cpp").read_text(encoding="utf-8")
CODEC_H = (ROOT / "main/protocol_codec.h").read_text(encoding="utf-8")
CODEC_CPP = (ROOT / "main/protocol_codec.cpp").read_text(encoding="utf-8")
DISPLAY_H = (ROOT / "components/factory_display/include/factory_display.h").read_text(encoding="utf-8")
DISPLAY_CPP = (ROOT / "components/factory_display/factory_display.cpp").read_text(encoding="utf-8")

checks = []
def check(name, value):
    checks.append((name, bool(value)))

check("packet length expanded", "kMaxPacketLength = 511" in CODEC_H)
check("extended status formatter carries disciplined epoch",
      "sync_epoch_disciplined_us" in CODEC_H and
      "sync_master_minus_disciplined_us" in CODEC_CPP)
check("sync qualification uses source offset at selected epoch",
      "sync_source_offset_us = clock_sync.source_offset_us" in MAIN and
      "sync_master_minus_disciplined_us =\n            sync_source_offset_us + sync_epoch_local_us - sync_epoch_disciplined_us;" in MAIN)
check("sync epoch disciplined mapping is captured on SYNC_SET",
      "clock_sync.offset_epoch_disciplined_us =\n                            rtc_discipline_local_to_disciplined_us(" in MAIN)
check("serial sync report exposes disciplined epoch",
      "offset_epoch_disciplined_us=%lld offset_master_minus_disciplined_us=%lld" in MAIN)
check("start health summary retained",
      "last_start_health.start_error_us" in MAIN and
      "last_start_health.scheduler_lateness_us" in MAIN)
check("display health API exists",
      "factory_display_get_health" in DISPLAY_H and
      "factory_display_get_health" in DISPLAY_CPP)
check("display summary tracks final frame-not-ready",
      "s_health_summary.frame_not_ready_count" in DISPLAY_CPP)
check("no scan-restart policy introduced",
      "restart_scan" not in DISPLAY_CPP.lower())

failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
if failed:
    raise SystemExit(f"{len(failed)} check(s) failed")
print(f"PASS: {len(checks)} v6.23.4 fleet-health source-contract checks")
