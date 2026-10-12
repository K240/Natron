"""Stock Reader native OCIO/premult at the decoder boundary, with CPU references."""
import ctypes
import hashlib
import json
import os
import struct
from pathlib import Path
import NatronEngine
from PySide6.QtCore import QCoreApplication

root = Path.cwd()
output = root / 'build/reader-vulkan-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Reader Vulkan test started', encoding='utf-8')
fixtures = root / 'build/reader-vulkan-fixtures'
assert 'org.natron.test.OcioVulkanSuite' in NatronEngine.natron.getPluginIDs()
abi = ctypes.CDLL(str(root / 'build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx'))
abi.OfxReaderVulkanSelfTest.restype = ctypes.c_char_p
raw_test = abi.OfxReaderVulkanSelfTest().decode()
assert raw_test.startswith('{'), raw_test
algorithm = json.loads(raw_test)
assert algorithm['passed'] and algorithm['cases'] == 216 and algorithm['validationErrors'] == 0
module = ctypes.CDLL(str(root / 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx'))
module.OfxReaderVulkanRenderCount.restype = ctypes.c_uint64
module.OfxReaderVulkanRenderCount.argtypes = [ctypes.c_uint]
module.OfxOCIOVulkanTransferCounts.restype = ctypes.c_int
module.OfxOCIOVulkanTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
class DeviceInfo(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in
               ('vendor_id', 'device_id', 'api_version', 'validation_enabled', 'validation_errors')]
module.OfxOCIOVulkanDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
module.OfxOCIOVulkanDeviceInfo.restype = ctypes.c_int
module.OfxOCIOVulkanClipRenderCount.restype = ctypes.c_uint64
app.addFormat('ReaderVulkanSmoke 43x29 1')
app.getProjectParam('outputFormat').setValue('ReaderVulkanSmoke')
app.getProjectParam('frameRange').set(1, 1)
artifacts, cases = [], []


def transfers():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert module.OfxOCIOVulkanTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
    return uploads.value, downloads.value


def count():
    return sum(module.OfxReaderVulkanRenderCount(level) for level in range(31))


def spaces(node, output_space='linear'):
    node.getParam('ocioConfigFile').setValue((fixtures / 'reader.ocio').as_posix())
    node.getParam('ocioInputSpace').setValue('linear')
    node.getParam('ocioOutputSpace').setValue(output_space)


def pfm(path):
    with path.open('rb') as stream:
        assert stream.readline().strip() == b'PF'
        dimensions = tuple(map(int, stream.readline().split()))
        scale = float(stream.readline())
        data = stream.read()
    assert len(data) == dimensions[0] * dimensions[1] * 12
    return dimensions, struct.unpack(('<' if scale < 0 else '>') + 'f' * (len(data) // 4), data)


variants = [('identity', 2, 2), ('identity-premult', 2, 1), ('identity-associated', 1, 1),
            ('color', 2, 2), ('color-premult', 2, 1), ('color-unpremult', 1, 2), ('color-associated', 1, 1)]
for fixture_name in ('zero', 'negative', 'transparent', 'epsilon', 'negative-alpha'):
    for name, file_premult, output_premult in variants:
        reader = app.createReader((fixtures / f'{fixture_name}.exr').as_posix())
        reader.getParam('outputComponents').setValue('RGBA')
        spaces(reader, 'linear' if name.startswith('identity') else 'graded')
        reader.getParam('filePremult').setValue(file_premult)
        reader.getParam('outputPremult').setValue(output_premult)
        results = []
        for enabled, suffix in ((False, 'cpu'), (True, 'native')):
            reader.getParam('enableGPU').setValue(enabled)
            path = output / f'{fixture_name}-{name}-{suffix}.pfm'
            path.unlink(missing_ok=True)
            writer = app.createNode('fr.inria.openfx.WritePFM')
            assert writer.connectInput(0, reader)
            writer.getParam('filename').setValue(path.as_posix())
            writer.getParam('outputComponents').setValue('RGB')
            spaces(writer)
            QCoreApplication.processEvents()
            before, count_before = transfers(), count()
            app.render(writer, 1, 1)
            assert path.exists(), (fixture_name, name, suffix)
            delta = [after - previous for after, previous in zip(transfers(), before)]
            calls = count() - count_before
            assert calls == int(enabled), (fixture_name, name, suffix, calls)
            assert delta == ([1, 1] if enabled else [0, 0]), (fixture_name, name, suffix, delta)
            results.append(pfm(path))
            artifacts.append(path)
        assert results[0][0] == results[1][0]
        maximum = 0.0
        for index, (actual, expected) in enumerate(zip(results[0][1], results[1][1])):
            error = abs(actual - expected)
            maximum = max(maximum, error)
            assert error <= 2e-5 + 2e-5 * abs(expected), (fixture_name, name, index, actual, expected)
        cases.append({'fixture': fixture_name, 'name': name, 'maximum_error': maximum,
                      'samples_compared': len(results[0][1]), 'native_transfers': [1, 1], 'cpu_transfers': [0, 0]})

reader_name, writer_name = reader.getScriptName(), writer.getScriptName()
roundtrip_reference = path
chain_reader = app.createReader((fixtures / 'zero.exr').as_posix())
chain_reader.getParam('outputComponents').setValue('RGBA')
spaces(chain_reader, 'graded')
chain_reader.getParam('filePremult').setValue(2)
chain_reader.getParam('outputPremult').setValue(2)
chain_nodes = [app.createNode('fr.inria.openfx.OCIOColorSpace') for _ in range(2)]
for index, node in enumerate(chain_nodes):
    spaces(node, 'graded' if index == 0 else 'linear')
    node.getParam('ocioInputSpace').setValue('linear' if index == 0 else 'graded')
    assert node.connectInput(0, chain_nodes[index - 1] if index else chain_reader)
chain_results = []
for enabled, suffix in ((False, 'cpu'), (True, 'native')):
    for node in [chain_reader] + chain_nodes:
        node.getParam('enableGPU').setValue(enabled)
    chain_path = output / f'native-chain-{suffix}.pfm'
    chain_path.unlink(missing_ok=True)
    chain_writer = app.createNode('fr.inria.openfx.WritePFM')
    assert chain_writer.connectInput(0, chain_nodes[-1])
    chain_writer.getParam('filename').setValue(chain_path.as_posix())
    chain_writer.getParam('outputComponents').setValue('RGB')
    spaces(chain_writer)
    before, count_before, clips_before = transfers(), count(), module.OfxOCIOVulkanClipRenderCount()
    app.render(chain_writer, 1, 1)
    assert count() - count_before == int(enabled)
    assert module.OfxOCIOVulkanClipRenderCount() - clips_before == (2 if enabled else 0)
    assert [after - previous for after, previous in zip(transfers(), before)] == ([1, 1] if enabled else [0, 0])
    chain_results.append(pfm(chain_path))
    artifacts.append(chain_path)
assert chain_results[0][0] == chain_results[1][0]
chain_error = max(abs(a-b) for a, b in zip(chain_results[0][1], chain_results[1][1]))
assert all(abs(a-b) <= 3e-5 + 3e-5 * abs(a) for a, b in zip(chain_results[0][1], chain_results[1][1]))
project = output / 'Reader-Vulkan.ntp'
assert app.saveProjectAs(project.as_posix())
loaded = app.loadProject(project.as_posix())
assert loaded is not None
loaded_reader = next(node for node in loaded.getChildren() if node.getScriptName() == reader_name)
loaded_writer = next(node for node in loaded.getChildren() if node.getScriptName() == writer_name)
assert loaded_reader is not None and loaded_writer is not None
assert loaded_reader.getParam('enableGPU').getValue()
loaded_path = output / 'roundtrip-native.pfm'
loaded_path.unlink(missing_ok=True)
loaded_writer.getParam('filename').setValue(loaded_path.as_posix())
loaded_source = output / 'roundtrip-input.exr'
loaded_source.write_bytes((fixtures / 'negative-alpha.exr').read_bytes())
loaded_reader.getParam('filename').setValue(loaded_source.as_posix())
before, count_before = transfers(), count()
loaded.render(loaded_writer, 1, 1)
assert pfm(loaded_path) == pfm(roundtrip_reference)
assert count() - count_before == 1
assert [after - previous for after, previous in zip(transfers(), before)] == [1, 1]
artifacts += [loaded_path, loaded_source]
info = DeviceInfo()
assert module.OfxOCIOVulkanDeviceInfo(ctypes.byref(info)) == 0
assert info.validation_enabled and info.validation_errors == 0
report = {'ok': True, 'cases': cases, 'project_roundtrip': True,
          'algorithm': algorithm,
          'native_chain': {'ocio_nodes': 2, 'native_transfers': [1, 1], 'cpu_transfers': [0, 0],
                           'maximum_error': chain_error, 'samples_compared': len(chain_results[0][1])},
          'device': info.name.decode(), 'validation_errors': info.validation_errors,
          'roundtrip_transfers': [1, 1],
          'native_scale_renders': [module.OfxReaderVulkanRenderCount(level) for level in range(6)],
          'profile': os.environ.get('NATRON_TEST_PROFILE'),
          'artifact_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts + [project]}}
(output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('READER_VULKAN_SMOKE_OK', json.dumps(report))
