#!/usr/bin/env python3
"""Execute the actual Wi-Fi manager task/adapter against a host radio and real timer."""
from pathlib import Path
import os
import subprocess
import tempfile
from test_v6241_startup_hardening import function

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'posix':
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

main = (ROOT/'main/main.cpp').read_text()
event = main.split('void WifiEventHandler(', 1)[1].split('void InitialiseNvs()', 1)[0]
assert 'esp_wifi_connect(' not in event and 'vTaskDelay(' not in event
for name in ('WifiStationStarted', 'WifiStationConnected', 'WifiStationDisconnected', 'WifiScanCompleted', 'WifiFactoryLinkReady'):
    assert name+'(' in event, name
init = main.split('void InitialiseWifi()', 1)[1].split('bool SendPacket(', 1)[0]
assert init.index('esp_wifi_set_storage(WIFI_STORAGE_RAM)') < init.index('esp_wifi_set_config(')
assert init.index('WifiMaintenanceInit(') < init.index('esp_wifi_start()')
command = main.split('void CommandTask(', 1)[1]
assert command.index('WifiControlLease wifi_lease(') < command.index('ProcessCommand(')
assert command.index('!wifi_lease.TimingReady()') < command.index('ProcessCommand(')
assert 'packet.command.type == factory_timer::CommandType::StartAt' in command
assert command.index('RefreshCommandSocketForLink(') < command.index('recvfrom(')

with tempfile.TemporaryDirectory(prefix='factory_wifi_') as temp:
    output = Path(temp)
    common = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
              '-I', str(ROOT/'tests/wifi-host/stubs'), '-I', str(ROOT/'main')]
    (output/'socket_recovery.inc').write_text(function(main,'RefreshCommandSocketForLink'))
    socket_test=output/'socket_recovery'
    compiled=subprocess.run(common+['-I',str(output),str(ROOT/'tests/wifi-host/socket_recovery_test.cpp'),'-o',str(socket_test)],capture_output=True,text=True,timeout=45)
    assert compiled.returncode==0,compiled.stdout+compiled.stderr
    socket_result=subprocess.run([str(socket_test)],capture_output=True,text=True,timeout=15)
    assert socket_result.returncode==0,socket_result.stdout+socket_result.stderr
    print(socket_result.stdout.rstrip())
    source = (ROOT/'main/wifi_maintenance.cpp').read_text()
    for mode in ('current', 'zones_default', 'zones_explicit_off', 'ignore_idle_roam_flag', 'old_retry_loop', 'no_active_guard', 'early_handover_connect'):
        variant = source
        if mode == 'old_retry_loop':
            variant = variant.replace('ctx.failures < 3 ? 0 : 3000000 + esp_random() % 1000001', '0')
        elif mode == 'no_active_guard':
            variant = variant.replace('!kIdleRoamingEnabled || active ||', '!kIdleRoamingEnabled || false ||')
        elif mode == 'early_handover_connect':
            variant = variant.replace(' && !ctx.awaiting_disconnect)', ')')
        elif mode == 'ignore_idle_roam_flag':
            variant = variant.replace('constexpr bool kIdleRoamingEnabled = false;', 'constexpr bool kIdleRoamingEnabled = true;')
        path = output/(mode+'.cpp'); path.write_text(variant)
        executable = output/mode
        option=[] if mode in ('zones_default','ignore_idle_roam_flag') else ['-DCONFIG_FACTORY_WIFI_IDLE_ROAM='+('0' if mode=='zones_explicit_off' else '1')]
        args = common + option + [f'-DFACTORY_WIFI_SOURCE_PATH="{path}"', str(ROOT/'tests/wifi-host/maintenance_test.cpp'),
                         str(ROOT/'main/countdown_timer.cpp'), '-o', str(executable)]
        build = subprocess.run(args, capture_output=True, text=True, timeout=45)
        assert build.returncode == 0, build.stdout+build.stderr
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
        if mode in ('current','zones_default','zones_explicit_off'):
            assert result.returncode == 0, result.stdout+result.stderr
            print(result.stdout.rstrip())
        else:
            assert variant != source and result.returncode != 0, mode
            print('REGRESSION_SENSITIVITY=PASS '+mode)
print('WIFI_INTEGRATION=PASS event wiring / RAM config / timing guard before command processing')
