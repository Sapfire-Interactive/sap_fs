#include "sap_fs/mapped_file.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sap::fs {

    struct MappedFile::impl {
        void* m_addr = nullptr;
        size_t m_size = 0;
    };

    MappedFile::MappedFile(MappedFile&& other) noexcept : m_impl(std::move(other.m_impl)) { other.m_impl = nullptr; }

    MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
        m_impl = std::move(other.m_impl);
        other.m_impl = nullptr;
        return *this;
    }

    stl::result<MappedFile> MappedFile::map_absolute(std::filesystem::path absolute_path) {
        int fd = ::open(absolute_path.c_str(), O_RDONLY);
        if (fd < 0)
            return stl::make_error<MappedFile>("mmap open: {}", ::strerror(errno));

        struct stat st;
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            return stl::make_error<MappedFile>("mmap fstat: {}", ::strerror(errno));
        }
        MappedFile mf;
        mf.m_impl = stl::make_unique<MappedFile::impl>();
        mf.m_impl->m_size = static_cast<size_t>(st.st_size);
        if (st.st_size > 0) {
            void* addr = ::mmap(nullptr, mf.m_impl->m_size, PROT_READ, MAP_PRIVATE, fd, 0);
            int mmap_errno = errno; // save before close() can clobber it
            ::close(fd);
            if (addr == MAP_FAILED)
                return stl::make_error<MappedFile>("mmap: {}", ::strerror(mmap_errno));
            mf.m_impl->m_addr = addr;
        } else {
            ::close(fd); // empty file: valid MappedFile with empty span
        }
        return mf;
    }

    MappedFile::~MappedFile() {
        if (m_impl && m_impl->m_addr)
            ::munmap(m_impl->m_addr, m_impl->m_size);
    }

    stl::span<const stl::byte> MappedFile::bytes() const {
        if (!m_impl || !m_impl->m_addr)
            return {};
        return {static_cast<const stl::byte*>(m_impl->m_addr), m_impl->m_size};
    }

    u64 MappedFile::size() const { return m_impl ? m_impl->m_size : 0; }

    void MappedFile::advise_sequential() {
        if (m_impl && m_impl->m_addr)
            ::madvise(m_impl->m_addr, m_impl->m_size, MADV_SEQUENTIAL);
    }

    void MappedFile::advise_dontneed(u64 offset, u64 length) {
        if (m_impl && m_impl->m_addr)
            ::madvise(static_cast<char*>(m_impl->m_addr) + offset, length, MADV_DONTNEED);
    }
} // namespace sap::fs