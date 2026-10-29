"""Verify preserved pre-optimization and current Windows renderer measurements."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before-matrix', type=Path, required=True)
parser.add_argument('--before-snapshot', type=Path, required=True)
parser.add_argument('--after-matrix', type=Path, required=True)
parser.add_argument('--adapters', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
proofs = {}
inputs = {}


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


for name, matrix in (('before', args.before_matrix), ('after', args.after_matrix)):
    output = args.output.with_name(args.output.stem + '-' + name + '.json')
    command = ['uv', 'run', '--frozen', 'python', str(ROOT/'tools/verify-renderer-api-comparison.py'),
               '--matrix', str(matrix), '--adapters', str(args.adapters), '--output', str(output)]
    if name == 'before':
        command += ['--input-snapshot', str(args.before_snapshot)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{name} verification failed:\n{result.stdout}\n{result.stderr}')
    proofs[name] = json.loads(output.read_text(encoding='utf-8'))
    for path in (matrix, output):
        inputs[str(path.resolve())] = sha(path)

before, after = proofs['before'], proofs['after']
assert before['passed'] and after['passed']
assert before['timedSamples'] == after['timedSamples'] == 240
assert before['pixelsCompared'] == after['pixelsCompared'] == 737280
assert before['scope'] == after['scope'] and before['dependencyDifference'] == after['dependencyDifference']
assert before['userSettingsSha256'] == after['userSettingsSha256']
improvements = []
for backend in ('vulkan-0', 'vulkan-1'):
    for resolution in ('4K', '8K'):
        for mode in ('cpu', 'gpu'):
            old = next(r for r in before['rows'] if (r['backend'], r['resolution'], r['mode']) == (backend, resolution, mode))
            new = next(r for r in after['rows'] if (r['backend'], r['resolution'], r['mode']) == (backend, resolution, mode))
            assert old['device'] == new['device'] and old['referenceWithinTolerance'] and new['referenceWithinTolerance']
            improvements.append({'backend': backend, 'device': new['device'], 'resolution': resolution, 'mode': mode,
                                 'beforeP50Ms': old['p50Ms'], 'afterP50Ms': new['p50Ms'],
                                 'p50ReductionPercent': 100*(1-new['p50Ms']/old['p50Ms']),
                                 'beforeP95Ms': old['p95Ms'], 'afterP95Ms': new['p95Ms'],
                                 'beforeMaximumByteError': old['maximumByteError'],
                                 'afterMaximumByteError': new['maximumByteError']})

assert all(r['afterP50Ms'] < r['beforeP50Ms'] for r in improvements if r['mode'] == 'gpu')
inputs[str(args.before_snapshot.resolve())] = sha(args.before_snapshot)
inputs[str(args.adapters.resolve())] = sha(args.adapters)
inputs[str(Path(__file__).resolve())] = sha(Path(__file__))
report = {'passed': True, 'completionPlatform': 'windows', 'fullVulkanMigrationComplete': False,
          'scope': after['scope'], 'gpuMedianImprovedOnBothDevicesAndResolutions': True,
          'measurementsAtDifferentTimes': True, 'sameSessionRandomizedPairedBenchmark': False,
          'cpuAndSystemSchedulingCanVary': True, 'timedSamplesVerified': 480, 'pixelsCompared': 1474560,
          'improvements': improvements, 'currentRtxGlComparisons': after['rtxComparisons'],
          'legacyAccuracyFailures': after['legacyAccuracyFailures'], 'legacyDependencyDifference': after['dependencyDifference'],
          'memoryScope': after['memoryScope'], 'parsecExcluded': True,
          'userSettingsSha256': after['userSettingsSha256'], 'inputs': inputs}
args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k: report[k] for k in ('passed', 'gpuMedianImprovedOnBothDevicesAndResolutions', 'timedSamplesVerified',
                                       'pixelsCompared', 'improvements')}))
