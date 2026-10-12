"""Classify live interact declarations; absence of UI is not incompatibility."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--directory', type=Path, required=True)
args = parser.parse_args()
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
native = json.loads((args.directory / 'native.json').read_text(encoding='utf-8-sig'))
legacy = json.loads((args.directory / 'legacy-discovery.json').read_text(encoding='utf-8-sig'))
assert not native['failures'] and not legacy['failures']
key = lambda p: (p['binary'], p['id'], p['major'], p['minor'])
current = {key(p): p for p in native['pluginVersions']}
discovery = {key(p): p for p in legacy['pluginVersions']}
assert len(current) == len(native['pluginVersions'])
assert current.keys() == discovery.keys()
rows, modules = [], {}
render_declarations_available = all('retiredOpenGLRendering' in c
                                    for p in current.values() for c in p['contexts'])
for k, p in sorted(current.items()):
    native_required = False
    problems = set()
    for mode, plugin in [('native', p), ('legacy-discovery', discovery[k])]:
        for context in plugin['contexts']:
            if context['drawSuiteOverlay']:
                native_required = True
            if context['legacyOverlay'] and not context['drawSuiteOverlay']:
                problems.add('Viewer overlay')
            for param in context['params']:
                native_required |= param['drawSuiteCustom'] or param['drawSuiteBackground']
                if param['legacyCustom'] and not param['drawSuiteCustom']:
                    problems.add('Custom control: ' + param['name'])
                if param['legacyBackground'] and not param['drawSuiteBackground']:
                    problems.add('Curve background: ' + param['name'])
    changed = p['contexts'] != discovery[k]['contexts']
    render_modes, render_problems, retired_declarations = set(), set(), set()
    if render_declarations_available:
        for context in p['contexts']:
            retired = context['retiredOpenGLRendering']
            retired_declarations.add(retired)
            native_render = context['vulkanRendering']
            if retired == 'needed' and not native_render:
                render_modes.add('unsupported-opengl-required')
                render_problems.add(context['context'] + ': requires OpenGL without a Vulkan implementation')
            elif context['vulkanRequired'] or retired == 'needed':
                render_modes.add('vulkan-required')
            elif native_render:
                render_modes.add('cpu-or-vulkan')
            else:
                render_modes.add('cpu')
    rows.append({'bundle': Path(p['bundle']).name, 'id': p['id'], 'label': p['label'],
                 'major': p['major'], 'minor': p['minor'],
                 'status': 'unsupported-ui' if problems else 'drawsuite-declared' if native_required else 'not-required',
                 'unsupportedUi': '; '.join(sorted(problems)), 'legacyCapabilityChangesDeclaration': changed,
                 'binary': p['binary'], 'renderCompatibility': 'not-tested-by-this-audit',
                 'declaredRenderModes': '; '.join(sorted(render_modes)) if render_declarations_available else 'not-inspected',
                 'retiredOpenGLDeclarations': '; '.join(sorted(retired_declarations)),
                 'unsupportedRender': '; '.join(sorted(render_problems))})
    path = Path(p['binary'])
    modules[str(path)] = sha(path)
for name, selected in [('plugins', rows), ('unsupported', [r for r in rows if r['status'] == 'unsupported-ui']),
                       ('unsupported-render', [r for r in rows if r['unsupportedRender']])]:
    with (args.directory / f'{name}.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        fields = list(rows[0]) if rows else ['bundle', 'id', 'label', 'major', 'minor', 'status', 'unsupportedUi']
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(selected)
report = {'passed': True, 'scope': 'Live effect/context/parameter interact declarations; effect/interact instances and rendering are not invoked.',
          'versions': len(rows), 'uniqueIds': len({r['id'] for r in rows}),
          'unsupportedVersions': sum(r['status'] == 'unsupported-ui' for r in rows),
          'nativeVersions': sum(r['status'] == 'drawsuite-declared' for r in rows),
          'noUiRequiredVersions': sum(r['status'] == 'not-required' for r in rows),
          'conditionalDeclarations': sum(r['legacyCapabilityChangesDeclaration'] for r in rows),
          'renderCompatibilityVerified': False, 'moduleSha256': modules,
          'renderDeclarationsAvailable': render_declarations_available,
          'unsupportedRenderVersions': sum(bool(r['unsupportedRender']) for r in rows) if render_declarations_available else None,
          'declaredRenderModeCounts': {mode: sum(mode in r['declaredRenderModes'].split('; ') for r in rows)
                                      for mode in ('cpu', 'cpu-or-vulkan', 'vulkan-required', 'unsupported-opengl-required')}
                                      if render_declarations_available else None,
          'artifactSha256': {name: sha(args.directory / name) for name in ('native.json', 'legacy-discovery.json', 'plugins.csv', 'unsupported.csv', 'unsupported-render.csv')}}
(args.directory / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k not in ('moduleSha256', 'artifactSha256')}))
