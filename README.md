# sap_fs

Cross-platform C++20 filesystem library. Provides a rooted `Filesystem` abstraction and memory-mapped file access via `MappedFile`. Error handling uses `stl::result<T>` from [sap_core](https://github.com/Sapfire-Interactive/sap_core) — no exceptions.

Part of the [Sapfire](https://github.com/Sapfire-Interactive) library ecosystem.

## What's inside

### `Filesystem`
A root-anchored filesystem handle. All paths are relative and validated against the root to prevent path traversal attacks — `validate_path()` rejects anything that escapes the root directory.

Operations: `read`, `read_string`, `write`, `remove`, `exists`, `size`, `mtime`, `set_mtime`, `list`, `list_recursive`, `mkdir`, `absolute`.

All fallible operations return `stl::result<T>` rather than throwing.

### `MappedFile`
Maps a file's bytes directly into the process's virtual address space using `mmap` (POSIX) or `MapViewOfFile` (Windows). You get a `stl::span<const stl::byte>` pointing at the file's pages — the OS loads them on demand via page faults, with no explicit `read()` call and no copy into a user-space buffer.

```cpp
auto r = sap::fs::MappedFile::map_absolute("/path/to/large_file.bin");
if (!r) { /* handle error */ }
auto bytes = r->bytes(); // span<const byte> — zero copy
```

Two cache hints are exposed:
- `advise_sequential()` — hint to the kernel to prefetch pages ahead (`MADV_SEQUENTIAL` / `FILE_FLAG_SEQUENTIAL_SCAN`)
- `advise_dontneed(offset, length)` — hint that pages beyond this offset can be dropped from page cache (`MADV_DONTNEED`)

Platform implementations are split into `*_posix.cpp` and `*_windows.cpp` translation units.

## Build

Requires CMake 3.20+, C++20, and [sap_core](https://github.com/Sapfire-Interactive/sap_core).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Usage example

```cpp
#include <sap_fs/fs.h>
#include <sap_fs/mapped_file.h>

// Rooted filesystem
sap::fs::Filesystem fs{"/var/myapp/data"};

auto content = fs.read_string("config.toml");
if (!content)
    std::println("error: {}", content.error());

// Memory-mapped read — no copy, no read() syscall
auto mapped = sap::fs::MappedFile::map_absolute("/var/myapp/data/assets.bin");
if (mapped) {
    mapped->advise_sequential();
    auto bytes = mapped->bytes(); // span<const byte>
}
```
