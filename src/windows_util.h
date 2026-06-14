#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <sap_core/stl/string.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace sap::fs {

    inline stl::string win_error_string(DWORD err = GetLastError()) {
        LPWSTR buf = nullptr;
        DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
        if (len == 0)
            return "error " + std::to_string(err);
        std::wstring ws(buf, len);
        LocalFree(buf);
        while (!ws.empty() && (ws.back() == L'\n' || ws.back() == L'\r' || ws.back() == L' '))
            ws.pop_back();
        int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 1)
            return "error " + std::to_string(err);
        stl::string s(static_cast<size_t>(sz - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), sz, nullptr, nullptr);
        return s;
    }

    // Prepend \\?\ for paths past MAX_PATH so CreateFileW accepts them.
    // POSIX has no such limit, hence no equivalent in the posix sources.
    inline std::filesystem::path maybe_longpath(const std::filesystem::path& p) {
        const auto& native = p.native();
        if (native.size() > 259 && native.rfind(L"\\\\?\\", 0) != 0)
            return L"\\\\?\\" + native;
        return p;
    }

} // namespace sap::fs
