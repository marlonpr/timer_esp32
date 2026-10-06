from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
k=(ROOT/'main/Kconfig.projbuild').read_text()
cfg=(ROOT/'sdkconfig.esp01.sampler_task_canary').read_text()
checks={
 'midsecond profile enabled': 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND=y' in cfg,
 'midsecond profile omits boundary macro': 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY=' not in cfg,
 'midsecond profile omits lead macro': 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US=' not in cfg,
 'hold remains 1000us': 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=1000' in cfg,
 'source guards boundary macro to non-midsecond branch': '#else\nconstexpr uint32_t kTaskCanaryBoundary = CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY;' in s,
 'source guards lead macro to non-midsecond branch': 'constexpr int64_t kTaskCanaryLeadUs = CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US;\n#endif' in s,
 'midsecond substitutes boundary zero': 'constexpr uint32_t kTaskCanaryBoundary = 0;' in s,
 'midsecond substitutes lead zero': 'constexpr int64_t kTaskCanaryLeadUs = 0;' in s,
 'midsecond after-run config used': 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_AFTER_RUN_US' in s,
}
failed=[]
for name,ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {name}")
    if not ok: failed.append(name)
print(f"RESULT={len(checks)-len(failed)}/{len(checks)}")
raise SystemExit(1 if failed else 0)
