"""Map Windows performance-counter LUIDs to DXGI hardware descriptions."""
import ctypes as c
from ctypes import wintypes as w
import json
import time
import uuid


class Luid(c.Structure):
    _fields_ = [('low', w.DWORD), ('high', w.LONG)]


class AdapterDescription(c.Structure):
    _fields_ = [('description', w.WCHAR*128)] + [(name,w.UINT) for name in ('vendorId','deviceId','subSystemId','revision')]
    _fields_ += [(name,c.c_size_t) for name in ('dedicatedVideoMemory','dedicatedSystemMemory','sharedSystemMemory')]
    _fields_ += [('luid',Luid),('flags',w.UINT)]


def adapters():
    api = c.WinDLL('dxgi')
    api.CreateDXGIFactory1.argtypes = [c.c_void_p,c.POINTER(c.c_void_p)]
    api.CreateDXGIFactory1.restype = w.LONG
    iid = c.create_string_buffer(uuid.UUID('770aae78-f26f-4dba-a829-253c83d1b387').bytes_le)
    factory = c.c_void_p()
    status = api.CreateDXGIFactory1(iid,c.byref(factory))
    if status:
        raise OSError(f'CreateDXGIFactory1: 0x{status & 0xffffffff:x}')

    def method(pointer,index,restype,*args):
        table = c.cast(pointer,c.POINTER(c.POINTER(c.c_void_p))).contents
        return c.WINFUNCTYPE(restype,c.c_void_p,*args)(table[index])

    result = []
    try:
        for index in range(32):
            adapter = c.c_void_p()
            status = method(factory,12,w.LONG,w.UINT,c.POINTER(c.c_void_p))(factory,index,c.byref(adapter))
            if status & 0xffffffff == 0x887a0002:
                break
            if status:
                raise OSError(f'EnumAdapters1: 0x{status & 0xffffffff:x}')
            try:
                desc = AdapterDescription()
                status = method(adapter,10,w.LONG,c.POINTER(AdapterDescription))(adapter,c.byref(desc))
                if status:
                    raise OSError(f'GetDesc1: 0x{status & 0xffffffff:x}')
                result.append({'name':desc.description,'vendorId':desc.vendorId,'deviceId':desc.deviceId,
                               'counterInstance':f'luid_0x{desc.luid.high & 0xffffffff:08X}_0x{desc.luid.low:08X}_phys_0',
                               'dedicatedVideoMemory':desc.dedicatedVideoMemory,'sharedSystemMemory':desc.sharedSystemMemory,'flags':desc.flags})
            finally:
                method(adapter,2,w.ULONG)(adapter)
    finally:
        method(factory,2,w.ULONG)(factory)
    return {'recordedUnixNs':time.time_ns(),'adapters':result}


if __name__ == '__main__':
    print(json.dumps(adapters(),indent=2))
