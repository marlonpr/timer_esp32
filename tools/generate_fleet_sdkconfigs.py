#!/usr/bin/env python3
"""Generate ESP01..ESP15 classic-ESP32 sdkconfig profiles from sdkconfig.esp01.

Only CONFIG_FACTORY_DEVICE_ID may differ. The script verifies this invariant
immediately after writing, preventing profile drift across the fleet.
"""
from __future__ import annotations
import argparse
import difflib
import re
from pathlib import Path

IDENTITY_RE = re.compile(r'^CONFIG_FACTORY_DEVICE_ID="ESP\d{2}"$', re.M)

def normalized(text: str) -> str:
    return IDENTITY_RE.sub('CONFIG_FACTORY_DEVICE_ID="ESPXX"', text)

def verify_network_identity(root: Path, base: str) -> list[str]:
    failures: list[str] = []
    # Fleet profiles must remain DHCP clients. No factory static-IP/hostname
    # setting may be cloned from ESP01.
    forbidden = re.compile(r'^CONFIG_FACTORY_.*(?:STATIC.*IP|IP.*ADDR|HOSTNAME).*$', re.M)
    bad = forbidden.findall(base)
    if bad:
        failures.append('base sdkconfig contains clone-unsafe factory network identity: ' + ', '.join(bad))
    main_cpp = (root / 'main/main.cpp').read_text()
    if 'esp_netif_create_default_wifi_sta()' not in main_cpp:
        failures.append('default DHCP Wi-Fi station creation not found')
    if 'esp_netif_set_hostname(wifi_sta_netif, CONFIG_FACTORY_DEVICE_ID)' not in main_cpp:
        failures.append('station hostname is not derived from CONFIG_FACTORY_DEVICE_ID')
    # set_ip_info is allowed only for clearing a rejected DHCP lease to 0.0.0.0.
    set_ip_calls = main_cpp.count('esp_netif_set_ip_info(')
    if set_ip_calls != 1 or 'const esp_netif_ip_info_t empty_ip_info{};' not in main_cpp:
        failures.append(f'unexpected static-IP path: esp_netif_set_ip_info calls={set_ip_calls}')
    return failures

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='sdkconfig.esp01')
    ap.add_argument('--start', type=int, default=1)
    ap.add_argument('--end', type=int, default=15)
    ap.add_argument('--check-only', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    base_path = root / args.base
    base = base_path.read_text()
    matches = IDENTITY_RE.findall(base)
    if matches != ['CONFIG_FACTORY_DEVICE_ID="ESP01"']:
        raise SystemExit(f'{base_path}: expected exactly CONFIG_FACTORY_DEVICE_ID="ESP01"')
    base_norm = normalized(base)

    network_failures = verify_network_identity(root, base)
    for failure in network_failures:
        print(f'NETWORK: FAIL {failure}')
    if not network_failures:
        print('NETWORK_IDENTITY_VERIFY=PASS mode=DHCP hostname=CONFIG_FACTORY_DEVICE_ID')

    failures = len(network_failures)
    for n in range(args.start, args.end + 1):
        device = f'ESP{n:02d}'
        path = root / f'sdkconfig.esp{n:02d}'
        expected = base.replace('CONFIG_FACTORY_DEVICE_ID="ESP01"', f'CONFIG_FACTORY_DEVICE_ID="{device}"')
        if not args.check_only:
            path.write_text(expected)
        if not path.exists():
            print(f'{device}: MISSING {path.name}')
            failures += 1
            continue
        actual = path.read_text()
        if normalized(actual) != base_norm:
            print(f'{device}: FAIL non-identity difference detected')
            diff = difflib.unified_diff(
                base_norm.splitlines(), normalized(actual).splitlines(),
                fromfile=base_path.name, tofile=path.name, lineterm='')
            for line in list(diff)[:60]: print(line)
            failures += 1
        elif actual != expected:
            print(f'{device}: FAIL identity/content mismatch')
            failures += 1
        else:
            print(f'{device}: PASS identity-only profile')
    if failures:
        print(f'FLEET_PROFILE_VERIFY=FAIL count={failures}')
        return 1
    print(f'FLEET_PROFILE_VERIFY=PASS range=ESP{args.start:02d}-ESP{args.end:02d}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
