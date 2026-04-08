#pragma once

#include <sap_core/stl/result.h>
#include <sap_core/timestamp.h>
#include <sap_core/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace sap::fs {

    class Filesystem {
    public:
        explicit Filesystem(std::filesystem::path root);
        // Get the root directory
        [[nodiscard]] const std::filesystem::path& root() const { return m_Root; }
        // Check if a file exists
        [[nodiscard]] bool exists(std::string_view relative_path) const;
        // Read file content
        [[nodiscard]] stl::result<std::vector<u8>> read(std::string_view relative_path) const;
        // Read file as string
        [[nodiscard]] stl::result<std::string> read_string(std::string_view relative_path) const;
        // Write file content (creates parent directories if needed)
        [[nodiscard]] stl::result<> write(std::string_view relative_path, const std::vector<u8>& content);
        [[nodiscard]] stl::result<> write(std::string_view relative_path, std::string_view content);
        // Delete a file
        [[nodiscard]] stl::result<> remove(std::string_view relative_path);
        // Get file size
        [[nodiscard]] stl::result<size_t> size(std::string_view relative_path) const;
        // Get file modification time (ms since epoch)
        [[nodiscard]] stl::result<Timestamp> mtime(std::string_view relative_path) const;
        // Set file modification time
        [[nodiscard]] stl::result<> set_mtime(std::string_view relative_path, Timestamp time);
        // List files in directory (non-recursive)
        [[nodiscard]] stl::result<std::vector<std::string>> list(std::string_view relative_dir = "") const;
        // List all files recursively
        [[nodiscard]] stl::result<std::vector<std::string>> list_recursive(std::string_view relative_dir = "") const;
        // Create directory (and parents)
        [[nodiscard]] stl::result<> mkdir(std::string_view relative_path);
        // Get absolute path for a relative path
        [[nodiscard]] std::filesystem::path absolute(std::string_view relative_path) const;

    private:
        std::filesystem::path m_Root;
        // Validate path doesn't escape root (prevent path traversal attacks)
        [[nodiscard]] stl::result<std::filesystem::path> validate_path(std::string_view relative_path) const;
    };

} // namespace sap::fs
