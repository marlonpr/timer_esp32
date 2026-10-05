#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "main/main.cpp").read_text(encoding="utf-8")
H = (ROOT / "main/protocol_codec.h").read_text(encoding="utf-8")
CPP = (ROOT / "main/protocol_codec.cpp").read_text(encoding="utf-8")
RTC_H = (ROOT / "components/rtc_discipline/rtc_discipline.h").read_text(encoding="utf-8")
RTC_C = (ROOT / "components/rtc_discipline/rtc_discipline.c").read_text(encoding="utf-8")

checks=[]
def check(name, cond): checks.append((name, bool(cond)))
check("packet envelope 511", "kMaxPacketLength = 511" in H)
check("accepted edges in status struct", "uint64_t accepted_edges" in RTC_H)
check("inferred missing in status struct", "uint64_t inferred_missing_edges" in RTC_H)
check("holdover transition counter exists", "uint64_t holdover_entries" in RTC_H)
check("holdover counter increments only on entry", "old_state != RTC_DISCIPLINE_HOLDOVER" in RTC_C and "s_ctx.status.holdover_entries++" in RTC_C)
check("fleet status exposes continuity counters", "rtc.accepted_edges" in MAIN and "rtc.inferred_missing_edges" in MAIN and "rtc.holdover_entries" in MAIN)
check("31-field formatter appends continuity counters", "rtc_accepted_edges" in CPP and "rtc_inferred_missing_edges" in CPP and "rtc_holdover_entries" in CPP)
failed=[n for n,ok in checks if not ok]
for n,ok in checks: print(f"{'PASS' if ok else 'FAIL'}: {n}")
if failed: raise SystemExit(f"{len(failed)} check(s) failed")
print(f"PASS: {len(checks)} v6.23.4 qualification-guard source-contract checks")
