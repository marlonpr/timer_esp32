#!/usr/bin/env python3
"""Static contract checks for the v6.23 SQW robustness build."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rtc = (root / "components/rtc_discipline/rtc_discipline.c").read_text(encoding="utf-8")
sdk = (root / "sdkconfig.esp01").read_text(encoding="utf-8")

checks = {
    "core-1 constant": "#define SQW_ISR_CORE_ID 1" in rtc,
    "pinned ISR installer": "xTaskCreatePinnedToCore(" in rtc and "SQW_ISR_CORE_ID);" in rtc,
    "GPIO ISR service installed by pinned task": "gpio_install_isr_service(0)" in rtc,
    "GPIO config does not preclaim core 0": ".intr_type = GPIO_INTR_DISABLE" in rtc,
    "SQW edge enabled after core-1 service allocation": (
        "gpio_set_intr_type(ctx->sqw_gpio, GPIO_INTR_POSEDGE)" in rtc and
        rtc.index("gpio_install_isr_service(0)") < rtc.index("gpio_set_intr_type(ctx->sqw_gpio, GPIO_INTR_POSEDGE)")
    ),
    "installer core runtime assertion": "installer_core != SQW_ISR_CORE_ID" in rtc,
    "late-only +d/-d gate": (
        "first_interval_residual_us < threshold_us" in rtc and
        "second_interval_residual_us > -threshold_us" in rtc
    ),
    "old symmetric gate removed": "fabs(first_interval_residual_us) < threshold_us" not in rtc,
    "no unsafe IRAM claim": "ESP_INTR_FLAG_IRAM" not in rtc and "IRAM_ATTR sqw_isr" not in rtc,
    "FreeRTOS remains flash-resident": "# CONFIG_FREERTOS_IN_IRAM is not set" in sdk,
}

for name, ok in checks.items():
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
if not all(checks.values()):
    raise SystemExit(1)
print("v6.23.1 SQW source contract: PASS")
