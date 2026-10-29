"""Run identical CPU-source / OCIO / CPU-probe graphs on GL and Vulkan hosts.

Each timed sample uses a previously unrendered frame. Fixture counters prove a
full-size source and OCIO input, and source/probe calls for every sample.
"""
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import time
import traceback
import zlib

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Common API benchmark started', encoding='utf-8')
width = int(os.environ.get('NATRON_BENCHMARK_WIDTH', '3840'))
height = int(os.environ.get('NATRON_BENCHMARK_HEIGHT', '2160'))
samples = int(os.environ.get('NATRON_BENCHMARK_SAMPLES', '20'))
label = os.environ['NATRON_BENCHMARK_API']
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'api': label,
          'sourceSize': [width,height], 'outputSize': [64,48], 'samples': samples, 'cases': [],
          'scope': 'identical full-size CPU source, shipped OCIO Linear-to-sRGB, dispersed CPU PNG probe; unique frames; headless wall time',
          'viewerPlaybackTested': False, 'parsecExcluded': True, 'referenceToleranceBytes':1}
module = ctypes.CDLL(str(Path('build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx').resolve()))
module.OfxBenchmarkCounter.argtypes = [ctypes.c_int]
module.OfxBenchmarkCounter.restype = ctypes.c_uint64
counts = lambda: [module.OfxBenchmarkCounter(i) for i in range(10)]
io = None
native_count = None
direct_count = None
if label == 'vulkan':
    io = ctypes.CDLL(str(Path('build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx').resolve()))
    io.OfxOCIOVulkanClipRenderCount.restype = ctypes.c_uint64
    native_count = io.OfxOCIOVulkanClipRenderCount
    if hasattr(io, 'OfxOCIOVulkanDirectClipRenderCount'):
        io.OfxOCIOVulkanDirectClipRenderCount.restype = ctypes.c_uint64
        direct_count = io.OfxOCIOVulkanDirectClipRenderCount
    class DeviceInfo(ctypes.Structure):
        _fields_ = [('name',ctypes.c_char*256)] + [(name,ctypes.c_uint32) for name in
                      ('vendorId','deviceId','apiVersion','validationEnabled','validationErrors')]
    module.OfxBenchmarkGetDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
    module.OfxBenchmarkGetDeviceInfo.restype = ctypes.c_int
    info = DeviceInfo()
    assert module.OfxBenchmarkGetDeviceInfo(ctypes.byref(info)) == 0
    report['device'] = {name: getattr(info,name) for name,_ in info._fields_ if name != 'name'}
    report['device']['name'] = info.name.decode('utf-8')


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values)-1)*fraction
    lo = math.floor(index)
    return values[lo] + (values[math.ceil(index)]-values[lo])*(index-lo)


