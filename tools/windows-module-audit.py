"""Inspect modules in the calling Windows process without starting another app."""
import ctypes
from ctypes import wintypes


def loaded_modules():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.K32EnumProcessModules.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD))
    kernel.K32EnumProcessModules.restype = wintypes.BOOL
    kernel.GetModuleFileNameW.argtypes = (wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
    kernel.GetModuleFileNameW.restype = wintypes.DWORD
    handles, size = (wintypes.HMODULE * 4096)(), wintypes.DWORD()
    if not kernel.K32EnumProcessModules(kernel.GetCurrentProcess(), handles, ctypes.sizeof(handles), ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    if size.value > ctypes.sizeof(handles):
        raise RuntimeError('Module enumeration buffer was too small')
    modules = []
    for handle in handles[:size.value // ctypes.sizeof(wintypes.HMODULE)]:
        name = ctypes.create_unicode_buffer(32768)
        if not kernel.GetModuleFileNameW(handle, name, len(name)):
            raise ctypes.WinError(ctypes.get_last_error())
        modules.append(name.value)
    return sorted(modules, key=str.casefold)
