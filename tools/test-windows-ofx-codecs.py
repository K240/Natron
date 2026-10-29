"""Compare the GLES-free Ultra HDR DLL with the installed codec's CPU API."""
import argparse
import ctypes as c
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

root = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(dll, directory):
    directory.mkdir(parents=True, exist_ok=True)
    handles = [os.add_dll_directory(str(dll.parent)),
               os.add_dll_directory(str(root / 'build/msys64/ucrt64/bin'))]
    api = c.CDLL(str(dll))

    class Error(c.Structure):
        _fields_ = [('code', c.c_int), ('has_detail', c.c_int), ('detail', c.c_char * 256)]

    class Raw(c.Structure):
        _fields_ = [('fmt', c.c_int), ('gamut', c.c_int), ('transfer', c.c_int), ('range', c.c_int),
                    ('width', c.c_uint), ('height', c.c_uint), ('planes', c.c_void_p * 3), ('stride', c.c_uint * 3)]

    class Compressed(c.Structure):
        _fields_ = [('data', c.c_void_p), ('size', c.c_size_t), ('capacity', c.c_size_t),
                    ('gamut', c.c_int), ('transfer', c.c_int), ('range', c.c_int)]

    def function(name, args, result=Error):
        value = getattr(api, name)
        value.argtypes = args
        value.restype = result
        return value

    create_encoder = function('uhdr_create_encoder', [], c.c_void_p)
    release_encoder = function('uhdr_release_encoder', [c.c_void_p], None)
    set_raw = function('uhdr_enc_set_raw_image', [c.c_void_p, c.POINTER(Raw), c.c_int])
    encode = function('uhdr_encode', [c.c_void_p])
    get_encoded = function('uhdr_get_encoded_stream', [c.c_void_p], c.POINTER(Compressed))
    create_decoder = function('uhdr_create_decoder', [], c.c_void_p)
    release_decoder = function('uhdr_release_decoder', [c.c_void_p], None)
    set_image = function('uhdr_dec_set_image', [c.c_void_p, c.POINTER(Compressed)])
    set_format = function('uhdr_dec_set_out_img_format', [c.c_void_p, c.c_int])
    set_transfer = function('uhdr_dec_set_out_color_transfer', [c.c_void_p, c.c_int])
    decode = function('uhdr_decode', [c.c_void_p])
    get_decoded = function('uhdr_get_decoded_image', [c.c_void_p], c.POINTER(Raw))

    def check(error):
        assert error.code == 0, error.detail.decode(errors='replace')

    width, height = 64, 48
    pixels = (c.c_uint32 * (width * height))()
    for y in range(height):
        for x in range(width):
            r, g, b = 128 + x * 12, 192 + y * 14, 96 + ((x + y) % 64) * 13
            pixels[y * width + x] = r | (g << 10) | (b << 20) | (3 << 30)
    cases = []
    for transfer in (1, 2):  # HLG, PQ; BT.2100, full range, RGBA1010102.
        raw = Raw(5, 2, transfer, 1, width, height,
                  (c.c_void_p * 3)(c.cast(pixels, c.c_void_p).value, None, None), (c.c_uint * 3)(width, 0, 0))
        encoder = create_encoder()
        assert encoder
        try:
            check(set_raw(encoder, c.byref(raw), 0))
            check(encode(encoder))
            stream = get_encoded(encoder).contents
            jpeg = c.string_at(stream.data, stream.size)
            assert jpeg[:2] == b'\xff\xd8' and len(jpeg) > 1000
            (directory / f'hdr-{transfer}.jpg').write_bytes(jpeg)
            for fmt, output_transfer, size in ((3, 3, 4), (5, 1, 4), (4, 0, 8)):
                decoder = create_decoder()
                assert decoder
                try:
                    check(set_image(decoder, c.byref(stream)))
                    check(set_format(decoder, fmt))
                    check(set_transfer(decoder, output_transfer))
                    check(decode(decoder))
                    decoded = get_decoded(decoder).contents
                    assert (decoded.width, decoded.height, decoded.fmt) == (width, height, fmt)
                    output = b''.join(c.string_at(decoded.planes[0] + y * decoded.stride[0] * size, width * size)
                                      for y in range(height))
                    if fmt == 4:
                        assert all(math.isfinite(v[0]) for v in struct.iter_unpack('<e', output))
                    filename = f'hdr-{transfer}-format{fmt}.raw'
                    (directory / filename).write_bytes(output)
                    cases.append({'transfer': transfer, 'format': fmt, 'pixels': width * height, 'artifact': filename})
                finally:
                    release_decoder(decoder)
        finally:
            release_encoder(encoder)
    import runpy
    modules = runpy.run_path(str(root / 'tools/windows-module-audit.py'))['loaded_modules']()
    graphics = [m for m in modules if Path(m).name.lower() in ('opengl32.dll', 'libegl.dll', 'libglesv2.dll')]
    report = {'passed': True, 'dllSha256': digest(dll), 'cases': cases, 'graphicsModules': graphics,
              'artifacts': {p.name: digest(p) for p in directory.iterdir() if p.suffix in ('.jpg', '.raw')}}
    (directory / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))


