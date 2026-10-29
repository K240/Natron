"""Check shipped Vulkan OFX sources against their fixed revisions and patches."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
config = root / 'build-configs/windows-ofx'
pins = json.loads((config / 'sources.json').read_text(encoding='utf-8'))['repositories']


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args], stderr=subprocess.PIPE)


def patch(directory, path):
    expected = path.read_bytes()
    actual = git(directory, 'diff', '--binary', '--ignore-submodules=all')
    if actual != expected:
        raise RuntimeError(f'Source differs from saved patch: {path.name}')
    git(directory, 'apply', '--reverse', '--check', str(path))
    return hashlib.sha256(expected).hexdigest()


result = {}
for name in ('openfx-io', 'openfx-misc', 'openfx-arena'):
    directory = root / 'build/ofx-src' / name
    head = git(directory, 'rev-parse', 'HEAD').decode().strip()
    if head != pins[name]:
        raise RuntimeError(f'Unpinned source: {name}')
    submodules = {}
    for line in git(directory, 'ls-tree', 'HEAD').decode().splitlines():
        mode, kind, revision, path = line.replace('\t', ' ').split(' ', 3)
        if kind != 'commit':
            continue
        actual = git(directory / path, 'rev-parse', 'HEAD').decode().strip()
        if actual != revision:
            raise RuntimeError(f'Unpinned submodule: {name}/{path}')
        if path != 'SupportExt' and git(directory / path, 'status', '--porcelain').strip():
            raise RuntimeError(f'Unexpected SDK/submodule changes: {name}/{path}')
        submodules[path] = actual
    result[name] = {
        'revision': head, 'submodules': submodules,
        'sourcePatchSha256': patch(directory, config / (name + '.patch')),
        'supportPatchSha256': patch(directory / 'SupportExt', config / (name + '-supportext-vulkan.patch')),
    }
print(json.dumps({'passed': True, 'repositories': result}, indent=2))
