#!/usr/bin/env python3
"""Verify startup ordering, diagnostic profiles, and sensitivity to the old epoch bug."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DISPLAY = ROOT/'components/factory_display/factory_display.cpp'
source = DISPLAY.read_text()
main = (ROOT/'main/main.cpp').read_text()
command_startup = main.split('void CommandTask(void *) {',1)[1].split('while (true)',1)[0]
assert command_startup.index('xEventGroupWaitBits') < command_startup.index('factory_display_cache_runtime_inventory();')
assert command_startup.index('factory_display_cache_runtime_inventory();') < command_startup.index('CreateCommandSocket();')
display_task = source.split('void DisplayTask(void*) {',1)[1].split('}  // namespace',1)[0]
assert 'RefreshRuntimeTaskInventory();' not in display_task
monitor = (ROOT/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
run_prep = monitor.split('void cpu0_latency_monitor_prepare_run(',1)[1].split('void cpu0_latency_monitor_begin_run(',1)[0]
assert 'RefreshTaskInventory();' not in run_prep
assert 'now = factory_timer::InjectStartTaskTestDelay();' in main
assert 'set(PROJECT_VER "6.23.18")' in (ROOT/'CMakeLists.txt').read_text()
for n in range(1,16):
    normal = (ROOT/f'sdkconfig.esp{n:02d}').read_text()
    assert re.search(r'^CONFIG_FACTORY_START_TASK_TEST_DELAY_US=0$',normal,re.M)
for n in range(1,6):
    profile = (ROOT/f'sdkconfig.esp{n:02d}.start_delay_500us').read_text()
    assert f'CONFIG_FACTORY_DEVICE_ID="ESP{n:02d}"' in profile
    assert re.search(r'^CONFIG_FACTORY_START_TASK_TEST_DELAY_US=500$',profile,re.M)
    assert '# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY is not set' in profile
    assert '# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set' in profile

# Reintroduce the original error only after boundary 0: replace the armed
# epoch with the actual late START time. The same actual display-loop test
# must then detect an exact 1,000,500 us boundary-0 -> boundary-1 interval.
head,body = source.split('void DisplayTask(void*) {',1)
body = body.replace('const int64_t start_disciplined_us = command.start_disciplined_us;',
                    'int64_t start_disciplined_us = command.start_disciplined_us;',1)
marker = '        s_prepared_sequence.store(0, std::memory_order_release);\n\n\n        ESP_LOGI(kTag,'
assert marker in body
body = body.replace(marker,'        s_prepared_sequence.store(0, std::memory_order_release);\n'
                    '        start_disciplined_us = rtc_discipline_local_to_disciplined_us(esp_timer_get_time());\n\n'
                    '        ESP_LOGI(kTag,',1)
with tempfile.TemporaryDirectory(prefix='factory_epoch_mutation_') as td:
    folder=Path(td)
    mutant=folder/'factory_display_mutant.cpp'
    mutant.write_text(head+'void DisplayTask(void*) {'+body)
    exe=folder/'epoch_mutant'
    command=['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function',
        '-DCONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS=1','-DCONFIG_FACTORY_DISPLAY_EDGE_GPIO=33',
        '-DCONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=1','-DCONFIG_FACTORY_START_TASK_TEST_DELAY_US=500',
        f'-DFACTORY_DISPLAY_SOURCE_PATH="{mutant}"',
        '-I',str(ROOT/'tests/lifecycle-host/stubs'),'-I',str(ROOT/'components/rtc_discipline'),
        '-I',str(ROOT/'components/factory_display'),'-I',str(ROOT/'components/factory_display/include'),
        '-I',str(ROOT/'main'),str(ROOT/'tests/lifecycle-host/display_late_start_test.cpp'),
        str(ROOT/'main/countdown_timer.cpp'),'-o',str(exe)]
    build=subprocess.run(command,text=True,capture_output=True)
    assert build.returncode==0,build.stdout+build.stderr
    result=subprocess.run([str(exe)],text=True,capture_output=True)
    assert result.returncode==2 and 'boundary=0->1 delta_us=1000500' in result.stderr,result.stdout+result.stderr
print('PASS: inventories before UDP acceptance; 15 normal profiles delay=0, five diagnostic profiles delay=500; '
      'reintroduced post-START epoch fails with boundary0->1=1000500 us')
