#include <gtest/gtest.h>

#include <sap_fs/mapped_file.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace sap::fs;

// ── helpers ───────────────────────────────────────────────────────────────────

static void seed_file(const fs::path& p, const void* data, size_t len) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
}

static void seed_file(const fs::path& p, std::string_view s) {
    seed_file(p, s.data(), s.size());
}

static std::vector<std::byte> make_pattern(size_t n, uint8_t val) {
    return std::vector<std::byte>(n, static_cast<std::byte>(val));
}

#define ASSERT_OK(expr) ASSERT_TRUE((expr).has_value())
#define ASSERT_ERR(expr) ASSERT_FALSE((expr).has_value())

// ── fixture ───────────────────────────────────────────────────────────────────

class MappedFileTest : public ::testing::Test {
protected:
    fs::path m_dir;

    void SetUp() override {
        m_dir = fs::temp_directory_path() /
                ("sap_fs_mmap_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(m_dir);
    }

    void TearDown() override { fs::remove_all(m_dir); }

    fs::path tmp(std::string_view name) const { return m_dir / name; }
};

// ── open errors ───────────────────────────────────────────────────────────────

TEST_F(MappedFileTest, MapAbsolute_MissingFile_ReturnsError) {
    ASSERT_ERR(MappedFile::map_absolute(tmp("does_not_exist.bin")));
}

TEST_F(MappedFileTest, MapAbsolute_Directory_ReturnsError) {
    // mmap on a directory fd fails with EACCES / EISDIR
    ASSERT_ERR(MappedFile::map_absolute(m_dir));
}

// ── empty file ────────────────────────────────────────────────────────────────

TEST_F(MappedFileTest, EmptyFile_Size_IsZero) {
    seed_file(tmp("empty.bin"), "");
    auto res = MappedFile::map_absolute(tmp("empty.bin"));
    ASSERT_OK(res);
    EXPECT_EQ(res.value().size(), 0u);
}

TEST_F(MappedFileTest, EmptyFile_Bytes_IsEmptySpan) {
    seed_file(tmp("empty.bin"), "");
    auto res = MappedFile::map_absolute(tmp("empty.bin"));
    ASSERT_OK(res);
    EXPECT_TRUE(res.value().bytes().empty());
    EXPECT_EQ(res.value().bytes().data(), nullptr);
}

TEST_F(MappedFileTest, EmptyFile_DoesNotCrash) {
    seed_file(tmp("empty.bin"), "");
    auto res = MappedFile::map_absolute(tmp("empty.bin"));
    ASSERT_OK(res);
    // exercises destructor path on a file that was never mapped
    auto mf = std::move(res.value());
    (void)mf;
}

// ── content correctness ───────────────────────────────────────────────────────

TEST_F(MappedFileTest, Bytes_MatchFileContent) {
    const std::string content = "hello, mapped world!";
    seed_file(tmp("content.bin"), content);

    auto res = MappedFile::map_absolute(tmp("content.bin"));
    ASSERT_OK(res);
    auto& mf = res.value();

    ASSERT_EQ(mf.size(), content.size());
    EXPECT_EQ(0, std::memcmp(mf.bytes().data(), content.data(), content.size()));
}

TEST_F(MappedFileTest, Size_MatchesFilesystemSize) {
    const std::string content = "size check";
    seed_file(tmp("sz.bin"), content);

    auto res = MappedFile::map_absolute(tmp("sz.bin"));
    ASSERT_OK(res);
    EXPECT_EQ(res.value().size(), fs::file_size(tmp("sz.bin")));
}

TEST_F(MappedFileTest, BinaryData_AllByteValues_RoundTrip) {
    std::vector<uint8_t> all_bytes(256);
    for (int i = 0; i < 256; ++i)
        all_bytes[i] = static_cast<uint8_t>(i);
    seed_file(tmp("binary.bin"), all_bytes.data(), all_bytes.size());

    auto res = MappedFile::map_absolute(tmp("binary.bin"));
    ASSERT_OK(res);
    auto& mf = res.value();

    ASSERT_EQ(mf.size(), 256u);
    for (int i = 0; i < 256; ++i)
        EXPECT_EQ(static_cast<uint8_t>(mf.bytes()[i]), static_cast<uint8_t>(i)) << "at index " << i;
}

TEST_F(MappedFileTest, RandomAccess_SubspanAtVariousOffsets) {
    // Build a file where each 16-byte block is filled with the block index
    constexpr size_t kBlocks    = 16;
    constexpr size_t kBlockSize = 16;
    std::vector<std::byte> content(kBlocks * kBlockSize);
    for (size_t i = 0; i < kBlocks; ++i)
        std::fill_n(content.data() + i * kBlockSize, kBlockSize, static_cast<std::byte>(i));
    seed_file(tmp("blocks.bin"), content.data(), content.size());

    auto res = MappedFile::map_absolute(tmp("blocks.bin"));
    ASSERT_OK(res);
    auto bytes = res.value().bytes();

    for (size_t i = 0; i < kBlocks; ++i) {
        auto block = bytes.subspan(i * kBlockSize, kBlockSize);
        auto expected = make_pattern(kBlockSize, static_cast<uint8_t>(i));
        EXPECT_EQ(0, std::memcmp(block.data(), expected.data(), kBlockSize)) << "block " << i;
    }
}

// ── large file ────────────────────────────────────────────────────────────────

TEST_F(MappedFileTest, LargeFile_SpotCheckBytes) {
    constexpr size_t kSize = 4 * 1024 * 1024; // 4 MiB
    std::vector<uint8_t> data(kSize);
    for (size_t i = 0; i < kSize; ++i)
        data[i] = static_cast<uint8_t>(i & 0xFF);
    seed_file(tmp("large.bin"), data.data(), data.size());

    auto res = MappedFile::map_absolute(tmp("large.bin"));
    ASSERT_OK(res);
    auto& mf = res.value();

    ASSERT_EQ(mf.size(), kSize);
    // spot-check at several offsets rather than comparing the entire buffer
    for (size_t off : { size_t(0), size_t(1024), size_t(65536), size_t(1024*1024), kSize - 1 }) {
        EXPECT_EQ(static_cast<uint8_t>(mf.bytes()[off]), static_cast<uint8_t>(off & 0xFF))
            << "at offset " << off;
    }
}

// ── concurrency ───────────────────────────────────────────────────────────────

TEST_F(MappedFileTest, ConcurrentReads_MultipleThreads_CorrectData) {
    constexpr int    kThreads   = 8;
    constexpr size_t kBlockSize = 512 * 1024; // 512 KiB per thread
    constexpr size_t kFileSize  = kThreads * kBlockSize;

    std::vector<std::byte> content(kFileSize);
    for (int i = 0; i < kThreads; ++i)
        std::fill_n(content.data() + i * kBlockSize, kBlockSize, static_cast<std::byte>(i));
    seed_file(tmp("concurrent.bin"), content.data(), content.size());

    auto res = MappedFile::map_absolute(tmp("concurrent.bin"));
    ASSERT_OK(res);
    auto& mf = res.value();

    std::atomic<int> failures{ 0 };
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            size_t off = static_cast<size_t>(i) * kBlockSize;
            auto block = mf.bytes().subspan(off, kBlockSize);
            auto expected = make_pattern(kBlockSize, static_cast<uint8_t>(i));
            if (std::memcmp(block.data(), expected.data(), kBlockSize) != 0)
                ++failures;
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

// ── move semantics ────────────────────────────────────────────────────────────

TEST_F(MappedFileTest, MoveCtor_SourceBecomesEmpty) {
    seed_file(tmp("move.bin"), "move me");

    auto res = MappedFile::map_absolute(tmp("move.bin"));
    ASSERT_OK(res);

    MappedFile a = std::move(res.value());
    EXPECT_EQ(a.size(), 7u);

    MappedFile b = std::move(a);
    EXPECT_EQ(b.size(), 7u);
    EXPECT_EQ(a.size(), 0u);        // source emptied
    EXPECT_TRUE(a.bytes().empty()); // source span is gone
}

TEST_F(MappedFileTest, MoveAssignment_TransfersOwnership) {
    seed_file(tmp("ma.bin"), "assigned");
    seed_file(tmp("mb.bin"), "overwritten");

    auto ra = MappedFile::map_absolute(tmp("ma.bin"));
    auto rb = MappedFile::map_absolute(tmp("mb.bin"));
    ASSERT_OK(ra);
    ASSERT_OK(rb);

    MappedFile a = std::move(ra.value());
    MappedFile b = std::move(rb.value());
    b = std::move(a);

    EXPECT_EQ(b.size(), 8u); // "assigned" is 8 bytes
    EXPECT_EQ(a.size(), 0u); // source emptied
}

// ── advise (best-effort, must not crash) ──────────────────────────────────────

TEST_F(MappedFileTest, AdviseSequential_DoesNotCrash) {
    seed_file(tmp("advseq.bin"), "some content for sequential hint");
    auto res = MappedFile::map_absolute(tmp("advseq.bin"));
    ASSERT_OK(res);
    res.value().advise_sequential(); // best-effort, no return value to check
}

TEST_F(MappedFileTest, AdviseDontneed_DoesNotCrash) {
    seed_file(tmp("advdn.bin"), "dontneed content");
    auto res = MappedFile::map_absolute(tmp("advdn.bin"));
    ASSERT_OK(res);
    res.value().advise_dontneed(0, res.value().size());
}

TEST_F(MappedFileTest, AdviseSequential_EmptyFile_DoesNotCrash) {
    seed_file(tmp("empty_seq.bin"), "");
    auto res = MappedFile::map_absolute(tmp("empty_seq.bin"));
    ASSERT_OK(res);
    res.value().advise_sequential();
}

TEST_F(MappedFileTest, AdviseDontneed_EmptyFile_DoesNotCrash) {
    seed_file(tmp("empty_dn.bin"), "");
    auto res = MappedFile::map_absolute(tmp("empty_dn.bin"));
    ASSERT_OK(res);
    res.value().advise_dontneed(0, 0);
}

// ── bytes() / size() consistency ─────────────────────────────────────────────

TEST_F(MappedFileTest, Bytes_Size_AreConsistent) {
    seed_file(tmp("consist.bin"), "consistency");
    auto res = MappedFile::map_absolute(tmp("consist.bin"));
    ASSERT_OK(res);
    auto& mf = res.value();
    EXPECT_EQ(mf.bytes().size(), mf.size());
}

TEST_F(MappedFileTest, Bytes_NonNull_ForNonEmptyFile) {
    seed_file(tmp("notnull.bin"), "x");
    auto res = MappedFile::map_absolute(tmp("notnull.bin"));
    ASSERT_OK(res);
    EXPECT_NE(res.value().bytes().data(), nullptr);
}

// ── mapping is read-only view (does not modify file) ─────────────────────────

TEST_F(MappedFileTest, Destructor_DoesNotModifyFile) {
    const std::string content = "untouched";
    seed_file(tmp("intact.bin"), content);

    {
        auto res = MappedFile::map_absolute(tmp("intact.bin"));
        ASSERT_OK(res);
        // MappedFile goes out of scope here — destructor must not truncate or modify
    }

    // Re-read the file via ifstream and verify content is intact
    std::ifstream f(tmp("intact.bin"), std::ios::binary);
    std::string actual(std::istreambuf_iterator<char>(f), {});
    EXPECT_EQ(actual, content);
}