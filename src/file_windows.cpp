#include "windows_util.h"

#include "sap_fs/file.h"

#include <sap_core/stl/vector.h>

#include <algorithm>
#include <cstring>

namespace sap::fs {

    struct File::impl {
        HANDLE m_handle = INVALID_HANDLE_VALUE;
        stl::vector<stl::byte> m_write_buf;
        u32 m_write_buf_capacity = 0;
        // FILE_FLAG_OVERLAPPED handles don't maintain a file pointer, so we
        // track it ourselves. pread() reads without updating this.
        u64 m_cursor = 0;
        bool m_append = false;

        stl::result<> flush_buffer();
        ~impl();
    };

    // Synchronous positioned read via OVERLAPPED. Does NOT update the cursor.
    // Safe to call concurrently on the same handle (kernel serialises).
    static stl::result<DWORD> overlapped_read(HANDLE handle, void* buf, DWORD count, u64 offset) {
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        BOOL ok = ReadFile(handle, buf, count, nullptr, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                return DWORD(0);
            if (err != ERROR_IO_PENDING)
                return stl::make_error<DWORD>("ReadFile: {}", win_error_string(err));
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(handle, &ov, &transferred, TRUE)) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                return DWORD(0);
            return stl::make_error<DWORD>("ReadFile: {}", win_error_string(err));
        }
        return transferred;
    }

    // Synchronous positioned write via OVERLAPPED.
    static stl::result<DWORD> overlapped_write(HANDLE handle, const void* buf, DWORD count, u64 offset) {
        OVERLAPPED ov = {};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        BOOL ok = WriteFile(handle, buf, count, nullptr, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING)
                return stl::make_error<DWORD>("WriteFile: {}", win_error_string(err));
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(handle, &ov, &transferred, TRUE))
            return stl::make_error<DWORD>("WriteFile: {}", win_error_string());
        return transferred;
    }

    // Atomic append via OVERLAPPED using the MAXDWORD/MAXDWORD sentinel offset.
    // Only valid when the handle was opened with FILE_APPEND_DATA access.
    static stl::result<DWORD> overlapped_append(HANDLE handle, const void* buf, DWORD count) {
        OVERLAPPED ov = {};
        ov.Offset = MAXDWORD;
        ov.OffsetHigh = MAXDWORD;
        BOOL ok = WriteFile(handle, buf, count, nullptr, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING)
                return stl::make_error<DWORD>("WriteFile (append): {}", win_error_string(err));
        }
        DWORD transferred = 0;
        if (!GetOverlappedResult(handle, &ov, &transferred, TRUE))
            return stl::make_error<DWORD>("WriteFile (append): {}", win_error_string());
        return transferred;
    }

    // Drains the write buffer to the OS; shared by flush() and ~impl.
    stl::result<> File::impl::flush_buffer() {
        if (m_write_buf.empty())
            return stl::success;
        auto* ptr = m_write_buf.data();
        size_t remaining = m_write_buf.size();
        while (remaining > 0) {
            DWORD chunk = static_cast<DWORD>(std::min(remaining, static_cast<size_t>(MAXDWORD)));
            stl::result<DWORD> res =
                m_append ? overlapped_append(m_handle, ptr, chunk) : overlapped_write(m_handle, ptr, chunk, m_cursor);
            if (!res)
                return stl::make_error<>("{}", res.error());
            if (!m_append)
                m_cursor += res.value();
            ptr += res.value();
            remaining -= res.value();
        }
        m_write_buf.clear();
        return stl::success;
    }

    File::impl::~impl() {
        if (m_handle == INVALID_HANDLE_VALUE)
            return;
        if (!flush_buffer())
            std::terminate(); // caller didn't call close() on a writable file
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }

    File::~File() = default;

    File::File(File&& other) noexcept : m_impl(std::move(other.m_impl)), m_path(std::move(other.m_path)) { other.m_impl = nullptr; }

    File& File::operator=(File&& other) noexcept {
        m_impl = std::move(other.m_impl);
        m_path = std::move(other.m_path);
        other.m_impl = nullptr;
        return *this;
    }

    stl::result<File> File::open_absolute(std::filesystem::path absolute_path, OpenOptions opts) {
        DWORD desired_access = 0;
        bool append_mode = false;
        switch (opts.open_mode) {
        case EOpenMode::Read:
            desired_access = GENERIC_READ;
            break;
        case EOpenMode::Write:
            desired_access = GENERIC_WRITE;
            break;
        case EOpenMode::Append:
            desired_access = FILE_APPEND_DATA;
            append_mode = true;
            break;
        case EOpenMode::ReadWrite:
            desired_access = GENERIC_READ | GENERIC_WRITE;
            opts.write_buffer_bytes = 0; // buffering + ReadWrite is unsafe; see plan §8.3
            break;
        }

        DWORD creation_disposition = 0;
        switch (opts.create_mode) {
        case ECreateMode::MustExist:
            creation_disposition = OPEN_EXISTING;
            break;
        case ECreateMode::CreateNew:
            creation_disposition = CREATE_NEW;
            break;
        case ECreateMode::CreateOrOpen:
            creation_disposition = OPEN_ALWAYS;
            break;
        case ECreateMode::CreateOrTruncate:
            creation_disposition = CREATE_ALWAYS;
            break;
        }

        // FILE_FLAG_OVERLAPPED is required for positioned reads (pread) to work
        // correctly — without it the OVERLAPPED offset fields are ignored by ReadFile.
        // FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE matches POSIX
        // default-open behaviour where multiple processes can open the same file.
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;

        HANDLE handle =
            CreateFileW(maybe_longpath(absolute_path).c_str(), desired_access, share, nullptr, creation_disposition, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return stl::make_error<File>("open: {}", win_error_string());

        File f;
        f.m_impl = stl::make_unique<File::impl>();
        f.m_impl->m_handle = handle;
        f.m_impl->m_write_buf_capacity = opts.write_buffer_bytes;
        f.m_impl->m_append = append_mode;
        if (opts.write_buffer_bytes > 0)
            f.m_impl->m_write_buf.reserve(opts.write_buffer_bytes);
        f.m_path = std::move(absolute_path);
        return f;
    }

    stl::result<> File::flush() {
        if (!m_impl)
            return stl::make_error<>("File::flush: impl is null");
        if (auto res = m_impl->flush_buffer(); !res)
            return stl::make_error<>("File::flush: {}", res.error());
        return stl::success;
    }

    stl::result<size_t> File::read(stl::span<stl::byte> out) {
        if (!m_impl)
            return stl::make_error<size_t>("File::read: impl is null");
        auto res = overlapped_read(m_impl->m_handle, out.data(), static_cast<DWORD>(out.size()), m_impl->m_cursor);
        if (!res)
            return stl::make_error<size_t>("File::read: {}", res.error());
        m_impl->m_cursor += res.value();
        return static_cast<size_t>(res.value());
    }

    stl::result<size_t> File::pread(stl::span<stl::byte> out, u64 offset) const {
        if (!m_impl)
            return stl::make_error<size_t>("File::pread: impl is null");
        auto res = overlapped_read(m_impl->m_handle, out.data(), static_cast<DWORD>(out.size()), offset);
        if (!res)
            return stl::make_error<size_t>("File::pread: {}", res.error());
        return static_cast<size_t>(res.value()); // cursor NOT updated
    }

    stl::result<> File::write(stl::span<const stl::byte> data) {
        if (!m_impl)
            return stl::make_error<>("File::write: impl is null");
        if (m_impl->m_write_buf_capacity == 0) {
            auto* ptr = data.data();
            size_t remaining = data.size();
            while (remaining > 0) {
                DWORD chunk = static_cast<DWORD>(std::min(remaining, static_cast<size_t>(MAXDWORD)));
                stl::result<DWORD> res = m_impl->m_append ? overlapped_append(m_impl->m_handle, ptr, chunk)
                                                          : overlapped_write(m_impl->m_handle, ptr, chunk, m_impl->m_cursor);
                if (!res)
                    return stl::make_error<>("File::write: {}", res.error());
                if (!m_impl->m_append)
                    m_impl->m_cursor += res.value();
                ptr += res.value();
                remaining -= res.value();
            }
            return stl::success;
        }
        size_t offset = 0;
        while (offset < data.size()) {
            size_t space = m_impl->m_write_buf_capacity - m_impl->m_write_buf.size();
            size_t chunk = std::min(space, data.size() - offset);
            m_impl->m_write_buf.insert(m_impl->m_write_buf.end(), data.data() + offset, data.data() + offset + chunk);
            offset += chunk;
            if (m_impl->m_write_buf.size() == m_impl->m_write_buf_capacity) {
                if (auto res = flush(); !res)
                    return res;
            }
        }
        return stl::success;
    }

    stl::result<> File::fsync() {
        if (!m_impl)
            return stl::make_error<>("File::fsync: impl is null");
        if (auto res = flush(); !res)
            return res;
        if (!FlushFileBuffers(m_impl->m_handle))
            return stl::make_error<>("File::fsync: {}", win_error_string());
        return stl::success;
    }

    stl::result<u64> File::tell() const {
        if (!m_impl)
            return stl::make_error<u64>("File::tell: impl is null");
        return m_impl->m_cursor;
    }

    stl::result<> File::seek(i64 offset, ESeekWhence whence) {
        if (!m_impl)
            return stl::make_error<>("File::seek: impl is null");
        if (auto res = flush(); !res)
            return res;
        switch (whence) {
        case ESeekWhence::Begin:
            m_impl->m_cursor = static_cast<u64>(offset);
            break;
        case ESeekWhence::Current:
            m_impl->m_cursor = static_cast<u64>(static_cast<i64>(m_impl->m_cursor) + offset);
            break;
        case ESeekWhence::End:
            {
                LARGE_INTEGER sz = {};
                if (!GetFileSizeEx(m_impl->m_handle, &sz))
                    return stl::make_error<>("File::seek: {}", win_error_string());
                m_impl->m_cursor = static_cast<u64>(sz.QuadPart + offset);
                break;
            }
        }
        return stl::success;
    }

    stl::result<u64> File::size() const {
        if (!m_impl)
            return stl::make_error<u64>("File::size: impl is null");
        LARGE_INTEGER sz = {};
        if (!GetFileSizeEx(m_impl->m_handle, &sz))
            return stl::make_error<u64>("File::size: {}", win_error_string());
        return static_cast<u64>(sz.QuadPart);
    }

    stl::result<> File::close() {
        if (!m_impl || m_impl->m_handle == INVALID_HANDLE_VALUE)
            return stl::success;
        if (auto res = flush(); !res)
            return res;
        if (!CloseHandle(m_impl->m_handle))
            return stl::make_error<>("File::close: {}", win_error_string());
        m_impl->m_handle = INVALID_HANDLE_VALUE;
        return stl::success;
    }

    bool File::is_open() const { return m_impl && m_impl->m_handle != INVALID_HANDLE_VALUE; }

} // namespace sap::fs