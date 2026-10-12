"""Independently check timing samples, image quality, driver memory and live inputs."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--matrix',type=Path,default=root/'build/windows-renderer-api-comparison-final-tests/report.json')
parser.add_argument('--adapters',type=Path,required=True)
parser.add_argument('--output',type=Path,default=root/'build/windows-renderer-api-comparison-proof.json')
parser.add_argument('--input-snapshot',type=Path,help='Previously preserved input overrides for a historical measurement')
args = parser.parse_args()
read = lambda p: json.loads(Path(p).read_text(encoding='utf-8-sig'))
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
matrix = read(args.matrix)
assert matrix['passed'] and len(matrix['cases']) == 6 and matrix['samples'] == 20
assert matrix['settingsBefore'] == matrix['settingsAfter'] and matrix['excludedTests'] == ['parsec']
snapshot = read(args.input_snapshot) if args.input_snapshot else {'overrides':{}}
if args.input_snapshot:
    assert snapshot['matrixSha256'] == sha(args.matrix)
for name,digest in matrix['inputSha256'].items():
    override = snapshot['overrides'].get(name)
    if override:
        assert override['sha256'] == digest,name
    assert sha(Path(override['path']) if override else root/name) == digest,name
adapters = read(args.adapters)['adapters']
rows, total_pixels, artifacts = [], 0, {}


def percentile(values,fraction):
    values = sorted(values)
    index = (len(values)-1)*fraction
    lo = math.floor(index)
    return values[lo] + (values[math.ceil(index)]-values[lo])*(index-lo)


def image_error(path,frame,width,height):
    image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and (image.width(),image.height()) == (64,48),path
    pixels, error = bytes(image.constBits()), 0
    for y in range(48):
        sy = (47-y)*(height-1)//47
        for x in range(64):
            sx = x*(width-1)//63
            linear = (((sx+frame)&63)/128,((sy+3*frame)&63)/128,((sx+sy+7*frame)&63)/128)
            reference = [round((12.92*v if v <= .0031308 else 1.055*v**(1/2.4)-.055)*255) for v in linear]+[255]
            error = max(error,max(abs(pixels[(y*64+x)*4+c]-reference[c]) for c in range(4)))
    return error


for cell in matrix['cases']:
    assert sha(cell['result']) == cell['resultSha256'] and sha(cell['memory']) == cell['memorySha256']
    result, memory = read(cell['result']),read(cell['memory'])
    assert result['ok'] and memory['passed'] and memory['processExited']
    assert memory['requestedIntervalMs'] == 20 and not memory['instantaneousPeakProven']
    assert memory['maximumObservedIntervalMs'] < 500 and memory['sampleCount'] > 10
    assert result['referenceToleranceBytes'] == 1
    profile = Path(cell['profile'])
    stdout,stderr = (profile/'stdout.log').read_text(encoding='utf-8-sig'),(profile/'stderr.log').read_text(encoding='utf-8-sig')
    assert 'NATRON_API_COMPARISON_EXIT_REQUESTED' in stdout and 'NATRON_OWNED_INTERPRETER_EOF_OK' in stdout
    assert not (profile/'timeout.json').exists() and not re.search(r'VUID-|Vulkan runtime error:|Traceback|NATRON_LEGACY_EXCEPTION',stderr)
    width,height = result['sourceSize']
    assert [width,height] == ([3840,2160] if cell['resolution'] == '4K' else [7680,4320])
    assert result['counters'][6:10] == [width,height,width,height]
    if cell['backend'] == 'opengl':
        assert stderr.count('enabled=1') == 23 and 'renderer=NVIDIA GeForce RTX 5080' in stderr
        candidates = [a for a in adapters if a['vendorId'] == 0x10de and '5080' in a['name']]
    else:
        device = result['device']
        assert device['vendorId'] == (0x10de if cell['backend'] == 'vulkan-0' else 0x1002)
        assert not device['validationEnabled'] and not device['validationErrors']
        candidates = [a for a in adapters if a['vendorId'] == device['vendorId'] and a['deviceId'] == device['deviceId']]
    # DXGI may enumerate a second logical adapter for the same physical GPU.
    # Only accept the LUID actually present in the recorded driver counters.
    recorded_instances = set().union(*(s['bytes']['Adapter/Dedicated'] for s in memory['samples']))
    candidates = [a for a in candidates if a['counterInstance'] in recorded_instances]
    assert len(candidates) == 1,candidates
    adapter = candidates[0]
    for case in result['cases']:
        assert case['mode'] in ('cpu','gpu') and len(case['measurements']) == 20
        values, maximum = [], 0
        for measurement in case['measurements']:
            assert measurement['sourceCallbacks'] > 0 and measurement['probeCallbacks'] > 0
            assert measurement['milliseconds'] > 0
            if cell['backend'] != 'opengl':
                assert (measurement['nativeCallbacks'] > 0) == (case['mode'] == 'gpu')
                if measurement.get('directCallbacks') is not None:
                    assert (measurement['directCallbacks'] > 0) == (case['mode'] == 'gpu')
            png = Path(cell['result']).parent/measurement['png']
            assert sha(png) == measurement['sha256']
            maximum = max(maximum,image_error(png,measurement['frame'],width,height))
            artifacts[str(png)] = measurement['sha256']
            values.append(measurement['milliseconds'])
            total_pixels += 3072
        assert maximum <= case['maximumByteError']
        assert case['referenceWithinTolerance'] == (case['maximumByteError'] <= 1)
        if cell['backend'] != 'opengl' or case['mode'] == 'cpu':
            assert case['referenceWithinTolerance'],(cell['backend'],cell['resolution'],maximum)
        assert abs(case['p50Ms']-percentile(values,.5)) < 1e-6
        assert abs(case['p95Ms']-percentile(values,.95)) < 1e-6
        selected = [s for s in memory['samples'] if case['startedUnixNs'] <= s['wallUnixNs'] <= case['stoppedUnixNs']]
        assert len(selected) > 5
        instance = adapter['counterInstance']
        driver = {kind:max(s['bytes'][f'Adapter/{kind}'][instance] for s in selected)
                  for kind in ('Dedicated','Shared','Total Committed')}
        rows.append({'backend':cell['backend'],'resolution':cell['resolution'],'mode':case['mode'],'device':adapter['name'],
                     'samples':20,'p50Ms':case['p50Ms'],'p95Ms':case['p95Ms'],'maximumByteError':case['maximumByteError'],
                     'referenceWithinTolerance':case['referenceWithinTolerance'],'driverDedicatedPeakBytes':driver['Dedicated'],
                     'driverSharedPeakBytes':driver['Shared'],'driverCommittedPeakBytes':driver['Total Committed'],
                     'driverSampleCount':len(selected),'maximumObservedIntervalMs':memory['maximumObservedIntervalMs']})
table = args.output.with_suffix('.csv')
with table.open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
comparisons=[]
for resolution in ('4K','8K'):
    for mode in ('cpu','gpu'):
        old=next(r for r in rows if r['backend']=='opengl' and r['resolution']==resolution and r['mode']==mode)
        new=next(r for r in rows if r['backend']=='vulkan-0' and r['resolution']==resolution and r['mode']==mode)
        comparisons.append({'resolution':resolution,'mode':mode,'legacyP50Ms':old['p50Ms'],'vulkanP50Ms':new['p50Ms'],
                            'vulkanOverLegacyP50':new['p50Ms']/old['p50Ms'],
                            'outputsMeetSameTolerance':old['referenceWithinTolerance'] and new['referenceWithinTolerance']})
proof={'passed':True,'scope':matrix['scope'],'completionPlatform':'windows','fullVulkanMigrationComplete':False,
       'timedSamples':240,'processes':6,'pixelsCompared':total_pixels,'rows':rows,'rtxComparisons':comparisons,
       'legacyAccuracyFailures':[r for r in rows if not r['referenceWithinTolerance']],
       'dependencyDifference':matrix['legacyDependencyDifference'],
       'memoryScope':'adapter-wide maximum observed at 20 ms requested interval; includes other processes; instantaneous peak not proven',
       'parsecExcluded':True,'userSettingsSha256':matrix['settingsAfter'],
       'inputs':{str(p):sha(p) for p in (args.matrix,args.adapters,Path(__file__),root/'tools/windows-gpu-adapters.py')},
       'artifactSha256':artifacts,'table':str(table)}
if args.input_snapshot:
    proof['historicalInputSnapshot'] = {'path':str(args.input_snapshot),'sha256':sha(args.input_snapshot),
                                       'overrides':snapshot['overrides']}
args.output.write_text(json.dumps(proof,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in proof.items() if k not in ('artifactSha256','rows','inputs')}))
