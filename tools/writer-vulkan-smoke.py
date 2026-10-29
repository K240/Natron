"""Compare stock Writer CPU/native render actions at the encoder boundary."""
import ctypes
import hashlib
import json
import os
from pathlib import Path

from PySide6.QtCore import QCoreApplication

root = Path.cwd()
output = root / 'build/writer-vulkan-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Writer Vulkan test started', encoding='utf-8')
fixtures = root / 'build/reader-vulkan-fixtures'
abi = ctypes.CDLL(str(root / 'build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx'))
abi.OfxWriterVulkanSelfTest.restype = ctypes.c_char_p
raw = abi.OfxWriterVulkanSelfTest().decode()
assert raw.startswith('{'), raw
algorithm = json.loads(raw)
assert algorithm['passed'] and algorithm['cases'] == 240 and algorithm['validationErrors'] == 0
module = ctypes.CDLL(str(root / 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx'))
module.OfxWriterVulkanRenderCount.restype = ctypes.c_uint64
module.OfxOCIOVulkanTransferCounts.restype = ctypes.c_int
module.OfxOCIOVulkanTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2


class DeviceInfo(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in
               ('vendor_id', 'device_id', 'api_version', 'validation_enabled', 'validation_errors')]


module.OfxOCIOVulkanDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
module.OfxOCIOVulkanDeviceInfo.restype = ctypes.c_int
app.addFormat('WriterVulkanSmoke 43x29 1')
app.getProjectParam('outputFormat').setValue('WriterVulkanSmoke')
app.getProjectParam('frameRange').set(1, 1)


def transfers():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert module.OfxOCIOVulkanTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
    return uploads.value, downloads.value


def spaces(node, output_space='linear'):
    node.getParam('ocioConfigFile').setValue((fixtures / 'reader.ocio').as_posix())
    node.getParam('ocioInputSpace').setValue('linear')
    node.getParam('ocioOutputSpace').setValue(output_space)


cases, artifacts = [], []
for fixture in (() if os.environ.get('NATRON_TEST_WRITER_CHAIN_ONLY') else ('zero', 'negative', 'transparent', 'epsilon', 'negative-alpha')):
    for plugin, extension, depth in (('WriteEXR', 'exr', 1), ('WriteEXR', 'exr', 0),
                                     ('WritePNG', 'png', None), ('WriteOIIO', 'tif', None), ('WriteOIIO', 'jpg', None)):
        for color in ('linear', 'graded'):
            for premult in (0, 1, 2):
                results = []
                for enabled, suffix in ((False, 'cpu'), (True, 'native')):
                    # Separate Readers prevent earlier renders from hiding uploads in cache.
                    reader = app.createReader((fixtures / f'{fixture}.exr').as_posix())
                    reader.getParam('outputComponents').setValue('RGBA')
                    reader.getParam('enableGPU').setValue(enabled)
                    reader.getParam('filePremult').setValue(2)
                    reader.getParam('outputPremult').setValue(2)
                    spaces(reader)
                    writer = app.createNode('fr.inria.openfx.' + plugin)
                    assert writer.connectInput(0, reader)
                    path = output / f'{fixture}-{plugin}-{depth}-{color}-{premult}-{suffix}.{extension}'
                    path.unlink(missing_ok=True)
                    writer.getParam('filename').setValue(path.as_posix())
                    if depth is not None:
                        writer.getParam('dataType').setValue(depth)
                    elif extension == 'tif':
                        writer.getParam('bitDepth').setValue('32f')
                    writer.getParam('outputComponents').setValue('RGBA')
                    writer.getParam('enableGPU').setValue(enabled)
                    writer.getParam('inputPremult').setValue(premult)
                    spaces(writer, color)
                    QCoreApplication.processEvents()
                    before, calls_before = transfers(), module.OfxWriterVulkanRenderCount()
                    app.render(writer, 1, 1)
                    assert path.exists(), (fixture, plugin, color, premult, suffix)
                    delta = [a - b for a, b in zip(transfers(), before)]
                    calls = module.OfxWriterVulkanRenderCount() - calls_before
                    assert calls == int(enabled), (fixture, plugin, color, premult, suffix, calls)
                    assert delta == ([1, 1] if enabled else [0, 0]), (fixture, plugin, color, premult, suffix, delta)
                    results.append({'path': path.name, 'transfers': delta, 'renders': calls})
                    artifacts.append(path)
                cases.append({'fixture': fixture, 'plugin': plugin, 'extension': extension, 'depth': depth,
                              'color': color, 'premult': premult, 'results': results})
                print('WRITER_CASE_OK', fixture, plugin, color, premult, flush=True)

for reader_gpu, writer_gpu in ((False, True), (True, False)):
    results = []
    for enabled, suffix in ((False, 'cpu'), (True, 'native')):
        reader = app.createReader((fixtures / 'zero.exr').as_posix())
        reader.getParam('outputComponents').setValue('RGBA')
        reader.getParam('filePremult').setValue(2)
        reader.getParam('outputPremult').setValue(2)
        reader.getParam('enableGPU').setValue(enabled and reader_gpu)
        spaces(reader)
        path = output / f'mixed-reader{int(reader_gpu)}-writer{int(writer_gpu)}-{suffix}.exr'
        path.unlink(missing_ok=True)
        writer = app.createNode('fr.inria.openfx.WriteEXR')
        assert writer.connectInput(0, reader)
        writer.getParam('filename').setValue(path.as_posix())
        writer.getParam('outputComponents').setValue('RGBA')
        writer.getParam('inputPremult').setValue(1)
        writer.getParam('enableGPU').setValue(enabled and writer_gpu)
        spaces(writer, 'graded')
        QCoreApplication.processEvents()
        before, calls_before = transfers(), module.OfxWriterVulkanRenderCount()
        app.render(writer, 1, 1)
        delta = [a-b for a,b in zip(transfers(), before)]
        calls = module.OfxWriterVulkanRenderCount()-calls_before
        assert delta == ([1, 1] if enabled else [0, 0]), ('mixed', reader_gpu, writer_gpu, delta)
        assert calls == int(enabled and writer_gpu)
        results.append({'path': path.name, 'transfers': delta, 'renders': calls})
        artifacts.append(path)
    cases.append({'fixture': 'zero', 'plugin': 'MixedBoundary', 'extension': 'exr', 'depth': 1,
                  'color': 'graded', 'premult': 1, 'reader_gpu': reader_gpu, 'writer_gpu': writer_gpu, 'results': results})

chain_results = {'encoded': [], 'original': []}
repeated = []
for enabled, suffix in ((False, 'cpu'), (True, 'native')):
    reader = app.createReader((fixtures / 'zero.exr').as_posix())
    reader.getParam('outputComponents').setValue('RGBA')
    reader.getParam('enableGPU').setValue(enabled)
    reader.getParam('filePremult').setValue(2)
    reader.getParam('outputPremult').setValue(2)
    spaces(reader)
    encoded = output / f'wrapper-encoded-{suffix}.exr'
    original = output / f'wrapper-original-{suffix}.exr'
    encoded.unlink(missing_ok=True)
    original.unlink(missing_ok=True)
    first_writer = app.createWriter(encoded.as_posix())
    assert first_writer.connectInput(0, reader)
    first_writer.getParam('outputComponents').setValue('RGBA')
    first_writer.getParam('bitDepth').setValue('32f')
    first_writer.getParam('inputPremult').setValue(2)
    first_writer.getParam('enableGPU').setValue(enabled)
    spaces(first_writer, 'graded')
    writer = app.createNode('fr.inria.openfx.WriteEXR')
    assert writer.connectInput(0, first_writer)
    writer.getParam('filename').setValue(original.as_posix())
    writer.getParam('outputComponents').setValue('RGBA')
    writer.getParam('inputPremult').setValue(1)
    writer.getParam('enableGPU').setValue(enabled)
    spaces(writer)
    QCoreApplication.processEvents()
    before, calls_before = transfers(), module.OfxWriterVulkanRenderCount()
    app.render(writer, 1, 1)
    delta = [a - b for a, b in zip(transfers(), before)]
    calls = module.OfxWriterVulkanRenderCount() - calls_before
    assert delta == ([1, 2] if enabled else [0, 0]), (suffix, 'wrapper-chain', delta, calls)
    assert calls == (2 if enabled else 0), (suffix, 'wrapper-chain', calls)
    reference_bytes = original.read_bytes()
    encoded.unlink()
    original.unlink()
    before, calls_before = transfers(), module.OfxWriterVulkanRenderCount()
    app.render(writer, 1, 1)
    repeat_delta = [a-b for a,b in zip(transfers(), before)]
    assert encoded.exists() and original.read_bytes() == reference_bytes
    assert module.OfxWriterVulkanRenderCount()-calls_before == (2 if enabled else 0)
    assert repeat_delta[1] == (2 if enabled else 0) and repeat_delta[0] in ((0, 1) if enabled else (0,))
    repeated.append({'enabled': enabled, 'transfers': repeat_delta, 'encoded_again': True})
    for kind, path in (('encoded', encoded), ('original', original)):
        assert path.exists(), path
        chain_results[kind].append({'path': path.name, 'transfers': delta, 'renders': calls})
        artifacts.append(path)
for kind, results in chain_results.items():
    cases.append({'fixture': 'zero', 'plugin': 'WriterWrapper-' + kind, 'extension': 'exr', 'depth': 1,
                  'color': 'graded' if kind == 'encoded' else 'linear', 'premult': 1, 'results': results})

names = [node.getScriptName() for node in (reader, first_writer, writer)]
project = output / 'Writer-Vulkan.ntp'
assert app.saveProjectAs(project.as_posix())
loaded = app.loadProject(project.as_posix())
assert loaded is not None
nodes = {node.getScriptName(): node for node in loaded.getChildren()}
reader, first_writer, writer = [nodes[name] for name in names]
assert all(node.getParam('enableGPU').getValue() for node in (reader, first_writer, writer))
source = output / 'roundtrip-input.exr'
source.write_bytes((fixtures / 'zero.exr').read_bytes())
reader.getParam('filename').setValue(source.as_posix())
original = output / 'roundtrip-original.exr'
writer.getParam('filename').setValue(original.as_posix())
before, calls_before = transfers(), module.OfxWriterVulkanRenderCount()
loaded.render(writer, 1, 1)
assert [a - b for a, b in zip(transfers(), before)] == [1, 2]
assert module.OfxWriterVulkanRenderCount() - calls_before == 2
assert original.read_bytes() == (output / 'wrapper-original-native.exr').read_bytes()
artifacts += [project, source, original]

info = DeviceInfo()
assert module.OfxOCIOVulkanDeviceInfo(ctypes.byref(info)) == 0
assert info.validation_enabled and info.validation_errors == 0
report = {'ok': True, 'cases': cases, 'algorithm': algorithm, 'project_roundtrip': True,
          'repeat_render': repeated,
          'native_chain': {'writers': 2, 'transfers': [1, 2], 'pass_through_original': True}, 'device': info.name.decode(),
          'validation_errors': info.validation_errors, 'profile': os.environ.get('NATRON_TEST_PROFILE'),
          'artifact_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts}}
(output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('WRITER_VULKAN_SMOKE_OK', len(cases), flush=True)
