#!/usr/bin/env python3
"""Generate/check zone profiles by default; shared-SSID roaming is opt-in.

The frozen settings are copied from ESP01. Only identity, planned zone SSID
and the explicitly selected roaming option may vary. Shared profiles always
go in a separate directory and never overwrite the production zone profiles.
"""
from __future__ import annotations
import argparse
import csv
import json
import re
from pathlib import Path
from generate_zone_sdkconfigs import expected, load_plan, ROAM, settings, set_setting, zone_counts

IDENTITY_RE = re.compile(r'^CONFIG_FACTORY_DEVICE_ID="ESP\d{2}"$', re.M)

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
    ap.add_argument('--mode', choices=('zones','shared'), default='zones')
    ap.add_argument('--ssid', help='Actual shared AP SSID; required when generating shared profiles')
    ap.add_argument('--output-dir', type=Path)
    ap.add_argument('--check-only', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not 1 <= args.start <= args.end <= 15:
        ap.error('Require 1 <= start <= end <= 15')
    output = (root/(args.output_dir or (Path('.') if args.mode=='zones' else Path('shared_profiles')))).resolve()
    if args.mode=='shared' and output==root:
        ap.error('Shared profiles must use a separate output directory')
    if args.mode=='zones' and args.ssid:
        ap.error('--ssid is only for --mode shared; edit network_zones.json for zone SSIDs')
    ssids,zones=load_plan()
    manifest=output/'network_profile_manifest.json'
    if args.mode=='shared':
        shared_ssid=args.ssid
        if args.check_only and shared_ssid is None and manifest.is_file():
            saved=json.loads(manifest.read_text())
            if saved.get('mode')!='shared': ap.error('Not a shared-SSID manifest')
            shared_ssid=saved.get('ssid')
        if not isinstance(shared_ssid,str) or not 1 <= len(shared_ssid.encode('utf-8')) <= 31 or any(ord(c)<32 for c in shared_ssid):
            ap.error('Provide --ssid with 1-31 UTF-8 bytes and no control characters')
        ssids=[shared_ssid]*3
    base_path = root / args.base
    base = base_path.read_text()
    matches = IDENTITY_RE.findall(base)
    if matches != ['CONFIG_FACTORY_DEVICE_ID="ESP01"']:
        raise SystemExit(f'{base_path}: expected exactly CONFIG_FACTORY_DEVICE_ID="ESP01"')
    if 'CONFIG_FACTORY_WIFI_BSSID_LOCK' not in settings(base):
        base=set_setting(base,'CONFIG_FACTORY_WIFI_BSSID_LOCK','n')

    network_failures = verify_network_identity(root, base)
    for failure in network_failures:
        print(f'NETWORK: FAIL {failure}')
    if not network_failures:
        print('NETWORK_IDENTITY_VERIFY=PASS mode=DHCP hostname=CONFIG_FACTORY_DEVICE_ID')

    failures = len(network_failures)
    if not args.check_only:
        output.mkdir(parents=True,exist_ok=True)
        if args.mode=='shared':
            manifest.write_text(json.dumps({'mode':'shared','ssid':ssids[0]},indent=2)+'\n')
    for n in range(args.start, args.end + 1):
        device = f'ESP{n:02d}'
        path = output / f'sdkconfig.esp{n:02d}'
        required_text = expected(base,n,ssids,zones)
        if args.mode=='shared': required_text=set_setting(required_text,ROAM,'y')
        if not args.check_only:
            path.write_text(required_text)
            if args.mode=='zones' and output==root:
                # Keep the supplied heartbeat/diagnostic builds on the same
                # board's home SSID without changing their diagnostic options.
                for variant in root.glob(f'sdkconfig.esp{n:02d}.*'):
                    text=set_setting(variant.read_text(),'CONFIG_FACTORY_WIFI_SSID',json.dumps(ssids[zones[device]-1],ensure_ascii=False))
                    variant.write_text(set_setting(text,ROAM,'n'))
        if not path.exists():
            print(f'{device}: MISSING {path.name}')
            failures += 1
            continue
        actual = path.read_text()
        required,values=settings(required_text),settings(actual)
        if any(values.get(key)!=value for key,value in required.items()):
            print(f'{device}: FAIL identity/network/frozen-setting mismatch')
            failures += 1
        else:
            print(f'{device}: PASS mode={args.mode} zone={zones[device]}')
    if not args.check_only:
        with (output/'commissioning.csv').open('w',newline='',encoding='utf-8') as stream:
            writer=csv.writer(stream)
            writer.writerow(['DeviceId','Zone','SSID','StationMac','ApBssid','Profile'])
            for n in range(args.start,args.end+1):
                device=f'ESP{n:02d}'
                writer.writerow([device,zones[device] if args.mode=='zones' else 'shared',ssids[zones[device]-1],'','',f'sdkconfig.esp{n:02d}'])
    if failures:
        print(f'FLEET_PROFILE_VERIFY=FAIL count={failures}')
        return 1
    print(f'FLEET_PROFILE_VERIFY=PASS range=ESP{args.start:02d}-ESP{args.end:02d}')
    print(f'NETWORK_PROFILE_MODE={args.mode} IDLE_ROAM={"ON" if args.mode=="shared" else "OFF"}')
    if args.mode=='zones': print('ZONE_COUNTS='+','.join(map(str,zone_counts(zones).values())))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
