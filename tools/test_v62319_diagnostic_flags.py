#!/usr/bin/env python3
"""Compile the actual build-flags header against fleet and diagnostic profiles."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "main/diagnostic_flags.h"
OPTIONS = set(re.findall(r"CONFIG_[A-Z0-9_]+", HEADER.read_text()))


def profile_defines(path):
    result = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
        if match and match[1] in OPTIONS:
            result[match[1]] = "1" if match[2] == "y" else "0" if match[2] == "n" else match[2]
    return result


cases = [(f"ESP{n:02d} production", profile_defines(ROOT / f"sdkconfig.esp{n:02d}"), 0)
         for n in range(1, 16)]
for suffix, expected in (("telemetry_canary", 1), ("sampler_task_canary", 1),
                         ("isr_canary", 2), ("start_delay_500us", 4), ("monitor_off", 16)):
    cases.append((suffix, profile_defines(ROOT / f"sdkconfig.esp01.{suffix}"), expected))
production = {"CONFIG_IDF_TARGET_ESP32": "1", "CONFIG_FACTORY_CPU0_LATENCY_MONITOR": "1"}
for option in ("CONFIG_FACTORY_TIMING_PROBE_OUTPUT", "CONFIG_FACTORY_TIMING_MECHANISM_TEST",
               "CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS", "CONFIG_FACTORY_POST_RUN_TIMING_DUMP",
               "CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US"):
    cases.append((option, {**production, option: "1"}, 8))
cases += [
    ("absent monitor option", {"CONFIG_IDF_TARGET_ESP32": "1"}, 16),
    ("explicit zero options", {**production, **{k: "0" for k in OPTIONS - set(production)}}, 0),
    ("ordinary COMMIT/refresh markers", {**production, "CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS": "1",
        "CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS": "1"}, 0),
    ("combined task/ISR/start/probe/off bits", {**production,
        "CONFIG_FACTORY_CPU0_LATENCY_MONITOR": "0", "CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY": "1",
        "CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY": "1", "CONFIG_FACTORY_START_TASK_TEST_DELAY_US": "500",
        "CONFIG_FACTORY_TIMING_PROBE_OUTPUT": "1"}, 31),
    ("S3 has no classic monitor", {"CONFIG_IDF_TARGET_ESP32": "0", "CONFIG_IDF_TARGET_ESP32S3": "1"}, 0),
]
with tempfile.TemporaryDirectory() as td:
    folder = Path(td)
    test = folder / "flags.cpp"
    binary = folder / "flags"
    for label, defines, expected in cases:
        (folder / "sdkconfig.h").write_text("\n".join(f"#define {k} {v}" for k, v in defines.items()) + "\n")
        test.write_text('#include "diagnostic_flags.h"\n'
            f'static_assert(factory_timer::BuildDiagnosticFlags() == {expected}, "wrong DiagnosticFlags");\n'
            f'int main() {{ return factory_timer::BuildDiagnosticFlags() == {expected} ? 0 : 1; }}\n')
        build = subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(folder),
            "-I", str(ROOT / "main"), str(test), "-o", str(binary)], capture_output=True, text=True)
        assert build.returncode == 0, (label, build.stdout + build.stderr)
        assert subprocess.run([str(binary)]).returncode == 0, label
        print(f"PASS: {label}: DiagnosticFlags=0x{expected:02X}")
print(f"DIAGNOSTIC_BUILD_PROFILES=PASS CASES={len(cases)}")
