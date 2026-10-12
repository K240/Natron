// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_ENVIRONMENT_H
#define NATRON_OFX_ENVIRONMENT_H
#include <cstdlib>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace NatronOFX
{
// OFX file properties and OCIO filenames use UTF-8, while Windows' narrow
// CRT getenv loses characters outside the active code page.
inline std::string environmentUtf8(const char* name)
{
#ifdef _WIN32
    const int nameLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);

    if (!nameLength) {
        return {};
    }

    std::vector<wchar_t> wideName(nameLength);

    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wideName.data(), nameLength)) {
        return {};
    }

    DWORD size = GetEnvironmentVariableW(wideName.data(), nullptr, 0);

    while (size) {
        std::vector<wchar_t> value(size);
        const DWORD copied = GetEnvironmentVariableW(wideName.data(), value.data(), size);

        if (!copied) {
            return {};
        }

        if (copied < size) {
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied, nullptr, 0, nullptr, nullptr);

            if (!bytes) {
                return {};
            }

            std::string utf8(bytes, '\0');

            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied, utf8.data(), bytes, nullptr, nullptr)) {
                return {};
            }

            return utf8;
        }

        size = copied;
    }

    return {};
#else
    const char *value = std::getenv(name);
    return value ? value : "";
#endif
}
}
#endif
