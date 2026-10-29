"""Compare every shipped OFX ID/version with the pre-migration inventory.

Cache declarations are inventory evidence, not proof of rendering or overlays.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--cache', type=Path, required=True)
parser.add_argument('--baseline', type=Path, default=Path('build/ofx-vulkan-audit/plugins.csv'))
parser.add_argument('--output', type=Path, default=Path('build/native-ofx-compatibility'))
args = parser.parse_args()
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
with args.baseline.open(encoding='utf-8-sig', newline='') as stream:
    baseline = list(csv.DictReader(stream))
key = lambda row: (row['bundle'], row['id'], int(row['major']), int(row['minor']))
before = {key(row): row for row in baseline}
cache = ET.parse(args.cache).getroot()
assert cache.get('version') == 'NatronOFXCachev1'
rows, modules = [], {}
for bundle in cache.findall('bundle'):
    binary = bundle.find('binary')
    name = Path(binary.get('bundle_path')).name
    if name not in {row['bundle'] for row in baseline}:
        raise ValueError('Unexpected bundle in shipped-plugin inventory: ' + name)
    path = Path(binary.get('path'))
    modules[name] = {'path': str(path.resolve()), 'sha256': digest(path)}
    for plugin in bundle.findall('plugin'):
        props = {p.get('name'): [v.get('value') for v in p.findall('value')]
                 for p in plugin.findall('apiproperties/property')}
        declared = lambda name: next(iter(props.get(name, [])), 'not-cached' if name in props else 'undeclared')
        row = {'bundle': name, 'id': plugin.get('name'),
               'major': int(plugin.get('major_version')), 'minor': int(plugin.get('minor_version')),
               'descriptorVulkan': declared('NatronOfxImageEffectPropVulkanRenderSupported'),
               'descriptorVulkanRequired': declared('NatronOfxImageEffectPropVulkanRenderRequired'),
               'descriptorNativeOverlays': declared('NatronOfxPropNativeOverlays'),
               'descriptorOpenGL': declared('OfxImageEffectPropOpenGLRenderSupported'),
               'baselineOpenGL': before.get((name, plugin.get('name'), int(plugin.get('major_version')),
                                             int(plugin.get('minor_version'))), {}).get('declaredOpenGLRender', 'absent'),
               'renderValidation': 'See per-feature render reports; cache metadata alone is insufficient',
               'overlayValidation': 'See per-interact input/Undo reports; declaration alone is insufficient'}
        rows.append(row)
after = {key(row) for row in rows}
assert len(after) == len(rows), 'Duplicate bundle/ID/version'
missing, added = sorted(before.keys() - after), sorted(after - before.keys())
args.output.mkdir(parents=True, exist_ok=True)
table = args.output / 'plugins.csv'
with table.open('w', encoding='utf-8', newline='') as stream:
    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(sorted(rows, key=key))
report = {'passed': not missing and not added, 'scope': 'All shipped OFX registration IDs/versions and cached declarations',
          'functional_compatibility_complete': False, 'versions': len(rows),
          'unique_ids': len({row['id'] for row in rows}), 'bundles': modules,
          'missing': missing, 'added': added, 'baseline_sha256': digest(args.baseline),
          'cache': str(args.cache.resolve()), 'cache_sha256': digest(args.cache), 'table_sha256': digest(table)}
(args.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ('bundles',)}))
if not report['passed']:
    raise SystemExit(1)
