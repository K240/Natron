"""Bind the independent fixture's native C ABI, including profile-only faults."""
import ctypes as c
from pathlib import Path

U64, U32, Status = c.c_uint64, c.c_uint32, c.c_int
P64, P32 = c.POINTER(U64), c.POINTER(U32)
Fn = c.CFUNCTYPE


class DeviceInfo(c.Structure):
    _fields_ = [('name', c.c_char * 256)] + [(name, U32) for name in (
        'vendor_id', 'device_id', 'api_version', 'validation_enabled', 'validation_errors')]


class ImageSuite(c.Structure):
    _fields_ = [
        ('createContext', Fn(Status, c.c_int, c.c_int, P64)),
        ('releaseContext', Fn(Status, U64)),
        ('getDeviceInfo', Fn(Status, U64, c.POINTER(DeviceInfo))),
        ('createImage', Fn(Status, U64, U32, U32, P64)),
        ('releaseImage', Fn(Status, U64, U64)),
        ('upload', Fn(Status, U64, U64, c.c_void_p, c.c_int64)),
        ('download', Fn(Status, U64, U64, c.c_void_p, c.c_int64)),
        ('copy', c.c_void_p), ('transform', c.c_void_p),
        ('mix', c.c_void_p), ('restoreChannels', c.c_void_p)]


class ShaderSuite(c.Structure):
    _fields_ = [
        ('createProgram', Fn(Status, U64, c.c_char_p, U32, P64, c.c_void_p, U32)),
        ('releaseProgram', Fn(Status, U64, U64)), ('render', c.c_void_p),
        ('getResources', Fn(Status, U64, U64, P32, P32, P32, U32))]


class TestSuite(c.Structure):
    _fields_ = [('getDeviceGeneration', Fn(Status, U64, P64)),
                ('armDeviceLoss', Fn(Status, U64, U32))]


class LossAbi:
    def __init__(self, root):
        path = Path(root) / 'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx'
        self.module = c.CDLL(str(path))
        for name, kind in [('Image', ImageSuite), ('Shader', ShaderSuite), ('Test', TestSuite)]:
            function = getattr(self.module, 'OfxPressure' + name + 'Suite')
            function.restype = c.POINTER(kind)
            pointer = function()
            assert pointer, name + ' suite unavailable'
            setattr(self, name.lower(), pointer.contents)
        self.module.OfxPressureCounter.argtypes = [c.c_int]
        self.module.OfxPressureCounter.restype = U64
        self.owned = []

    def counters(self):
        return [self.module.OfxPressureCounter(i) for i in range(18)]

    def context(self):
        context = U64()
        assert self.image.createContext(-1, 1, c.byref(context)) == 0 and context.value
        self.owned.append(context.value)
        return context.value

    def generation(self, context):
        value = U64()
        assert self.test.getDeviceGeneration(context, c.byref(value)) == 0
        return value.value

    def retain(self):
        context, image, program = self.context(), U64(), U64()
        assert self.image.createImage(context, 2, 2, c.byref(image)) == 0
        error = c.create_string_buffer(8192)
        source = b'#version 450\nlayout(location=0) out vec4 result;void main(){result=vec4(0,1,0,1);}'
        assert self.shader.createProgram(context, source, 16, c.byref(program), error, len(error)) == 0, error.value
        return context, image.value, program.value, self.generation(context)

    def retired(self, held):
        context, image, program, generation = held
        info, extra, pixels = DeviceInfo(), U64(123), (c.c_float * 16)()
        mask, count = U32(), U32()
        status = [self.image.getDeviceInfo(context, c.byref(info)),
                  self.image.createImage(context, 2, 2, c.byref(extra)),
                  self.image.upload(context, image, pixels, 32),
                  self.image.download(context, image, pixels, 32),
                  self.shader.getResources(context, program, c.byref(mask), c.byref(count), None, 0)]
        assert status == [1001] * len(status) and extra.value == 0, status
        assert self.generation(context) == generation
        assert self.image.releaseImage(context, image) == 0
        assert self.shader.releaseProgram(context, program) == 0
        # Keep the context itself alive to prove replacement during retention.
        return status

    def info(self, context):
        value = DeviceInfo()
        assert self.image.getDeviceInfo(context, c.byref(value)) == 0
        return value

    def close(self):
        for context in reversed(self.owned):
            assert self.image.releaseContext(context) == 0
        self.owned.clear()
