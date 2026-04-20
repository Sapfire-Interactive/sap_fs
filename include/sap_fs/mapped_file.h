#pragma once

#include <sap_core/stl/result.h>
#include <sap_core/stl/unique_ptr.h>
#include <sap_core/types.h>

#include <filesystem>

namespace sap::fs {

    // Maps a file's bytes directly into the process's virtual address space.
    // Instead of calling read() and copying data into a buffer,
    // you get a span<const byte> pointing straight at the file's pages — the OS handles loading them on demand via page faults.
    class MappedFile {
    public:
        [[nodiscard]] static stl::result<MappedFile> map_absolute(std::filesystem::path absolute_path);
        ~MappedFile();
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;
        MappedFile(MappedFile&&) noexcept;
        MappedFile& operator=(MappedFile&&) noexcept;
        [[nodiscard]] stl::span<const stl::byte> bytes() const;
        [[nodiscard]] u64 size() const;
        // Hint that the kernel should prefetch sequential pages. Best-effort.
        void advise_sequential();
        // Hint that the kernel will not need further pages beyond this offset.
        // Lets the OS drop page cache entries eagerly.
        void advise_dontneed(u64 offset, u64 length);
        friend class Filesystem;

    private:
        MappedFile() = default;
        struct impl;
        stl::unique_ptr<impl> m_impl;
    };

} // namespace sap::fs