def compare(directory):
    import pefile
    directory.mkdir(parents=True, exist_ok=True)
    old = root / 'build/msys64/ucrt64/bin/libuhdr-1.dll'
    new = root / 'build/ofx-codecs/bin/libuhdr-1.dll'
    reports = {}
    for label, dll in (('reference', old), ('native', new)):
        subprocess.run(['uv', 'run', '--frozen', 'python', str(Path(__file__).resolve()),
                        '--probe', str(dll), '--report-directory', str(directory / label)],
                       cwd=root, check=True, timeout=90, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reports[label] = json.loads((directory / label / 'report.json').read_text())
    reference, native = reports['reference'], reports['native']
    assert native['passed'] and len(native['cases']) == 6 and not native['graphicsModules']
    assert native['artifacts'] == reference['artifacts'], 'CPU encoding/decoding changed'
    with pefile.PE(str(new)) as pe:
        imports = [entry.dll.decode() for entry in pe.DIRECTORY_ENTRY_IMPORT]
        exports = {entry.name for entry in pe.DIRECTORY_ENTRY_EXPORT.symbols if entry.name}
    required = set()
    consumers = []
    for dll in (root / 'build/ofx-runtime').rglob('*.dll'):
        with pefile.PE(str(dll), fast_load=True) as pe:
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
            for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
                if entry.dll.lower() == b'libuhdr-1.dll':
                    consumers.append(str(dll.relative_to(root)))
                    required.update(symbol.name for symbol in entry.imports if symbol.name)
    assert consumers and len(required) >= 10 and not required.difference(exports)
    assert not any('egl' in name.lower() or 'gles' in name.lower() or 'opengl' in name.lower() for name in imports)
    revision = json.loads((root / 'build-configs/windows-ofx/sources.json').read_text())['codecs']['libultrahdr']
    source = root / 'build/ofx-src/libultrahdr'
    assert subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() == revision
    assert not subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain']).strip()
    report = {'passed': True, 'revision': revision, 'cases': 6, 'pixelsCompared': 18432,
              'encodedAndDecodedBytesIdentical': True, 'graphicsModules': native['graphicsModules'],
              'imports': imports, 'requiredExports': len(required), 'consumers': consumers,
              'dllSha256': native['dllSha256'], 'referenceDllSha256': reference['dllSha256'],
              'artifacts': native['artifacts']}
    (directory / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--report-directory', type=Path, default=root / 'build/uhdr-codec-tests')
    args = parser.parse_args()
    if args.probe:
        probe(args.probe.resolve(), args.report_directory.resolve())
    else:
        compare(args.report_directory.resolve())
