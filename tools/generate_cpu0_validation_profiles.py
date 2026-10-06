#!/usr/bin/env python3
"""Generate ESP01-only CPU0 monitor validation profiles from the fleet baseline.

No profile is for production.  The normal ESP01..ESP15 fleet configs keep both
canaries disabled.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'sdkconfig.esp01'

TASK_OFF = '# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY is not set'
ISR_OFF = '# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set'
MON_ON = 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y'


def insert_after(text: str, marker: str, lines: list[str]) -> str:
    if marker not in text:
        raise ValueError(f'missing marker: {marker}')
    return text.replace(marker, marker + '\n' + '\n'.join(lines), 1)


def boundary_canary(base: str) -> str:
    t = base.replace(TASK_OFF, 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY=y', 1)
    t = insert_after(t, 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY=y', [
        '# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND is not set',
        'CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY=10',
        'CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US=150',
        'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=600',
    ])
    return t


def sampler_task_canary(base: str) -> str:
    t = base.replace(TASK_OFF, 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY=y', 1)
    t = insert_after(t, 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY=y', [
        'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND=y',
        'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_AFTER_RUN_US=10500000',
        'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=1000',
    ])
    return t


def isr_canary(base: str) -> str:
    t = base.replace(ISR_OFF, 'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY=y', 1)
    t = insert_after(t, 'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY=y', [
        'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_AFTER_RUN_US=16500000',
        'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US=1000',
    ])
    return t


def monitor_off(base: str) -> str:
    lines = []
    for line in base.splitlines():
        if line == MON_ON:
            lines.append('# CONFIG_FACTORY_CPU0_LATENCY_MONITOR is not set')
            continue
        if line.startswith('CONFIG_FACTORY_CPU0_LATENCY_') or line.startswith('# CONFIG_FACTORY_CPU0_LATENCY_'):
            # All dependent CPU0-monitor options disappear in an IDF-generated
            # monitor-off sdkconfig. Keep only the top-level disable line.
            continue
        lines.append(line)
    return '\n'.join(lines) + '\n'


def main() -> int:
    base = BASE.read_text()
    outputs = {
        'sdkconfig.esp01.telemetry_canary': boundary_canary(base),
        'sdkconfig.esp01.sampler_task_canary': sampler_task_canary(base),
        'sdkconfig.esp01.isr_canary': isr_canary(base),
        'sdkconfig.esp01.monitor_off': monitor_off(base),
    }
    for name, content in outputs.items():
        (ROOT / name).write_text(content)
        print(f'{name}: GENERATED')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
