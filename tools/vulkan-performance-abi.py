"""Bind profile-only device telemetry without depending on Natron C++ layout."""
import ctypes as c
import os
from pathlib import Path
import runpy

U64, U32, Status, Fn = c.c_uint64, c.c_uint32, c.c_int, c.CFUNCTYPE


class Statistics(c.Structure):
    _fields_ = [(name, U64) for name in (
        'generation', 'image_uploads', 'image_downloads', 'image_upload_bytes', 'image_download_bytes',
        'managed_local_block_bytes', 'managed_local_peak_block_bytes', 'managed_local_allocation_bytes',
        'driver_local_usage_bytes', 'driver_local_budget_bytes')]
    _fields_ += [('driver_budget_available', U32), ('validation_errors', U32)]


class Telemetry(c.Structure):
    _fields_ = [('getDeviceGeneration', Fn(Status, U64, c.POINTER(U64))),
                ('armDeviceLoss', Fn(Status, U64, U32)),
                ('getStatistics', Fn(Status, U64, c.POINTER(Statistics))),
                ('clearNodeCache', Fn(Status))]


class BenchmarkAbi:
    def __init__(self, root):
        root = Path(root)
        definitions = runpy.run_path(str(root / 'tools/vulkan-device-loss-abi.py'))
        self.loss = definitions['LossAbi'](root)
        context = U64()
        assert self.loss.image.createContext(-1, int(os.environ.get('NATRON_VULKAN_VALIDATION', '0')), c.byref(context)) == 0
        self.context = context.value
        assert self.context
        self.loss.owned.append(self.context)
        path = root / 'build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx'
        self.module = c.CDLL(str(path))
        self.module.OfxBenchmarkTelemetrySuite.restype = c.POINTER(Telemetry)
        pointer = self.module.OfxBenchmarkTelemetrySuite()
        assert pointer, 'Profile-only V2 telemetry unavailable'
        self.telemetry = pointer.contents
        self.module.OfxBenchmarkCounter.argtypes = [c.c_int]
        self.module.OfxBenchmarkCounter.restype = U64

    def snapshot(self):
        statistics = Statistics()
        assert self.telemetry.getStatistics(self.context, c.byref(statistics)) == 0
        return {name: getattr(statistics, name) for name, _ in statistics._fields_}

    def counters(self):
        return [self.module.OfxBenchmarkCounter(i) for i in range(10)]

    def clear(self):
        assert self.telemetry.clearNodeCache() == 0

    def verify_accounting(self):
        # A padded CPU row is repacked by the image suite. The counters measure
        # actual image texels copied, excluding row padding and GPU clears.
        image = U64()
        width, height, stride = 17, 13, 17 * 4 + 7
        pixels = (c.c_float * (stride * height))()
        for y in range(height):
            for x in range(width):
                for channel, value in enumerate((.125, .25, .5, 1.0)):
                    pixels[y * stride + x * 4 + channel] = value
        before = self.snapshot()
        assert self.loss.image.createImage(self.context, width, height, c.byref(image)) == 0
        try:
            assert self.loss.image.upload(self.context, image.value, pixels, stride * 4) == 0
            result = (c.c_float * (width * height * 4))()
            assert self.loss.image.download(self.context, image.value, result, width * 16) == 0
            assert list(result) == [.125, .25, .5, 1.0] * (width * height)
        finally:
            assert self.loss.image.releaseImage(self.context, image.value) == 0
        after = self.snapshot()
        for field, amount in (('image_uploads', 1), ('image_downloads', 1),
                              ('image_upload_bytes', width * height * 16),
                              ('image_download_bytes', width * height * 16)):
            assert after[field] - before[field] == amount, (field, before, after)
        assert after['managed_local_peak_block_bytes'] >= after['managed_local_block_bytes']
        return {'passed': True, 'compared_pixels': width * height,
                'padded_row_floats': stride, 'before': before, 'after': after}

    def close(self):
        self.loss.close()
