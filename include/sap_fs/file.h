#pragma once

#include <sap_core/stl/result.h>
#include <sap_core/stl/unique_ptr.h>
#include <sap_core/types.h>

#include <filesystem>

namespace sap::fs {

    enum class EOpenMode {
        Read, // O_RDONLY / GENERIC_READ
        Write, // O_WRONLY, truncate on open
        Append, // O_WRONLY | O_APPEND, creates if missing
        ReadWrite // O_RDWR
    };

    enum class ECreateMode {
        MustExist, // fail if file doesn't exist
        CreateNew, // fail if file exists
        CreateOrOpen, // create if missing, keep existing content if present
        CreateOrTruncate // create if missing, truncate to empty if present
    };

    enum class ESeekWhence {
        Begin, // SEEK_SET
        Current, // SEEK_CUR
        End // SEEK_END
    };

    struct OpenOptions {
        EOpenMode open_mode = EOpenMode::Read;
        ECreateMode create_mode = ECreateMode::MustExist;
        // Userspace buffer size for writes. 0 = unbuffered (every write() syscalls).
        // Reads are always unbuffered — the caller passes its own buffer.
        u32 write_buffer_bytes = 65536;
    };

    class File {
    public:
        ~File();
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        File(File&&) noexcept;
        File& operator=(File&&) noexcept;
        // Open an absolute path, bypassing Filesystem root sandboxing. Prefer
        // Filesystem::open for sandboxed access.
        [[nodiscard]] static stl::result<File> open_absolute(std::filesystem::path absolute_path, OpenOptions opts);
        // Sequential read from current cursor. Returns bytes read; 0 indicates EOF.
        // A short read is not an error — the caller decides what to do.
        [[nodiscard]] stl::result<size_t> read(stl::span<stl::byte> out);
        // Positioned read. Does not modify the cursor. Safe to call concurrently
        // with other preads on the same File (kernel serialises).
        [[nodiscard]] stl::result<size_t> pread(stl::span<stl::byte> out, u64 offset) const;
        // Sequential write. Writes the full span or returns an error. Partial
        // writes at the OS layer are retried internally.
        [[nodiscard]] stl::result<> write(stl::span<const stl::byte> data);
        // Flush userspace buffer into the OS.
        [[nodiscard]] stl::result<> flush();
        // fsync / FlushFileBuffers. Slow. Use only at durability checkpoints.
        [[nodiscard]] stl::result<> fsync();
        // Cursor introspection and movement.
        [[nodiscard]] stl::result<u64> tell() const;
        [[nodiscard]] stl::result<> seek(i64 offset, ESeekWhence whence);
        // Current file size (stat-based, not cached).
        [[nodiscard]] stl::result<u64> size() const;
        // The path this File was opened against. Stable across moves.
        [[nodiscard]] const std::filesystem::path& path() const { return m_path; }
        // Explicit close. Returns errors from final flush + handle release.
        // The destructor will close implicitly if still open, but if a write
        // buffer flush fails during destruction it calls std::terminate —
        // always call close() explicitly on writable Files to handle errors.
        [[nodiscard]] stl::result<> close();
        [[nodiscard]] bool is_open() const;
        // Friend access so Filesystem::open can construct File via the private ctor.
        friend class Filesystem;

    private:
        File() = default;
        // PIMPL — platform fd / HANDLE lives in the implementation, kept out of the
        // public header.
        struct impl;
        stl::unique_ptr<impl> m_impl;
        std::filesystem::path m_path;
    };
} // namespace sap::fs