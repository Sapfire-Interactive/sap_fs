#include "sap_fs/file.h"

#include <sap_core/stl/vector.h>

#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sap::fs {

    struct File::impl {
        int m_fd = -1;
        stl::vector<stl::byte> m_write_buf;
        u32 m_write_buf_capacity = 0;
    };

    File::~File() {
        if (!m_impl || m_impl->m_fd < 0)
            return;
        if (!m_impl->m_write_buf.empty()) {
            auto res = flush();
            if (!res)
                std::terminate(); // caller didn't call close() on a writeable file
        }
        ::close(m_impl->m_fd);
        m_impl->m_fd = -1;
    }

    File::File(File&& other) noexcept : m_impl(std::move(other.m_impl)), m_path(std::move(other.m_path)) { other.m_impl = nullptr; }

    File& File::operator=(File&& other) noexcept {
        m_impl = std::move(other.m_impl);
        m_path = std::move(other.m_path);
        other.m_impl = nullptr;
        return *this;
    }

    stl::result<File> File::open_absolute(std::filesystem::path absolute_path, OpenOptions opts) {
        int flags = 0;
        switch (opts.open_mode) {
        case EOpenMode::Read:
            flags = O_RDONLY;
            break;
        case EOpenMode::Write:
            flags = O_WRONLY;
            break;
        case EOpenMode::Append:
            flags = O_WRONLY | O_APPEND;
            break;
        case EOpenMode::ReadWrite:
            flags = O_RDWR;
            break;
        }
        switch (opts.create_mode) {
        case ECreateMode::MustExist:
            break; // default: fail if missing
        case ECreateMode::CreateNew:
            flags |= O_CREAT | O_EXCL;
            break;
        case ECreateMode::CreateOrOpen:
            flags |= O_CREAT;
            break;
        case ECreateMode::CreateOrTruncate:
            flags |= O_CREAT | O_TRUNC;
            break;
        }
        int fd = ::open(absolute_path.c_str(), flags, 0644);
        if (fd < 0)
            return stl::make_error<File>("open: {}", ::strerror(errno));
        File f;
        f.m_impl = stl::make_unique<File::impl>();
        f.m_impl->m_fd = fd;
        f.m_impl->m_write_buf_capacity = opts.write_buffer_bytes;
        if (opts.write_buffer_bytes > 0)
            f.m_impl->m_write_buf.reserve(opts.write_buffer_bytes);
        f.m_path = std::move(absolute_path);
        return f;
    }

    stl::result<> File::flush() {
        if (!m_impl)
            return stl::make_error<>("File::flush: impl is null");
        if (m_impl->m_write_buf.empty())
            return stl::success;
        auto* ptr = m_impl->m_write_buf.data();
        size_t remaining = m_impl->m_write_buf.size();
        while (remaining > 0) {
            ssize_t n = ::write(m_impl->m_fd, ptr, remaining);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return stl::make_error<>("write: {}", ::strerror(errno));
            }
            ptr += n;
            remaining -= n;
        }
        m_impl->m_write_buf.clear();
        return stl::success;
    }

    stl::result<size_t> File::read(stl::span<stl::byte> out) {
        if (!m_impl)
            return stl::make_error<size_t>("File::read: impl is null");
        ssize_t n;
        do {
            n = ::read(m_impl->m_fd, out.data(), out.size());
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return stl::make_error<size_t>("File::read: {}", ::strerror(errno));
        return static_cast<size_t>(n); // 0 = EOF
    }

    stl::result<size_t> File::pread(stl::span<stl::byte> out, u64 offset) const {
        if (!m_impl)
            return stl::make_error<size_t>("File::pread: impl is null");
        ssize_t n;
        do {
            n = ::pread(m_impl->m_fd, out.data(), out.size(), static_cast<off_t>(offset));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return stl::make_error<size_t>("File::pread: {}", ::strerror(errno));
        return static_cast<size_t>(n); // 0 = EOF
    }

    stl::result<> File::write(stl::span<const stl::byte> data) {
        if (!m_impl)
            return stl::make_error<>("File::write: impl is null");
        // unbuffered path, just write directly and retry on EINTR
        if (m_impl->m_write_buf_capacity == 0) {
            auto* ptr = data.data();
            size_t remaining = data.size();
            while (remaining > 0) {
                ssize_t n = ::write(m_impl->m_fd, ptr, remaining);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    return stl::make_error<>("File::write: {}", ::strerror(errno));
                }
                ptr += n;
                remaining -= n;
            }
            return stl::success;
        }
        // buffered path - fill buffer, flush when full
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
        int n = ::fsync(m_impl->m_fd);
        if (n < 0)
            return stl::make_error<>("File::fsync: {}", ::strerror(errno));
        return stl::success;
    }

    stl::result<u64> File::tell() const {
        if (!m_impl)
            return stl::make_error<u64>("File::tell: impl is null");
        off_t pos = ::lseek(m_impl->m_fd, 0, SEEK_CUR);
        if (pos < 0)
            return stl::make_error<u64>("File::tell: {}", ::strerror(errno));
        return static_cast<u64>(pos);
    }

    stl::result<> File::seek(i64 offset, ESeekWhence whence) {
        if (!m_impl)
            return stl::make_error<>("File::seek: impl is null");
        if (auto res = flush(); !res)
            return res;
        int w = (whence == ESeekWhence::Begin) ? SEEK_SET : (whence == ESeekWhence::Current) ? SEEK_CUR : SEEK_END;
        if (::lseek(m_impl->m_fd, offset, w) < 0)
            return stl::make_error("lseek: {}", ::strerror(errno));
        return {};
    }

    stl::result<u64> File::size() const {
        if (!m_impl)
            return stl::make_error<u64>("File::size impl is null");
        struct stat st;
        if (::fstat(m_impl->m_fd, &st) < 0)
            return stl::make_error<u64>("File::size: {}", ::strerror(errno));
        return static_cast<u64>(st.st_size);
    }

    stl::result<> File::close() {
        if (auto res = flush(); !res)
            return res;
        if (::close(m_impl->m_fd) < 0)
            return stl::make_error<>("File::close: {}", ::strerror(errno));
        m_impl->m_fd = -1;
        return stl::success;
    }

    bool File::is_open() const { return m_impl && m_impl->m_fd >= 0; }

} // namespace sap::fs