"""Read saved images independently through OIIO and compare every channel."""
import argparse
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--directory', type=Path, default=Path('build/writer-vulkan-smoke'))
args = parser.parse_args()
directory = args.directory.resolve()
report = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
tool = Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve()
converted = directory / 'comparison'
converted.mkdir(exist_ok=True)


def read(path, channel):
    destination = converted / (path.stem + '-' + channel + '.pfm')
    subprocess.run([str(tool), str(path), '--ch', channel, '-d', 'float', '-o', str(destination)],
                   check=True, capture_output=True)
    with destination.open('rb') as stream:
        kind = stream.readline().strip()
        dimensions = tuple(map(int, stream.readline().split()))
        scale = float(stream.readline())
        pixels = stream.read()
    assert kind in (b'PF', b'Pf'), kind
    channels = 3 if kind == b'PF' else 1
    assert len(pixels) == math.prod(dimensions) * channels * 4
    return dimensions, struct.unpack(('<' if scale < 0 else '>') + 'f' * (len(pixels) // 4), pixels)


samples, maximum, source_cases = 0, 0.0, 0
for case in report['cases']:
    paths = [directory / item['path'] for item in case['results']]
    for path in paths:
        assert hashlib.sha256(path.read_bytes()).hexdigest() == report['artifact_sha256'][path.name]
    case_maximum = 0.0
    for channels in (('R,G,B',) if paths[0].suffix == '.jpg' else ('R,G,B', 'A')):
        reference, actual = [read(path, channels) for path in paths]
        assert reference[0] == actual[0], (case, channels)
        for index, (expected, value) in enumerate(zip(reference[1], actual[1])):
            assert math.isfinite(expected) and math.isfinite(value), (case, channels, index)
            error = abs(expected - value)
            case_maximum = max(case_maximum, error)
            if paths[0].suffix == '.jpg':
                tolerance = 3 / 255 + 1e-7
            elif paths[0].suffix == '.png':
                tolerance = 1 / 255 + 1e-7
            elif case['depth'] == 0:
                tolerance = .001 + .001 * abs(expected)
            else:
                tolerance = 3e-5 + 3e-5 * abs(expected)
            assert error <= tolerance, (case, channels, index, expected, value, error)
        # Independently check identity EXR against the decoded source. A broken
        # CPU and GPU encoder must not pass merely because they agree.
        if case['plugin'] in ('WriteEXR', 'WriterWrapper-original') and case['color'] == 'linear' and case['fixture'] != 'negative':
            source = read(Path('build/reader-vulkan-fixtures') / (case['fixture'] + '.exr'), channels)
            source_alpha = read(Path('build/reader-vulkan-fixtures') / (case['fixture'] + '.exr'), 'A')
            assert source[0] == actual[0] == (43, 29)
            for index, (value, original) in enumerate(zip(actual[1], source[1])):
                a = source_alpha[1][index // 3 if channels == 'R,G,B' else index]
                expected = original
                if case['premult'] == 2:
                    expected = original * max(a, 0) if channels == 'R,G,B' else max(a, 0)
                if case['depth'] == 0:
                    expected = struct.unpack('<e', struct.pack('<e', expected))[0]
                assert abs(expected - value) <= 2e-6, (case, channels, index, expected, value)
            source_cases += 1
        samples += len(reference[1])
    maximum = max(maximum, case_maximum)
    case['maximum_error'] = case_maximum
report['comparison'] = {'passed': True, 'samples': samples, 'maximum_error': maximum,
                        'cases': len(report['cases']), 'source_channel_cases': source_cases}
(directory / 'comparison.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report['comparison']))
