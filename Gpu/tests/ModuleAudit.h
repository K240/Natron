// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_TESTS_MODULEAUDIT_H
#define NATRON_GPU_TESTS_MODULEAUDIT_H
#include <string>
#include <vector>
#include <atomic>
#ifdef _WIN32
#include <Windows.h>
#include <winternl.h>
#endif

// Collect the loader's call stack without allocating or calling into Qt while
// under the Windows loader lock. Resolve module names only after notification.
class ModuleAudit
{
public:
    ModuleAudit()
    {
#ifdef _WIN32
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        auto registration = reinterpret_cast<Register>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        _unregister = reinterpret_cast<Unregister>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));

        if (registration && _unregister) {
            registration(0, callback, this, &_cookie);
        }

#endif
    }
    ~ModuleAudit()
    {
#ifdef _WIN32

        if (_cookie) {
            _unregister(_cookie);
        }

#endif
    }
    std::vector<std::string> openGLLoadStack() const
    {
        std::vector<std::string> names;
#ifdef _WIN32

        // The first frame is this notification callback inside the test binary.
        const auto count = _frameCount.load(std::memory_order_acquire);

        for (USHORT i = 1; i < count; ++i) {
            HMODULE module = nullptr;

            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(_frames[i]), &module)) {
                wchar_t path[MAX_PATH] {};
                GetModuleFileNameW(module, path, MAX_PATH);
                const auto length = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);

                if (length > 0) {
                    std::string name(static_cast<size_t>(length), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, name.data(), length, nullptr, nullptr);
                    name.pop_back();
                    names.push_back(name);
                }
            }
        }

#endif
        return names;
    }
private:
#ifdef _WIN32
    struct Loaded {
        ULONG flags;
        PCUNICODE_STRING fullName;
        PCUNICODE_STRING baseName;
        void *base;
        ULONG size;
    };
    using Notify = void (CALLBACK*)(ULONG, const Loaded*, void*);
    using Register = LONG(NTAPI*)(ULONG, Notify, void*, void**);
    using Unregister = LONG(NTAPI*)(void*);
    static void CALLBACK callback(ULONG reason, const Loaded* data, void* context)
    {
        if (reason == 1 && data->baseName->Length == 12 * sizeof(wchar_t) &&
            _wcsnicmp(data->baseName->Buffer, L"opengl32.dll", 12) == 0) {
            auto* audit = static_cast<ModuleAudit *>(context);

            if (audit->_frameCount.load(std::memory_order_relaxed) == 0) {
                audit->_frameCount.store(CaptureStackBackTrace(0, 64, audit->_frames, nullptr), std::memory_order_release);
            }
        }
    }
    void *_cookie = nullptr;
    Unregister _unregister = nullptr;
    void *_frames[64] {};
    std::atomic<USHORT> _frameCount{0};
#endif
};
#endif
