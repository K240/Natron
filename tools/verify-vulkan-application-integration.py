"""Join independently checked processing, recovery and pressure checkpoints."""
import argparse
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--performance-proof', type=Path, required=True)
parser.add_argument('--device-loss-proof', type=Path, required=True)
parser.add_argument('--memory-proof', type=Path, required=True)
parser.add_argument('--viewer-performance-proof', type=Path, help='Optional independently checked real-Viewer seek evidence')
parser.add_argument('--playback-proof', type=Path, help='Optional independently checked Windows scheduler/Viewer evidence')
parser.add_argument('--completion-platform', choices=('windows', 'all'), default='windows',
                    help='Windows is the user-selected completion target; all retains the earlier cross-platform plan')
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-application-integration-proof.json')
args = parser.parse_args()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def checked(proof, key):
    assert proof['passed']
    for path, expected in proof[key].items():
        assert sha(root / path) == expected, f'Changed evidence: {path}'
    for path, expected in proof.get('artifactSha256', {}).items():
        assert sha(root / path) == expected, f'Changed playback artifact: {path}'


performance, loss, memory = [read(path) for path in (args.performance_proof, args.device_loss_proof, args.memory_proof)]
viewer = read(args.viewer_performance_proof) if args.viewer_performance_proof else None
playback = read(args.playback_proof) if args.playback_proof else None
checked(performance, 'inputs')
checked(loss, 'inputs')
checked(memory, 'evidence_sha256')
if viewer:
    checked(viewer, 'inputs')
    assert viewer['applicationCases'] == 64 and viewer['timedSamples'] == 1280
    assert viewer['viewerWsiTested'] and not viewer['continuousPlaybackTested'] and not viewer['fullVulkanMigrationComplete']
    assert viewer['sourceFilesChecked'] == performance['sourceFilesChecked']
    assert viewer['peFilesChecked'] == performance['peFilesChecked']
    assert viewer['userSettingsSha256'] == performance['userSettingsSha256']
if playback:
    checked(playback, 'inputs')
    assert playback['completionPlatform'] == 'windows' and playback['continuousPlaybackTested']
    assert playback['applicationCases'] == 16 and playback['playbackStages'] == 112
    assert playback['sourceFilesChecked'] == performance['sourceFilesChecked']
    assert playback['peFilesChecked'] == performance['peFilesChecked']
    assert playback['userSettingsSha256'] == performance['userSettingsSha256']
assert performance['applicationCases'] == 8 and performance['graphCases'] == 16 and performance['timedSamples'] == 320
assert loss['hostCases'] == 15 and loss['coreLossCases'] == 3 and len(memory['cases']) == 15
assert memory['device_loss_evidence'] == str(args.device_loss_proof)
assert memory['evidence_sha256'][str(args.device_loss_proof)] == sha(args.device_loss_proof)
assert performance['sourceFilesChecked'] == loss['sourceFilesChecked'] == memory['sources_checked']
assert performance['peFilesChecked'] == loss['peFilesChecked'] == memory['runtime_pe_files_checked']
assert performance['userSettingsSha256'] == loss['userSettingsSha256'] == memory['settings_sha256']
# Each proof's source/runtime audit hashes must identify the same build, rather
# than merely matching aggregate file counts. Rehash every live input as well.
audits = []
joined = [(performance, 'inputs'), (loss, 'inputs'), (memory, 'evidence_sha256')]
if viewer:
    joined.append((viewer, 'inputs'))
if playback:
    joined.append((playback, 'inputs'))
for proof, key in joined:
    audit_hashes = {}
    for name, digest in proof[key].items():
        document = read(root / name)
        if 'fileSha256' in document:
            audit_hashes['source'] = digest
            for file, expected in document['fileSha256'].items():
                assert sha(root / file) == expected, file
            assert sha(root / 'build/windows-qt6/build.ninja') == document['buildNinjaSha256']
        elif document.get('scope') == 'Application, shipped OFX and codec PE normal/delay imports':
            audit_hashes['runtime'] = digest
            for record in document['files']:
                assert sha(root / record['file']) == record['sha256'], record['file']
        for field in ('proofHashes', 'inputSha256'):
            for file, expected in document.get(field, {}).items():
                assert sha(root / file) == expected, file
    assert set(audit_hashes) == {'source', 'runtime'}
    audits.append(audit_hashes)
assert all(audit == audits[0] for audit in audits)
assert not performance['fullVulkanMigrationComplete'] and not loss['fullVulkanMigrationComplete'] and not memory['full_vulkan_migration_complete']
result = {'passed': True, 'performanceApplicationCases': 8, 'performanceGraphCases': 16, 'timedSamples': 320,
          'hostRecoveryCases': 15, 'coreRecoveryCases': 3, 'pressureCases': 15,
          'pixelsComparedAcrossRuns': performance['pixelsCompared'] + loss['pixelsCompared'] + memory['pixels_compared'],
          'sourceFilesChecked': performance['sourceFilesChecked'], 'peFilesChecked': performance['peFilesChecked'],
          'userSettingsSha256': performance['userSettingsSha256'], 'fullVulkanMigrationComplete': False,
          'remainingGates': performance['remainingGates'],
          'inputs': {str(path): sha(path) for path in (args.performance_proof, args.device_loss_proof, args.memory_proof)}}
if viewer:
    result['viewerApplicationCases'] = viewer['applicationCases']
    result['viewerTimedSamples'] = viewer['timedSamples']
    result['timedSamples'] += viewer['timedSamples']
    result['pixelsComparedAcrossRuns'] += viewer['pixelsCompared']
    result['viewerWsiTested'] = True
    result['remainingGates'] = viewer['remainingGates']
    result['inputs'][str(args.viewer_performance_proof)] = sha(args.viewer_performance_proof)
result['completionPlatform'] = args.completion_platform
result['excludedPlatforms'] = ['linux', 'macos'] if args.completion_platform == 'windows' else []
result['excludedTests'] = ['parsec']
result['remainingGates'] = [gate.replace('Parsec/physical scanout', 'physical scanout')
                           .replace('/Parsec', '').replace(' and Parsec', '')
                           for gate in result['remainingGates']]
if args.completion_platform == 'windows':
    result['remainingGates'] = [gate.replace('CI/package/Linux/macOS validation', 'Windows CI/package validation')
                                for gate in result['remainingGates']]
if playback:
    result['continuousPlaybackTested'] = True
    result['playbackApplicationCases'] = playback['applicationCases']
    result['playbackStages'] = playback['playbackStages']
    result['pixelsComparedAcrossRuns'] += playback['pixelsCompared']
    result['inputs'][str(args.playback_proof)] = sha(args.playback_proof)
    result['remainingGates'] = [gate.replace('continuous playback, ', '') for gate in result['remainingGates']]
args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
print(json.dumps(result))
