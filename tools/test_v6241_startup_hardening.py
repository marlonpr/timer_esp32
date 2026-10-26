#!/usr/bin/env python3
"""Compile the real DS3231 driver and main.cpp boot functions with fault-injection APIs."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source: str, name: str) -> str:
    """These selected functions have balanced braces in their literals/comments."""
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def run(command):
    result = subprocess.run([str(item) for item in command], capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr + f'\nCommand failed: {command}')
    if result.stdout:
        print(result.stdout.rstrip(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver-source', type=Path, help='Override actual driver source for regression sensitivity checks')
    parser.add_argument('--main-source', type=Path, help='Override main.cpp for regression sensitivity checks')
    args = parser.parse_args()
    host = ROOT / 'tests/startup-host'
    driver = ROOT / 'components/rtc_discipline'
    source = (args.main_source or ROOT/'main/main.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='factory_startup_') as temp:
        output = Path(temp)
        includes = ['-I', host/'stubs', '-I', host, '-I', driver, '-I', ROOT/'main', '-I', output]
        flags = ['-Wall', '-Wextra', '-Werror']
        objects = []
        for item in (args.driver_source or driver/'ds3231.c', host/'fake_i2c.c'):
            obj = output / (item.stem + '.o')
            run(['gcc', '-std=c11', *flags, *includes, '-c', item, '-o', obj])
            objects.append(obj)
        unit = output / 'ds3231_test'
        run(['gcc', '-std=c11', *flags, *includes, host/'ds3231_test.c', *objects, '-o', unit])
        run([unit])
        (output/'startup_functions.inc').write_text('\n'.join(function(source, name) for name in (
            'InitialiseRtcDiscipline', 'InitialiseWifi', 'RtcRunGateQualified', 'app_main')))
        for lock in (0, 1):
            boot = output / f'startup_lock_{lock}'
            run(['g++', '-std=c++17', *flags, f'-DCONFIG_FACTORY_WIFI_BSSID_LOCK={lock}',
                 *includes, host/'startup_test.cpp', ROOT/'main/network_policy.cpp', *objects, '-o', boot])
            run([boot])
    print('STARTUP_HARDENING=PASS actual driver + extracted boot/Wi-Fi/START gates; host APIs, not hardware')


if __name__ == '__main__':
    main()
