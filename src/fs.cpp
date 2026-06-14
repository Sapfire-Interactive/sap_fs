#include "sap_fs/fs.h"
#include <chrono>
#include <fstream>

namespace sap::fs {

    namespace fs = std::filesystem;

    Filesystem::Filesystem(fs::path root) : m_Root(std::move(root)) {}

    stl::result<fs::path> Filesystem::validate_path(std::string_view relative_path) const {
        // Prevent empty paths
        if (relative_path.empty()) {
            return stl::make_error<fs::path>("Empty path");
        }
        // Build absolute path
        fs::path abs_path = m_Root / relative_path;
        // Normalize to resolve .. and .
        abs_path = fs::weakly_canonical(abs_path);
        // Check that the result is still under root
        auto root_str = m_Root.string();
        auto abs_str = abs_path.string();
        if (abs_str.size() < root_str.size() || abs_str.substr(0, root_str.size()) != root_str) {
            return stl::make_error<fs::path>("Path escapes root directory");
        }
        return abs_path;
    }

    bool Filesystem::exists(std::string_view relative_path) const {
        auto path_result = validate_path(relative_path);
        if (!path_result)
            return false;
        return fs::exists(path_result.value());
    }

    stl::result<std::vector<u8>> Filesystem::read(std::string_view relative_path) const {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error<std::vector<u8>>("{}", path_result.error());
        }
        std::ifstream file(path_result.value(), std::ios::binary);
        if (!file) {
            return stl::make_error<std::vector<u8>>("Failed to open file: {}", path_result.value().string());
        }
        size_t size = fs::file_size(path_result.value());
        std::vector<u8> content;
        content.resize(size);
        if (!file.read(reinterpret_cast<char*>(content.data()), size)) {
            return stl::make_error<std::vector<u8>>("Failed to read file");
        }
        return content;
    }

    stl::result<std::string> Filesystem::read_string(std::string_view relative_path) const {
        auto byte_result = read(relative_path);
        if (!byte_result) {
            return stl::make_error<std::string>("{}", byte_result.error());
        }
        auto& bytes = byte_result.value();
        return std::string{bytes.begin(), bytes.end()};
    }

    stl::result<> Filesystem::write(std::string_view relative_path, const std::vector<u8>& content) {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error("{}", path_result.error());
        }
        auto& abs_path = path_result.value();
        // Create parent directories
        if (abs_path.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(abs_path.parent_path(), ec);
            if (ec) {
                return stl::make_error("Failed to create directories: {}", ec.message());
            }
        }
        std::ofstream file{abs_path, std::ios::binary | std::ios::trunc};
        if (!file) {
            return stl::make_error("Failed to open file for writing: {}", abs_path.string());
        }
        if (!file.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()))) {
            return stl::make_error("Failed to write file");
        }
        return stl::success;
    }

    stl::result<> Filesystem::write(std::string_view relative_path, std::string_view content) {
        std::vector<u8> bytes{content.begin(), content.end()};
        return write(relative_path, bytes);
    }

    stl::result<> Filesystem::remove(std::string_view relative_path) {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error("{}", path_result.error());
        }
        std::error_code ec;
        if (!fs::remove(path_result.value(), ec)) {
            if (ec) {
                return stl::make_error("Failed to remove file: {}", ec.message());
            }
            // File didn't exist, that's OK
        }
        return stl::success;
    }

    stl::result<size_t> Filesystem::size(std::string_view relative_path) const {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error<size_t>("{}", path_result.error());
        }
        std::error_code ec;
        auto sz = fs::file_size(path_result.value(), ec);
        if (ec) {
            return stl::make_error<size_t>("Failed to get file size: {}", ec.message());
        }
        return static_cast<size_t>(sz);
    }

    stl::result<Timestamp> Filesystem::mtime(std::string_view relative_path) const {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error<Timestamp>("{}", path_result.error());
        }
        std::error_code ec;
        auto ftime = fs::last_write_time(path_result.value(), ec);
        if (ec) {
            return stl::make_error<Timestamp>("Failed to get mtime: {}", ec.message());
        }
        // Convert to milliseconds since epoch
        auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
            std::chrono::clock_cast<std::chrono::system_clock>(ftime));
        return sctp.time_since_epoch().count();
    }

    stl::result<> Filesystem::set_mtime(std::string_view relative_path, Timestamp time) {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error("{}", path_result.error());
        }
        // Convert from milliseconds to file_time
        auto sys_time = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(time));
        auto file_time = std::chrono::clock_cast<std::chrono::file_clock>(sys_time);
        std::error_code ec;
        fs::last_write_time(path_result.value(), file_time, ec);
        if (ec) {
            return stl::make_error("Failed to set mtime: {}", ec.message());
        }
        return stl::success;
    }

    stl::result<std::vector<std::string>> Filesystem::list(std::string_view relative_dir) const {
        fs::path dir_path;
        if (relative_dir.empty()) {
            dir_path = m_Root;
        } else {
            auto path_result = validate_path(relative_dir);
            if (!path_result) {
                return stl::make_error<std::vector<std::string>>("{}", path_result.error());
            }
            dir_path = path_result.value();
        }
        if (!fs::exists(dir_path)) {
            return std::vector<std::string>{};
        }
        if (!fs::is_directory(dir_path)) {
            return stl::make_error<std::vector<std::string>>("Not a directory");
        }
        std::vector<std::string> entries;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
            if (ec)
                break;
            // Get path relative to root
            auto rel_path = fs::relative(entry.path(), m_Root, ec);
            if (!ec) {
                entries.push_back(rel_path.string());
            }
        }
        if (ec) {
            return stl::make_error<std::vector<std::string>>("Failed to list directory: {}", ec.message());
        }
        return entries;
    }

    stl::result<std::vector<std::string>> Filesystem::list_recursive(std::string_view relative_dir) const {
        fs::path dir_path;
        if (relative_dir.empty()) {
            dir_path = m_Root;
        } else {
            auto path_result = validate_path(relative_dir);
            if (!path_result) {
                return stl::make_error<std::vector<std::string>>("{}", path_result.error());
            }
            dir_path = path_result.value();
        }
        if (!fs::exists(dir_path)) {
            return std::vector<std::string>{};
        }
        if (!fs::is_directory(dir_path)) {
            return stl::make_error<std::vector<std::string>>("Not a directory");
        }
        std::vector<std::string> entries;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(dir_path, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;
            auto rel_path = fs::relative(entry.path(), m_Root, ec);
            if (!ec) {
                entries.push_back(rel_path.string());
            }
        }
        if (ec) {
            return stl::make_error<std::vector<std::string>>("Failed to list directory: {}", ec.message());
        }
        return entries;
    }

    stl::result<> Filesystem::mkdir(std::string_view relative_path) {
        auto path_result = validate_path(relative_path);
        if (!path_result) {
            return stl::make_error("{}", path_result.error());
        }
        std::error_code ec;
        fs::create_directories(path_result.value(), ec);
        if (ec) {
            return stl::make_error("Failed to create directory: {}", ec.message());
        }
        return stl::success;
    }

    fs::path Filesystem::absolute(std::string_view relative_path) const { return m_Root / relative_path; }

} // namespace sap::fs