def verify(path, frame):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, encoded, header = 8, bytearray(), None
    while offset < len(data):
        length = struct.unpack_from('>I',data,offset)[0]
        kind = data[offset+4:offset+8]
        chunk = data[offset+8:offset+8+length]
        assert zlib.crc32(kind+chunk) & 0xffffffff == struct.unpack_from('>I',data,offset+8+length)[0]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB',chunk)
        if kind == b'IDAT':
            encoded.extend(chunk)
        offset += length+12
    assert header[:3] == (64,48,8) and header[3] in (2,6) and header[4:] == (0,0,0), header
    channels = 3 if header[3] == 2 else 4
    pixels, previous, raw = bytearray(), bytearray(64*channels), zlib.decompress(encoded)
    assert len(raw) == 48*(64*channels+1)
    for row in range(48):
        start = row*(64*channels+1)
        filtering, values = raw[start], bytearray(raw[start+1:start+1+64*channels])
        assert filtering in range(5)
        for i in range(len(values)):
            a = values[i-channels] if i >= channels else 0
            b, d = previous[i], previous[i-channels] if i >= channels else 0
            prediction = a+b-d
            distances = (abs(prediction-a),abs(prediction-b),abs(prediction-d))
            paeth = a if distances[0] <= min(distances[1:]) else b if distances[1] <= distances[2] else d
            values[i] = (values[i] + (0,a,b,(a+b)//2,paeth)[filtering]) & 255
        for x in range(64):
            pixels.extend(values[x*channels:x*channels+3])
            pixels.append(values[x*channels+3] if channels == 4 else 255)
        previous = values
    actual, maximum = pixels, 0
    for y in range(48):
        sy = (47-y)*(height-1)//47
        for x in range(64):
            sx = x*(width-1)//63
            linear = (((sx+frame)&63)/128, ((sy+3*frame)&63)/128, ((sx+sy+7*frame)&63)/128)
            values = tuple(12.92*v if v <= .0031308 else 1.055*v**(1/2.4)-.055 for v in linear) + (1,)
            for channel,value in enumerate(values):
                maximum = max(maximum,abs(actual[(y*64+x)*4+channel]-round(value*255)))
    return maximum


try:
    assert (width,height) in ((3840,2160),(7680,4320)) and 5 <= samples <= 40
    app.addFormat('CommonProbe 64x48 1')
    app.getProjectParam('outputFormat').setValue('CommonProbe')
    app.getProjectParam('frameRange').set(1,2*(samples+3))
    app.getProjectParam('gpuRendering').setValue(0)
    for mode in ('cpu','gpu'):
        source = app.createNode('org.natron.test.VulkanBenchmarkCpuSource')
        probe = app.createNode('org.natron.test.VulkanBenchmarkCpuProbe')
        grade = app.createNode('fr.inria.openfx.OCIOColorSpace')
        assert source and probe and grade
        for node in (source,probe):
            node.getParam('width').setValue(width)
            node.getParam('height').setValue(height)
        assert grade.connectInput(0,source) and probe.connectInput(0,grade)
        grade.getParam('premult').setValue(False)
        grade.getParam('enableGPU').setValue(mode == 'gpu')
        if param := grade.getParam('enableGPURendering'):
            param.setValue(0 if mode == 'gpu' else 1)
        grade.getParam('ocioInputSpace').setValue('Linear')
        grade.getParam('ocioOutputSpace').setValue('sRGB')
        writer = app.createWriter((output.parent/f'{mode}-####.png').as_posix())
        assert writer and writer.connectInput(0,probe)
        if param := writer.getParam('enableGPU'):
            param.setValue(mode == 'gpu')
        if param := writer.getParam('enableGPURendering'):
            param.setValue(0 if mode == 'gpu' else 1)
        for name in ('ocioInputSpace','ocioOutputSpace'):
            if param := writer.getParam(name):
                param.setValue('Linear')
        case = {'mode':mode,'measurements':[],'maximumByteError':0,
                'gradeHostGpuKnob':grade.getParam('enableGPURendering').getValue() if grade.getParam('enableGPURendering') else None,
                'projectGpuMode':app.getProjectParam('gpuRendering').getValue()}
        report['cases'].append(case)
        case['startedUnixNs'] = time.time_ns()
        for frame in range(1,samples+4):
            before = counts()
            native_before = native_count() if native_count else None
            direct_before = direct_count() if direct_count else None
            start = time.perf_counter()
            app.render(writer,frame,frame)
            elapsed = (time.perf_counter()-start)*1000
            after = counts()
            native_delta = native_count()-native_before if native_count else None
            direct_delta = direct_count()-direct_before if direct_count else None
            if direct_count:
                assert (direct_delta > 0) == (mode == 'gpu'), 'Direct OCIO rendering was not exercised'
            if native_count:
                assert native_delta > 0 if mode == 'gpu' else native_delta == 0, 'The selected OCIO API was not exercised'
            assert after[1] > before[1] and after[3] > before[3], 'A timed frame reused cached input'
            assert after[6:10] == [width,height,width,height], after
            png = output.parent/f'{mode}-{frame:04}.png'
            error = verify(png,frame)
            if label != 'opengl' or mode == 'cpu':
                assert error <= 1, (png.name,error)
            case['maximumByteError'] = max(case['maximumByteError'],error)
            if frame > 3:
                case['measurements'].append({'frame':frame,'milliseconds':elapsed,'sourceCallbacks':after[1]-before[1],
                                             'probeCallbacks':after[3]-before[3], 'nativeCallbacks':native_delta, 'directCallbacks':direct_delta, 'png':png.name,
                                             'sha256':hashlib.sha256(png.read_bytes()).hexdigest()})
        values = [row['milliseconds'] for row in case['measurements']]
        case.update(p50Ms=percentile(values,.5),p95Ms=percentile(values,.95),meanMs=statistics.mean(values),stoppedUnixNs=time.time_ns(),
                    referenceWithinTolerance=case['maximumByteError'] <= 1)
        writer.destroy(False)
        probe.destroy(False)
        grade.destroy(False)
        source.destroy(False)
    report.update(ok=True,counters=counts())
except Exception:
    report['error'] = traceback.format_exc()
finally:
    output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    os.write(1,b'NATRON_API_COMPARISON_EXIT_REQUESTED\n')
assert report['ok'],report.get('error')
