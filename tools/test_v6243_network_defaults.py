#!/usr/bin/env python3
"""Validate the shipped zone default and isolated opt-in shared-SSID profiles."""
from pathlib import Path
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from generate_zone_sdkconfigs import load_plan, settings, zone_counts

ROOT=Path(__file__).resolve().parents[1]
tool=ROOT/'tools/generate_fleet_sdkconfigs.py'
ssids,zones=load_plan()

def call(*args):
    return subprocess.run([sys.executable,str(tool),*map(str,args)],capture_output=True,text=True,timeout=15)

before={p.name:hashlib.sha256(p.read_bytes()).digest() for p in ROOT.glob('sdkconfig.esp*')}
result=call('--check-only')
assert result.returncode==0,result.stdout+result.stderr
assert 'NETWORK_PROFILE_MODE=zones IDLE_ROAM=OFF' in result.stdout
if json.loads((ROOT/'network_zones.json').read_text()).get('map_purpose')=='bench':
    assert len({zones[d] for d in ('ESP01','ESP04','ESP03')})==3
for number in range(1,16):
    device=f'ESP{number:02d}'
    for path in ROOT.glob(f'sdkconfig.esp{number:02d}*'):
        values=settings(path.read_text())
        assert values['CONFIG_FACTORY_DEVICE_ID']==json.dumps(device),path
        assert values['CONFIG_FACTORY_WIFI_SSID']==json.dumps(ssids[zones[device]-1],ensure_ascii=False),path
        assert values['CONFIG_FACTORY_WIFI_IDLE_ROAM']=='n',path
rows=list(csv.DictReader((ROOT/'commissioning.csv').open()))
assert len(rows)==15
assert [sum(row['Zone']==str(z) for row in rows) for z in (1,2,3)]==list(zone_counts(zones).values())
assert all(row['SSID']==ssids[zones[row['DeviceId']]-1] for row in rows)
assert 'PASSWORD' not in (ROOT/'commissioning.csv').read_text()

with tempfile.TemporaryDirectory(prefix='factory_shared_optin_') as directory:
    output=Path(directory)
    assert call('--mode','shared','--output-dir',output).returncode!=0
    assert call('--mode','shared','--ssid','FT-SHARED','--output-dir',ROOT).returncode!=0
    result=call('--mode','shared','--ssid','FT-SHARED','--output-dir',output)
    assert result.returncode==0,result.stdout+result.stderr
    assert call('--mode','shared','--output-dir',output,'--check-only').returncode==0
    for number in range(1,16):
        values=settings((output/f'sdkconfig.esp{number:02d}').read_text())
        assert values['CONFIG_FACTORY_WIFI_SSID']=='"FT-SHARED"'
        assert values['CONFIG_FACTORY_WIFI_IDLE_ROAM']=='y'
        assert values['CONFIG_FACTORY_DEVICE_ID']==f'"ESP{number:02d}"'
    path=output/'sdkconfig.esp03';good=path.read_text()
    for original,replacement in [('CONFIG_FACTORY_WIFI_IDLE_ROAM=y','# CONFIG_FACTORY_WIFI_IDLE_ROAM is not set'),
                                 ('CONFIG_FACTORY_WIFI_SSID="FT-SHARED"','CONFIG_FACTORY_WIFI_SSID="FT-ZONE3"'),
                                 ('CONFIG_FACTORY_START_TASK_TEST_DELAY_US=0','CONFIG_FACTORY_START_TASK_TEST_DELAY_US=500')]:
        assert original in good
        path.write_text(good.replace(original,replacement))
        assert call('--mode','shared','--output-dir',output,'--check-only').returncode==1
        path.write_text(good)
    path.write_text(good+'\nCONFIG_EXAMPLE_GENERATED_DEFAULT=y\n')
    assert call('--mode','shared','--output-dir',output,'--check-only').returncode==0
with tempfile.TemporaryDirectory(prefix='factory_position_map_') as directory:
    # Execute the actual default-profile generator with a position-based plan,
    # keeping the supplied bench profiles untouched.
    copied=Path(directory)
    (copied/'tools').mkdir();(copied/'main').mkdir()
    for name in ('generate_fleet_sdkconfigs.py','generate_zone_sdkconfigs.py'):
        shutil.copyfile(ROOT/'tools'/name,copied/'tools'/name)
    shutil.copyfile(ROOT/'main/main.cpp',copied/'main/main.cpp')
    for profile in ROOT.glob('sdkconfig.esp*'): shutil.copyfile(profile,copied/profile.name)
    allocation={f'ESP{n:02d}':1 if n<=6 else 2 if n<=10 else 3 for n in range(1,16)}
    (copied/'network_zones.json').write_text(json.dumps({'map_purpose':'installation','zone_ssids':ssids,'device_zones':allocation}))
    for args in ([],['--check-only']):
        result=subprocess.run([sys.executable,str(copied/'tools/generate_fleet_sdkconfigs.py'),*args],capture_output=True,text=True,timeout=15)
        assert result.returncode==0,result.stdout+result.stderr
        assert 'ZONE_COUNTS=6,4,5' in result.stdout
    installed=list(csv.DictReader((copied/'commissioning.csv').open()))
    assert [sum(row['Zone']==str(z) for row in installed) for z in (1,2,3)]==[6,4,5]
    for profile in copied.glob('sdkconfig.esp*'):
        values=settings(profile.read_text());device=json.loads(values['CONFIG_FACTORY_DEVICE_ID'])
        assert values['CONFIG_FACTORY_WIFI_SSID']==json.dumps(ssids[allocation[device]-1],ensure_ascii=False),profile
        assert values['CONFIG_FACTORY_WIFI_IDLE_ROAM']=='n'
assert before=={p.name:hashlib.sha256(p.read_bytes()).digest() for p in ROOT.glob('sdkconfig.esp*')}
kconfig=(ROOT/'main/Kconfig.projbuild').read_text()
assert re.search(r'config FACTORY_WIFI_IDLE_ROAM\n.*?default n',kconfig,re.S)
build=(ROOT/'tools/build_fleet.ps1').read_text()
assert "[string]$NetworkMode = 'Zones'" in build
assert '--mode shared' in build and 'shared_profiles' in build
assert 'zone-v6243' in build and 'shared-v6243' in build
print('NETWORK_DEFAULTS=PASS bench map / unequal 6-4-5 installation plan / variants follow positions / roaming OFF / shared SSID opt-in and isolated / frozen settings checked')
