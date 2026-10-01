#!/usr/bin/env python3
"""Static contract checks for v6.23.2 HUB75 frame-boundary adoption."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
backend = (root / "components/factory_display/esp32/factory_display_backend_esp32.c").read_text(encoding="utf-8")
display = (root / "components/factory_display/factory_display.cpp").read_text(encoding="utf-8")

isr_start = backend.index("void IRAM_ATTR factory_display_backend_publish_prepared_from_isr")
isr_text = backend[isr_start:]
refresh_start = backend.index("static void refresh_task")
refresh_end = backend.index("bool factory_display_backend_init", refresh_start)
refresh_text = backend[refresh_start:refresh_end]
clear_start = backend.index("void factory_display_backend_clear")
clear_end = backend.index("void factory_display_backend_fill_rect", clear_start)
clear_text = backend[clear_start:clear_end]

checks = {
    "pending state exists": "pending_frame_index" in backend and "pending_frame_valid" in backend,
    "ISR publishes pending": "__atomic_store_n(&pending_frame_valid, 1U, __ATOMIC_RELEASE)" in isr_text,
    "ISR does not switch scan-active": "__atomic_store_n(&active_frame_index" not in isr_text,
    "refresh adopts pending": "__atomic_store_n(&active_frame_index, next, __ATOMIC_RELEASE)" in refresh_text,
    "adoption happens before pending release": (
        refresh_text.index("__atomic_store_n(&active_frame_index, next, __ATOMIC_RELEASE)")
        < refresh_text.index("__atomic_store_n(&pending_frame_valid, 0U, __ATOMIC_RELEASE)")
    ),
    "renderer waits for adoption": "wait_for_pending_frame_adoption();" in clear_text,
    "refresh marker stays at adoption": "direct_gpio_write_level(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO" in refresh_text,
    "diagnostic advertises boundary commit": "active_frame_publish=pending_refresh_boundary" in display,
    "backend boot log advertises boundary commit": "frame_commit=refresh_boundary" in backend,
}

# Outside initialization, the refresh loop must be the only place that changes
# active_frame_index. This catches a regression back to the v6.23.1 race.
active_store_count = backend.count("__atomic_store_n(&active_frame_index")
checks["single runtime active-index writer"] = active_store_count == 1

for name, ok in checks.items():
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
if not all(checks.values()):
    raise SystemExit(1)
print("v6.23.2 frame-boundary source contract: PASS")
