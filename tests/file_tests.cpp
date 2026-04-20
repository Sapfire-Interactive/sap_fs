#include <gtest/gtest.h>

#include <sap_fs/file.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace sap::fs;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::vector<std::byte> str_to_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

static std::string bytes_to_str(const std::vector<std::byte>& v) {
    return { reinterpret_cast<const char*>(v.data()), v.size() };
}

static std::vector<std::byte> make_pattern(size_t n, uint8_t val) {
    return std::vector<std::byte>(n, static_cast<std::byte>(val));
}

// Write a file directly via std::ofstream — used to seed existing-file test cases.
static void seed_file(const fs::path& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), content.size());
}

// ASSERT_OK / EXPECT_OK: check result and print the error string on failure.
#define ASSERT_OK(expr) ASSERT_TRUE((expr).has_value())
#define EXPECT_OK(expr) EXPECT_TRUE((expr).has_value())
#define ASSERT_ERR(expr) ASSERT_FALSE((expr).has_value())

// ── fixture ───────────────────────────────────────────────────────────────────

class FileTest : public ::testing::Test {
protected:
    fs::path m_dir;

    void SetUp() override {
        m_dir = fs::temp_directory_path() /
                ("sap_fs_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(m_dir);
    }

    void TearDown() override { fs::remove_all(m_dir); }

    fs::path tmp(std::string_view name) const { return m_dir / name; }
};

// ── open_absolute: error cases ────────────────────────────────────────────────

TEST_F(FileTest, MustExist_MissingFile_ReturnsError) {
    ASSERT_ERR(File::open_absolute(tmp("nope.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    }));
}

TEST_F(FileTest, CreateNew_ExistingFile_ReturnsError) {
    seed_file(tmp("existing.bin"), "hello");
    ASSERT_ERR(File::open_absolute(tmp("existing.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateNew,
    }));
}

TEST_F(FileTest, CreateNew_MissingFile_Succeeds) {
    auto res = File::open_absolute(tmp("new.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateNew,
    });
    ASSERT_OK(res);
    EXPECT_TRUE(res.value().is_open());
}

TEST_F(FileTest, MustExist_ExistingFile_Succeeds) {
    seed_file(tmp("file.bin"), "data");
    auto res = File::open_absolute(tmp("file.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    EXPECT_TRUE(res.value().is_open());
}

// ── is_open / close ───────────────────────────────────────────────────────────

TEST_F(FileTest, IsOpen_TrueAfterOpen_FalseAfterClose) {
    auto res = File::open_absolute(tmp("f.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    auto& f = res.value();
    EXPECT_TRUE(f.is_open());
    ASSERT_OK(f.close());
    EXPECT_FALSE(f.is_open());
}

TEST_F(FileTest, Close_FlushesBufferedWrites) {
    auto wres = File::open_absolute(tmp("close_flush.bin"), {
        .open_mode         = EOpenMode::Write,
        .create_mode       = ECreateMode::CreateOrTruncate,
        .write_buffer_bytes = 65536,
    });
    ASSERT_OK(wres);
    auto& wf = wres.value();
    auto data = str_to_bytes("durable");
    ASSERT_OK(wf.write({ data.data(), data.size() }));
    ASSERT_OK(wf.close());

    // Reopen and verify data actually landed
    auto rres = File::open_absolute(tmp("close_flush.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(rres);
    std::vector<std::byte> buf(7);
    auto n = rres.value().read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 7u);
    EXPECT_EQ(bytes_to_str(buf), "durable");
}

// ── write / read round-trip ───────────────────────────────────────────────────

TEST_F(FileTest, WriteRead_RoundTrip_Buffered) {
    const std::string payload = "hello, sap_fs!";
    {
        auto res = File::open_absolute(tmp("rt.bin"), {
            .open_mode         = EOpenMode::Write,
            .create_mode       = ECreateMode::CreateOrTruncate,
            .write_buffer_bytes = 65536,
        });
        ASSERT_OK(res);
        auto data = str_to_bytes(payload);
        ASSERT_OK(res.value().write({ data.data(), data.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("rt.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(payload.size());
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), payload.size());
        EXPECT_EQ(bytes_to_str(buf), payload);
    }
}

TEST_F(FileTest, WriteRead_RoundTrip_Unbuffered) {
    const std::string payload = "unbuffered path";
    {
        auto res = File::open_absolute(tmp("ub.bin"), {
            .open_mode         = EOpenMode::Write,
            .create_mode       = ECreateMode::CreateOrTruncate,
            .write_buffer_bytes = 0,
        });
        ASSERT_OK(res);
        auto data = str_to_bytes(payload);
        ASSERT_OK(res.value().write({ data.data(), data.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("ub.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(payload.size());
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), payload.size());
        EXPECT_EQ(bytes_to_str(buf), payload);
    }
}

// Write total > buffer capacity to trigger mid-write buffer flushes
TEST_F(FileTest, WriteRead_LargerThanBuffer_MultipleFlushes) {
    const size_t buf_cap = 1024;
    const size_t total   = buf_cap * 5 + 13; // spans 5 full flushes + a partial tail
    auto pattern = make_pattern(total, 0xAB);

    {
        auto res = File::open_absolute(tmp("big.bin"), {
            .open_mode         = EOpenMode::Write,
            .create_mode       = ECreateMode::CreateOrTruncate,
            .write_buffer_bytes = static_cast<u32>(buf_cap),
        });
        ASSERT_OK(res);
        ASSERT_OK(res.value().write({ pattern.data(), pattern.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("big.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(total);
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), total);
        EXPECT_EQ(buf, pattern);
    }
}

// Single write that exactly fills the buffer
TEST_F(FileTest, WriteRead_ExactlyBufferSize) {
    const size_t buf_cap = 512;
    auto pattern = make_pattern(buf_cap, 0x55);

    {
        auto res = File::open_absolute(tmp("exact.bin"), {
            .open_mode         = EOpenMode::Write,
            .create_mode       = ECreateMode::CreateOrTruncate,
            .write_buffer_bytes = static_cast<u32>(buf_cap),
        });
        ASSERT_OK(res);
        ASSERT_OK(res.value().write({ pattern.data(), pattern.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("exact.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(buf_cap);
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), buf_cap);
        EXPECT_EQ(buf, pattern);
    }
}

// ── write: empty span ─────────────────────────────────────────────────────────

TEST_F(FileTest, Write_EmptySpan_IsNoOp) {
    auto res = File::open_absolute(tmp("empty_write.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    ASSERT_OK(res.value().write({}));
    ASSERT_OK(res.value().close());
    EXPECT_EQ(fs::file_size(tmp("empty_write.bin")), 0u);
}

// ── read: EOF ─────────────────────────────────────────────────────────────────

TEST_F(FileTest, Read_AtEOF_ReturnsZero) {
    seed_file(tmp("eof.bin"), "abc");
    auto res = File::open_absolute(tmp("eof.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    // Drain the file
    std::vector<std::byte> buf(64);
    ASSERT_OK(f.read({ buf.data(), buf.size() }));

    // Another read at EOF must return 0
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 0u);
}

// ── append mode ───────────────────────────────────────────────────────────────

TEST_F(FileTest, AppendMode_PreservesExistingContent) {
    seed_file(tmp("append.bin"), "AAAA");

    {
        auto res = File::open_absolute(tmp("append.bin"), {
            .open_mode   = EOpenMode::Append,
            .create_mode = ECreateMode::CreateOrOpen,
        });
        ASSERT_OK(res);
        auto extra = str_to_bytes("BBBB");
        ASSERT_OK(res.value().write({ extra.data(), extra.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("append.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(8);
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), 8u);
        EXPECT_EQ(bytes_to_str(buf), "AAAABBBB");
    }
}

TEST_F(FileTest, AppendMode_CreatesMissingFile) {
    auto res = File::open_absolute(tmp("append_new.bin"), {
        .open_mode   = EOpenMode::Append,
        .create_mode = ECreateMode::CreateOrOpen,
    });
    ASSERT_OK(res);
    EXPECT_TRUE(fs::exists(tmp("append_new.bin")));
}

// ── CreateOrTruncate / CreateOrOpen ──────────────────────────────────────────

TEST_F(FileTest, CreateOrTruncate_TruncatesExistingFile) {
    seed_file(tmp("trunc.bin"), "original content that should vanish");

    {
        auto res = File::open_absolute(tmp("trunc.bin"), {
            .open_mode   = EOpenMode::Write,
            .create_mode = ECreateMode::CreateOrTruncate,
        });
        ASSERT_OK(res);
        auto data = str_to_bytes("short");
        ASSERT_OK(res.value().write({ data.data(), data.size() }));
        ASSERT_OK(res.value().close());
    }
    EXPECT_EQ(fs::file_size(tmp("trunc.bin")), 5u);
}

TEST_F(FileTest, CreateOrOpen_PreservesExistingContent) {
    seed_file(tmp("notrunc.bin"), "keep me");

    auto res = File::open_absolute(tmp("notrunc.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrOpen,
    });
    ASSERT_OK(res);
    // Don't write anything; just close.
    ASSERT_OK(res.value().close());
    EXPECT_EQ(fs::file_size(tmp("notrunc.bin")), 7u);
}

// ── size ─────────────────────────────────────────────────────────────────────

TEST_F(FileTest, Size_MatchesWrittenBytes_AfterFlush) {
    auto res = File::open_absolute(tmp("sz.bin"), {
        .open_mode         = EOpenMode::Write,
        .create_mode       = ECreateMode::CreateOrTruncate,
        .write_buffer_bytes = 65536,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    auto data = str_to_bytes("1234567890");
    ASSERT_OK(f.write({ data.data(), data.size() }));
    ASSERT_OK(f.flush());

    auto sz = f.size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 10u);
}

TEST_F(FileTest, Size_EmptyFile_IsZero) {
    seed_file(tmp("empty.bin"), "");
    auto res = File::open_absolute(tmp("empty.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto sz = res.value().size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 0u);
}

// ── tell / seek ───────────────────────────────────────────────────────────────

TEST_F(FileTest, Tell_AtStart_IsZero) {
    seed_file(tmp("tell.bin"), "ABCDEF");
    auto res = File::open_absolute(tmp("tell.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto pos = res.value().tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 0u);
}

TEST_F(FileTest, Tell_AdvancesAfterRead) {
    seed_file(tmp("tell2.bin"), "ABCDEF");
    auto res = File::open_absolute(tmp("tell2.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    std::vector<std::byte> buf(3);
    ASSERT_OK(f.read({ buf.data(), buf.size() }));
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 3u);
}

TEST_F(FileTest, Seek_Begin_MovesTo_Absolute) {
    seed_file(tmp("seek.bin"), "ABCDEF");
    auto res = File::open_absolute(tmp("seek.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    // Read past the start
    std::vector<std::byte> buf(3);
    ASSERT_OK(f.read({ buf.data(), buf.size() }));

    // Seek back to offset 1 from Begin
    ASSERT_OK(f.seek(1, ESeekWhence::Begin));
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 1u);
}

TEST_F(FileTest, Seek_Current_IsRelative) {
    seed_file(tmp("seekcur.bin"), "ABCDEF");
    auto res = File::open_absolute(tmp("seekcur.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    ASSERT_OK(f.seek(2, ESeekWhence::Begin));
    ASSERT_OK(f.seek(1, ESeekWhence::Current));
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 3u);
}

TEST_F(FileTest, Seek_End_PositionsFromEnd) {
    seed_file(tmp("seekend.bin"), "ABCDEF"); // 6 bytes
    auto res = File::open_absolute(tmp("seekend.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    ASSERT_OK(f.seek(-2, ESeekWhence::End));
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 4u);

    // Read those last 2 bytes
    std::vector<std::byte> buf(2);
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 2u);
    EXPECT_EQ(bytes_to_str(buf), "EF");
}

TEST_F(FileTest, Seek_FlushesWriteBuffer) {
    auto res = File::open_absolute(tmp("seekflush.bin"), {
        .open_mode         = EOpenMode::Write,
        .create_mode       = ECreateMode::CreateOrTruncate,
        .write_buffer_bytes = 65536,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    auto data = str_to_bytes("hello");
    ASSERT_OK(f.write({ data.data(), data.size() }));
    // Seeking should flush the buffer; file size should reflect the write
    ASSERT_OK(f.seek(0, ESeekWhence::Begin));

    auto sz = f.size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 5u);
}

// ── seek then read back correct bytes ────────────────────────────────────────

TEST_F(FileTest, SeekToBegin_ThenRead_CorrectBytes) {
    auto wres = File::open_absolute(tmp("seekread.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(wres);
    auto data = str_to_bytes("ABCDEFGHIJ");
    ASSERT_OK(wres.value().write({ data.data(), data.size() }));
    ASSERT_OK(wres.value().close());

    auto rres = File::open_absolute(tmp("seekread.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(rres);
    auto& f = rres.value();

    // Advance to the middle
    std::vector<std::byte> discard(5);
    ASSERT_OK(f.read({ discard.data(), discard.size() }));

    // Seek back to start
    ASSERT_OK(f.seek(0, ESeekWhence::Begin));

    std::vector<std::byte> buf(10);
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 10u);
    EXPECT_EQ(bytes_to_str(buf), "ABCDEFGHIJ");
}

// ── pread: cursor independence ────────────────────────────────────────────────

TEST_F(FileTest, Pread_DoesNotMoveCursor) {
    seed_file(tmp("pread.bin"), "0123456789");
    auto res = File::open_absolute(tmp("pread.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    // Park the cursor at offset 5
    ASSERT_OK(f.seek(5, ESeekWhence::Begin));

    // Pread from offset 0
    std::vector<std::byte> buf(4);
    auto n = f.pread({ buf.data(), buf.size() }, 0);
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 4u);
    EXPECT_EQ(bytes_to_str(buf), "0123");

    // Cursor must still be at 5
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 5u);
}

TEST_F(FileTest, Pread_ArbitraryOffsets_CorrectData) {
    // Build a file where each 8-byte block starts with a known marker byte
    std::vector<std::byte> content;
    for (uint8_t i = 0; i < 8; ++i) {
        auto block = make_pattern(8, i);
        content.insert(content.end(), block.begin(), block.end());
    }
    {
        auto res = File::open_absolute(tmp("blocks.bin"), {
            .open_mode   = EOpenMode::Write,
            .create_mode = ECreateMode::CreateOrTruncate,
        });
        ASSERT_OK(res);
        ASSERT_OK(res.value().write({ content.data(), content.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("blocks.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        auto& f = res.value();

        // Read each block by pread and verify the marker
        for (uint8_t i = 0; i < 8; ++i) {
            std::vector<std::byte> buf(8);
            auto n = f.pread({ buf.data(), buf.size() }, static_cast<u64>(i) * 8);
            ASSERT_OK(n);
            EXPECT_EQ(n.value(), 8u);
            EXPECT_EQ(buf, make_pattern(8, i)) << "block " << (int)i;
        }
    }
}

// ── pread: concurrent reads ───────────────────────────────────────────────────

TEST_F(FileTest, Pread_ConcurrentThreads_NoCursorDrift) {
    constexpr size_t kBlockSize  = 1024 * 1024; // 1 MiB
    constexpr int    kThreads    = 8;
    constexpr size_t kFileSize   = kBlockSize * kThreads;

    // Write the file: block i is filled with byte value i
    {
        auto res = File::open_absolute(tmp("concurrent.bin"), {
            .open_mode         = EOpenMode::Write,
            .create_mode       = ECreateMode::CreateOrTruncate,
            .write_buffer_bytes = 0,
        });
        ASSERT_OK(res);
        auto& f = res.value();
        for (int i = 0; i < kThreads; ++i) {
            auto block = make_pattern(kBlockSize, static_cast<uint8_t>(i));
            ASSERT_OK(f.write({ block.data(), block.size() }));
        }
        ASSERT_OK(f.close());
    }

    auto fres = File::open_absolute(tmp("concurrent.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(fres);
    auto& f = fres.value();

    // Park cursor at offset 0 before spawning threads
    ASSERT_OK(f.seek(0, ESeekWhence::Begin));

    std::atomic<int> failures{ 0 };
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            std::vector<std::byte> buf(kBlockSize);
            u64 off = static_cast<u64>(i) * kBlockSize;
            auto n = f.pread({ buf.data(), buf.size() }, off);
            if (!n.has_value()) { ++failures; return; }
            if (n.value() != kBlockSize) { ++failures; return; }
            auto expected = make_pattern(kBlockSize, static_cast<uint8_t>(i));
            if (buf != expected) ++failures;
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);

    // Cursor must still be at 0 — pread must not have moved it
    auto pos = f.tell();
    ASSERT_OK(pos);
    EXPECT_EQ(pos.value(), 0u);
}

// ── flush ─────────────────────────────────────────────────────────────────────

TEST_F(FileTest, Flush_MakesDataVisibleOnDisk) {
    auto res = File::open_absolute(tmp("flush.bin"), {
        .open_mode         = EOpenMode::Write,
        .create_mode       = ECreateMode::CreateOrTruncate,
        .write_buffer_bytes = 65536,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    auto data = str_to_bytes("flushed");
    ASSERT_OK(f.write({ data.data(), data.size() }));

    // Before flush: kernel may not see the data yet via stat
    ASSERT_OK(f.flush());
    // After flush: size() via fstat must match
    auto sz = f.size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 7u);
}

TEST_F(FileTest, Flush_EmptyBuffer_IsNoOp) {
    auto res = File::open_absolute(tmp("flush_noop.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    ASSERT_OK(res.value().flush()); // nothing in buffer — must not error
}

// ── fsync ─────────────────────────────────────────────────────────────────────

TEST_F(FileTest, Fsync_FlushesBufferAndSyncs) {
    auto res = File::open_absolute(tmp("fsync.bin"), {
        .open_mode         = EOpenMode::Write,
        .create_mode       = ECreateMode::CreateOrTruncate,
        .write_buffer_bytes = 65536,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    auto data = str_to_bytes("synced content");
    ASSERT_OK(f.write({ data.data(), data.size() }));
    ASSERT_OK(f.fsync());

    auto sz = f.size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 14u);
}

// ── ReadWrite mode ────────────────────────────────────────────────────────────

TEST_F(FileTest, ReadWrite_CanWriteThenReadBack) {
    auto res = File::open_absolute(tmp("rw.bin"), {
        .open_mode   = EOpenMode::ReadWrite,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    auto data = str_to_bytes("READWRITE");
    ASSERT_OK(f.write({ data.data(), data.size() }));
    ASSERT_OK(f.seek(0, ESeekWhence::Begin));

    std::vector<std::byte> buf(9);
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 9u);
    EXPECT_EQ(bytes_to_str(buf), "READWRITE");
}

TEST_F(FileTest, ReadWrite_WriteAtCursor_DoesNotCorruptOtherRegions) {
    seed_file(tmp("rw_partial.bin"), "AAAAAAAAAA"); // 10 As

    auto res = File::open_absolute(tmp("rw_partial.bin"), {
        .open_mode   = EOpenMode::ReadWrite,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    // Overwrite bytes 3..5 with "BBB"
    ASSERT_OK(f.seek(3, ESeekWhence::Begin));
    auto patch = str_to_bytes("BBB");
    ASSERT_OK(f.write({ patch.data(), patch.size() }));
    ASSERT_OK(f.seek(0, ESeekWhence::Begin));

    std::vector<std::byte> buf(10);
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(bytes_to_str(buf), "AAABBBAAAA");
}

// ── multiple sequential writes ────────────────────────────────────────────────

TEST_F(FileTest, MultipleWrites_AccumulateCorrectly) {
    auto res = File::open_absolute(tmp("multi.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    auto& f = res.value();

    for (char c = 'A'; c <= 'E'; ++c) {
        auto byte = str_to_bytes({ &c, 1 });
        ASSERT_OK(f.write({ byte.data(), byte.size() }));
    }
    ASSERT_OK(f.close());

    auto rres = File::open_absolute(tmp("multi.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(rres);
    std::vector<std::byte> buf(5);
    auto n = rres.value().read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(bytes_to_str(buf), "ABCDE");
}

// ── path() is stable across moves ────────────────────────────────────────────

TEST_F(FileTest, Path_StableAcrossMove) {
    auto res = File::open_absolute(tmp("path_test.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(res);
    const fs::path expected = tmp("path_test.bin");
    File moved = std::move(res.value());
    EXPECT_EQ(moved.path(), expected);
}

// ── move semantics ────────────────────────────────────────────────────────────

TEST_F(FileTest, MoveAssignment_TransfersOwnership) {
    auto r1 = File::open_absolute(tmp("move1.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(r1);
    File a = std::move(r1.value());

    auto r2 = File::open_absolute(tmp("move2.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(r2);
    File b = std::move(r2.value());

    a = std::move(b);
    EXPECT_TRUE(a.is_open());
    EXPECT_FALSE(b.is_open());
}

// ── binary data integrity ─────────────────────────────────────────────────────

TEST_F(FileTest, BinaryData_AllByteValues_RoundTrip) {
    std::vector<std::byte> all_bytes(256);
    for (int i = 0; i < 256; ++i)
        all_bytes[i] = static_cast<std::byte>(i);

    {
        auto res = File::open_absolute(tmp("binary.bin"), {
            .open_mode   = EOpenMode::Write,
            .create_mode = ECreateMode::CreateOrTruncate,
        });
        ASSERT_OK(res);
        ASSERT_OK(res.value().write({ all_bytes.data(), all_bytes.size() }));
        ASSERT_OK(res.value().close());
    }
    {
        auto res = File::open_absolute(tmp("binary.bin"), {
            .open_mode   = EOpenMode::Read,
            .create_mode = ECreateMode::MustExist,
        });
        ASSERT_OK(res);
        std::vector<std::byte> buf(256);
        auto n = res.value().read({ buf.data(), buf.size() });
        ASSERT_OK(n);
        EXPECT_EQ(n.value(), 256u);
        EXPECT_EQ(buf, all_bytes);
    }
}

// ── empty file edge case ──────────────────────────────────────────────────────

TEST_F(FileTest, EmptyFile_Size_IsZero_Read_ReturnsZero) {
    auto wres = File::open_absolute(tmp("empty2.bin"), {
        .open_mode   = EOpenMode::Write,
        .create_mode = ECreateMode::CreateOrTruncate,
    });
    ASSERT_OK(wres);
    ASSERT_OK(wres.value().close());

    auto rres = File::open_absolute(tmp("empty2.bin"), {
        .open_mode   = EOpenMode::Read,
        .create_mode = ECreateMode::MustExist,
    });
    ASSERT_OK(rres);
    auto& f = rres.value();

    auto sz = f.size();
    ASSERT_OK(sz);
    EXPECT_EQ(sz.value(), 0u);

    std::vector<std::byte> buf(16);
    auto n = f.read({ buf.data(), buf.size() });
    ASSERT_OK(n);
    EXPECT_EQ(n.value(), 0u);
}