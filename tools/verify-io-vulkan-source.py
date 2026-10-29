"""Check that the current IO sources exactly match the pinned, saved patches."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
sources = root / 'build/ofx-src/openfx-io'
config = root / 'build-configs/windows-ofx'


def git(directory, *args):
    return subprocess.run(['git', '-C', str(directory), *args], check=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


pin = json.loads((config / 'sources.json').read_text(encoding='utf-8'))['repositories']['openfx-io']
assert git(sources, 'rev-parse', 'HEAD').decode().strip() == pin, 'IO source pin changed'
result = {'passed': True, 'ioCommit': pin, 'submodules': {}, 'patchSha256': {}}
for line in git(sources, 'ls-tree', 'HEAD').decode().splitlines():
    fields = line.split()
    if fields[0] != '160000':
        continue
    commit, name = fields[2:]
    directory = sources / name
    assert git(directory, 'rev-parse', 'HEAD').decode().strip() == commit, f'{name} pin changed'
    if name != 'SupportExt':
        assert not git(directory, 'diff', '--binary'), f'Unexpected changes in {name}'
    result['submodules'][name] = commit
for directory, name in [(sources, 'openfx-io.patch'), (sources / 'SupportExt', 'openfx-io-supportext-vulkan.patch')]:
    expected = (config / name).read_bytes()
    actual = git(directory, 'diff', '--binary', '--ignore-submodules=all')
    assert actual == expected, f'Sources differ from saved patch: {name}'
    git(directory, 'apply', '--reverse', '--check', str(config / name))
    result['patchSha256'][name] = hashlib.sha256(expected).hexdigest()
print(json.dumps(result))
