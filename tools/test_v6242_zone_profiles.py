#!/usr/bin/env python3
"""Check separate zone generation, commissioning identities and negative profiles."""
from pathlib import Path
import csv
import hashlib
import json
import subprocess
import sys
import tempfile
from generate_zone_sdkconfigs import load_plan, zone_counts

ROOT=Path(__file__).resolve().parents[1]
tool=ROOT/'tools/generate_zone_sdkconfigs.py'
_,default_zones=load_plan()
before={p.name:hashlib.sha256(p.read_bytes()).digest() for p in ROOT.glob('sdkconfig.esp??')}
def call(*args):
    return subprocess.run([sys.executable,str(tool),*map(str,args)],capture_output=True,text=True,timeout=15)
with tempfile.TemporaryDirectory(prefix='factory_zone_') as directory:
    output=Path(directory)
    result=call('--zone-ssids','FT-ZONE1','FT-ZONE2','FT-ZONE3','--output-dir',output)
    assert result.returncode==0,result.stdout+result.stderr
    assert call('--output-dir',output,'--check-only').returncode==0
    expanded=output/'sdkconfig.esp01'
    expanded.write_text(expanded.read_text()+'\n# Generated SDK defaults\nCONFIG_EXAMPLE_GENERATED_DEFAULT=y\n')
    assert call('--output-dir',output,'--check-only').returncode==0
    rows=list(csv.DictReader((output/'commissioning.csv').open()))
    assert len(rows)==15 and len({r['DeviceId'] for r in rows})==15
    assert [sum(r['Zone']==str(z) for r in rows) for z in (1,2,3)]==list(zone_counts(default_zones).values())
    assert all(not r['StationMac'] and not r['ApBssid'] for r in rows)
    assert 'PASSWORD' not in (output/'commissioning.csv').read_text()
    for original,replacement in [('CONFIG_FACTORY_DEVICE_ID="ESP06"','CONFIG_FACTORY_DEVICE_ID="ESP05"'),
                                 (f'CONFIG_FACTORY_WIFI_SSID="FT-ZONE{default_zones["ESP06"]}"','CONFIG_FACTORY_WIFI_SSID="WRONG-ZONE"'),
                                 ('# CONFIG_FACTORY_WIFI_IDLE_ROAM is not set','CONFIG_FACTORY_WIFI_IDLE_ROAM=y'),
                                 ('# CONFIG_FACTORY_WIFI_BSSID_LOCK is not set','CONFIG_FACTORY_WIFI_BSSID_LOCK=y'),
                                 ('CONFIG_FACTORY_START_TASK_TEST_DELAY_US=0','CONFIG_FACTORY_START_TASK_TEST_DELAY_US=500')]:
        profile=output/'sdkconfig.esp06';good=profile.read_text();assert original in good
        profile.write_text(good.replace(original,replacement))
        assert call('--output-dir',output,'--check-only').returncode==1
        profile.write_text(good)
    assert call('--zone-ssids','same','same','other','--output-dir',output).returncode!=0
    assert call('--zone-ssids','x'*32,'two','three','--output-dir',output).returncode!=0
    assert call('--zone-ssids','one','two','three','--output-dir',ROOT).returncode!=0
    mapping=output/'zones.csv'
    zones={f'ESP{n:02d}':(n-1)//5+1 for n in range(1,16)}
    zones['ESP04'],zones['ESP06']=2,1
    zones['ESP03'],zones['ESP11']=3,1
    zones['ESP05'],zones['ESP12']=3,1
    with mapping.open('w',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(['DeviceId','Zone']);writer.writerows(zones.items())
    custom=call('--zone-ssids','FT-ZONE1','FT-ZONE2','FT-ZONE3','--zone-map',mapping,'--output-dir',output)
    assert custom.returncode==0,custom.stdout+custom.stderr
    assert call('--output-dir',output,'--check-only').returncode==0
    for device,zone in [('ESP01',1),('ESP04',2),('ESP03',3)]:
        assert f'CONFIG_FACTORY_WIFI_SSID="FT-ZONE{zone}"' in (output/f'sdkconfig.{device.lower()}').read_text()
    assert call('--zone-ssids','NEW-ZONE1','NEW-ZONE2','NEW-ZONE3','--output-dir',output).returncode==0
    assert 'CONFIG_FACTORY_WIFI_SSID="NEW-ZONE2"' in (output/'sdkconfig.esp04').read_text()
    assert 'CONFIG_FACTORY_WIFI_SSID="NEW-ZONE3"' in (output/'sdkconfig.esp05').read_text()
    # Physical positions may need unequal zone counts, independent of the
    # analyzer bench fixture. There is no five-clients-per-zone firmware limit.
    zones={f'ESP{n:02d}':1 if n<=6 else 2 if n<=10 else 3 for n in range(1,16)}
    with mapping.open('w',newline='') as stream:
        writer=csv.writer(stream);writer.writerow(['DeviceId','Zone']);writer.writerows(zones.items())
    unequal=call('--zone-ssids','FT-ZONE1','FT-ZONE2','FT-ZONE3','--zone-map',mapping,'--output-dir',output)
    assert unequal.returncode==0,unequal.stdout+unequal.stderr
    assert 'zone_counts=6,4,5' in unequal.stdout
    manifest=json.loads((output/'zone_profile_manifest.json').read_text())
    assert manifest['zone_counts']=={'1':6,'2':4,'3':5} and 'boards_per_zone' not in manifest
    assert call('--output-dir',output,'--check-only').returncode==0
    assert call('--zone-ssids','NEW-ZONE1','NEW-ZONE2','NEW-ZONE3','--output-dir',output).returncode==0
    assert json.loads((output/'zone_profile_manifest.json').read_text())['device_zones']==zones
    rows=list(csv.DictReader((output/'commissioning.csv').open()))
    assert [sum(r['Zone']==str(z) for r in rows) for z in (1,2,3)]==[6,4,5]
    for invalid in ('0','4',''):
        text=mapping.read_text();mapping.write_text(text.replace('ESP15,3','ESP15,'+invalid))
        assert call('--zone-ssids','one','two','three','--zone-map',mapping,'--output-dir',output).returncode!=0
        mapping.write_text(text)
    mapping.write_text(mapping.read_text().replace('ESP15,3','ESP14,3'))
    assert call('--zone-ssids','one','two','three','--zone-map',mapping,'--output-dir',output).returncode!=0
    assert before=={p.name:hashlib.sha256(p.read_bytes()).digest() for p in ROOT.glob('sdkconfig.esp??')}
print('ZONE_PROFILES_TEST=PASS 15 identities / 3 SSIDs / commissioning / bench and 6-4-5 position maps / regeneration preserves allocation / invalid-zone-ID-lock-diagnostic rejection / originals unchanged')
