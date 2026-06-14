#include "windows_util.h"

#include "sap_fs/mapped_file.h"

#include <sap_core/stl/vector.h>

#include <cstring>

namespace sap::fs {

    struct MappedFile::impl {
        HANDLE m_file = INVALID_HANDLE_VALUE;
        HANDLE m_mapping = nullptr;
        void* m_view = nullptr;
        u64 m_size = 0;

        ~impl() {
            if (m_view)
                UnmapViewOfFile(m_view);
            if (m_mapping)
                CloseHandle(m_mapping);
            if (m_file != INVALID_HANDLE_VALUE)
                CloseHandle(m_file);
        }
    };

    MappedFile::MappedFile(MappedFile&& other) noexcept : m_impl(std::move(other.m_impl)) { other.m_impl = nullptr; }

    MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
        m_impl = std::move(other.m_impl);
        other.m_impl = nullptr;
        return *this;
    }

    stl::result<MappedFile> MappedFile::map_absolute(std::filesystem::path absolute_path) {
        HANDLE file = CreateFileW(maybe_longpath(absolute_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return stl::make_error<MappedFile>("map open: {}", win_error_string());

        LARGE_INTEGER sz = {};
        GetFileSizeEx(file, &sz);

        MappedFile mf;
        mf.m_impl = stl::make_unique<MappedFile::impl>();
        mf.m_impl->m_file = file;
        mf.m_impl->m_size = static_cast<u64>(sz.QuadPart);

        if (sz.QuadPart > 0) {
            HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!mapping) {
                CloseHandle(file);
                return stl::make_error<MappedFile>("CreateFileMapping: {}", win_error_string());
            }
            void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (!view) {
                CloseHandle(mapping);
                CloseHandle(file);
                return stl::make_error<MappedFile>("MapViewOfFile: {}", win_error_string());
            }
            mf.m_impl->m_mapping = mapping;
            mf.m_impl->m_view = view;
        }
        return mf;
    }

    MappedFile::~MappedFile() = default;

    u64 MappedFile::size() const { return m_impl ? m_impl->m_size : 0; }

    stl::span<const stl::byte> MappedFile::bytes() const {
        if (!m_impl || !m_impl->m_view)
            return {};
        return {static_cast<const stl::byte*>(m_impl->m_view), m_impl->m_size};
    }

    void MappedFile::advise_sequential() {}

    void MappedFile::advise_dontneed(u64 /*offset*/, u64 /*length*/) {
        // TODO: can map to PrefetchVirtualMemory
    }
} // namespace sap::fs