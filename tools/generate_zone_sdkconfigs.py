#!/usr/bin/env python3
"""Generate/check a separate ESP01-15 zone profile set with idle roaming off.

The standard profiles are never overwritten. Password and non-identity settings
come from sdkconfig.esp01; no passwords are stored in the commissioning table.
"""
from pathlib import Path
import argparse
import csv
import json
import re

ROOT = Path(__file__).resolve().parents[1]
ID = re.compile(r'^CONFIG_FACTORY_DEVICE_ID="ESP01"$', re.M)
SSID = re.compile(r'^CONFIG_FACTORY_WIFI_SSID=".*"$', re.M)
ROAM = 'CONFIG_FACTORY_WIFI_IDLE_ROAM'


def set_setting(text, key, value):
    pattern = re.compile(r'^(?:'+re.escape(key)+r'=.*|# '+re.escape(key)+r' is not set)$', re.M)
    line = '# '+key+' is not set' if value == 'n' else key+'='+value
    if pattern.search(text):
        return pattern.sub(lambda _: line, text)
    return text.rstrip()+'\n'+line+'\n'


def validate_plan(ssids, zones):
    if not isinstance(ssids, list) or len(ssids) != 3 or not all(isinstance(s,str) for s in ssids) or len(set(ssids)) != 3:
        raise ValueError('Provide three distinct zone SSIDs')
    if any(not 1 <= len(s.encode('utf-8')) <= 31 or any(ord(c)<32 for c in s) for s in ssids):
        raise ValueError('Each SSID must contain 1-31 UTF-8 bytes without control characters')
    if not isinstance(zones,dict) or set(zones)!={f'ESP{n:02d}' for n in range(1,16)} or any(type(v) is not int or v not in (1,2,3) for v in zones.values()):
        raise ValueError('Zone map needs ESP01-ESP15 once each, with integer Zone 1, 2 or 3')


def zone_counts(zones):
    return {str(zone):sum(value==zone for value in zones.values()) for zone in (1,2,3)}


def load_plan():
    plan=json.loads((ROOT/'network_zones.json').read_text())
    ssids,zones=plan['zone_ssids'],plan['device_zones']
    validate_plan(ssids,zones)
    return ssids,zones


def expected(base, number, ssids, zones):
    text = ID.sub(lambda _: f'CONFIG_FACTORY_DEVICE_ID="ESP{number:02d}"', base)
    text = SSID.sub(lambda _: 'CONFIG_FACTORY_WIFI_SSID='+json.dumps(ssids[zones[f'ESP{number:02d}']-1], ensure_ascii=False), text)
    return set_setting(text, ROAM, 'n')


def settings(text):
    # ESP-IDF expands these override files on first build. Check the frozen
    # overrides semantically, permitting extra generated SDK defaults/comments.
    result={}
    for line in text.splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key,value=line.split('=',1); result[key]=value
        else:
            match=re.fullmatch(r'# (CONFIG_\w+) is not set',line)
            if match: result[match[1]]='n'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zone-ssids', nargs=3, metavar=('ZONE1', 'ZONE2', 'ZONE3'))
    parser.add_argument('--zone-map', type=Path, help='CSV with DeviceId,Zone; all 15 identities, allocation follows physical positions')
    parser.add_argument('--output-dir', type=Path, default=Path('zone_profiles'))
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    output = (ROOT/args.output_dir).resolve()
    if output == ROOT:
        parser.error('Use a separate output directory; standard profiles must not be overwritten')
    manifest = output/'zone_profile_manifest.json'
    ssids,zones=load_plan()
    saved=json.loads(manifest.read_text()) if manifest.is_file() else {}
    ssids=args.zone_ssids or saved.get('zone_ssids',ssids)
    zones=saved.get('device_zones',zones)
    if args.zone_map:
        with args.zone_map.open(newline='',encoding='utf-8-sig') as stream:
            rows=list(csv.DictReader(stream))
        try:
            zones={row['DeviceId']:int(row['Zone']) for row in rows}
        except (KeyError,ValueError):
            parser.error('Zone map needs DeviceId and integer Zone columns')
        if len(rows)!=15:
            parser.error('Zone map must contain exactly 15 rows')
    try:
        validate_plan(ssids,zones)
    except ValueError as error:
        parser.error(str(error))
    base = (ROOT/'sdkconfig.esp01').read_text()
    if len(ID.findall(base)) != 1 or len(SSID.findall(base)) != 1:
        parser.error('Base must contain exactly one ESP01 identity and one SSID')
    if 'CONFIG_FACTORY_WIFI_BSSID_LOCK' not in settings(base):
        base += '\n# CONFIG_FACTORY_WIFI_BSSID_LOCK is not set\n'
    if not args.check_only:
        output.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps({'zone_ssids': ssids, 'zone_counts':zone_counts(zones), 'device_zones':zones}, indent=2)+'\n')
        for number in range(1,16):
            (output/f'sdkconfig.esp{number:02d}').write_text(expected(base, number, ssids, zones))
        with (output/'commissioning.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = csv.writer(stream)
            writer.writerow(['DeviceId','Zone','SSID','StationMac','ApBssid','Profile'])
            for number in range(1,16):
                zone=zones[f'ESP{number:02d}']
                writer.writerow([f'ESP{number:02d}',zone,ssids[zone-1],'','',f'sdkconfig.esp{number:02d}'])
    failures=[]
    for number in range(1,16):
        path=output/f'sdkconfig.esp{number:02d}'
        required=settings(expected(base,number,ssids,zones))
        actual=settings(path.read_text()) if path.is_file() else {}
        if any(actual.get(key)!=value for key,value in required.items()): failures.append(path.name)
    if failures:
        print('ZONE_PROFILE_VERIFY=FAIL profiles='+','.join(failures))
        return 1
    print('ZONE_PROFILE_VERIFY=PASS boards=15 zones=3 zone_counts='+','.join(map(str,zone_counts(zones).values()))+' standard_profiles_unchanged=1')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
