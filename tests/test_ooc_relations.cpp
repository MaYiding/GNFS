// test_ooc_relations.cpp — Out-of-core relation storage correctness tests
//
// Tests write→read round-trip, random access, and compatibility with
// in-memory relation pipeline.

#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/util/temp_path.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using gnfs::core::Relation;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;
using gnfs::util::OwnedNativeFile;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n";                        \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_PASS(name)                                                                            \
    do {                                                                                           \
        std::cout << "  PASS: " << name << "\n";                                                   \
        tests_passed++;                                                                            \
    } while (0)

/// Create a test relation with known values
static Relation make_relation(int64_t a, uint64_t b, size_t n_rf, size_t n_af, size_t n_rlp = 0,
                              size_t n_alp = 0) {
    Relation rel;
    rel.a = a;
    rel.b = b;
    for (size_t i = 0; i < n_rf; ++i)
        rel.rational_factors.push_back(static_cast<uint32_t>(100 + i));
    for (size_t i = 0; i < n_af; ++i)
        rel.algebraic_factors.push_back(static_cast<uint32_t>(200 + i));
    for (size_t i = 0; i < n_rlp; ++i)
        rel.rational_large_prime.push_back({1000 + i, 0, 1});
    for (size_t i = 0; i < n_alp; ++i)
        rel.algebraic_large_prime.push_back({2000 + i, 500 + i, 1});
    return rel;
}

/// Compare two relations for equality
static bool relations_equal(const Relation& a, const Relation& b) {
    if (a.a != b.a || a.b != b.b)
        return false;
    if (a.rational_factors != b.rational_factors)
        return false;
    if (a.algebraic_factors != b.algebraic_factors)
        return false;
    if (a.rational_large_prime.size() != b.rational_large_prime.size())
        return false;
    for (size_t i = 0; i < a.rational_large_prime.size(); ++i) {
        if (a.rational_large_prime[i].p != b.rational_large_prime[i].p)
            return false;
        if (a.rational_large_prime[i].r != b.rational_large_prime[i].r)
            return false;
        if (a.rational_large_prime[i].e != b.rational_large_prime[i].e)
            return false;
    }
    if (a.algebraic_large_prime.size() != b.algebraic_large_prime.size())
        return false;
    for (size_t i = 0; i < a.algebraic_large_prime.size(); ++i) {
        if (a.algebraic_large_prime[i].p != b.algebraic_large_prime[i].p)
            return false;
        if (a.algebraic_large_prime[i].r != b.algebraic_large_prime[i].r)
            return false;
        if (a.algebraic_large_prime[i].e != b.algebraic_large_prime[i].e)
            return false;
    }
    if (a.extra_ab_pairs != b.extra_ab_pairs)
        return false;
    return true;
}

/// RAII temp file cleanup
struct TempFiles {
    std::string base;
    TempFiles(const std::string& b) : base(b) {}
    ~TempFiles() {
        std::remove((base + ".reldata").c_str());
        std::remove((base + ".relidx").c_str());
    }
};

using TestNativeHandle = OwnedNativeFile::NativeHandle;

static TestNativeHandle open_native_read_only(const std::string& path) {
#ifdef _WIN32
    const std::filesystem::path filesystem_path(path);
    HANDLE handle =
        ::CreateFileW(filesystem_path.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot open test file handle");
    }
    return handle;
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open test file descriptor");
    }
    return descriptor;
#endif
}

static TestNativeHandle open_native_write_only(const std::string& path) {
#ifdef _WIN32
    const std::filesystem::path filesystem_path(path);
    HANDLE handle = ::CreateFileW(filesystem_path.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot open write-only test file handle");
    }
    return handle;
#else
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_WRONLY);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open write-only test file descriptor");
    }
    return descriptor;
#endif
}

static bool native_handle_is_open(TestNativeHandle handle) {
#ifdef _WIN32
    DWORD flags = 0;
    return ::GetHandleInformation(handle, &flags) != 0;
#else
    int result = -1;
    do {
        result = ::fcntl(handle, F_GETFD);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
#endif
}

static bool native_handle_is_closed(TestNativeHandle handle) {
#ifdef _WIN32
    DWORD flags = 0;
    ::SetLastError(ERROR_SUCCESS);
    return ::GetHandleInformation(handle, &flags) == 0 && ::GetLastError() == ERROR_INVALID_HANDLE;
#else
    errno = 0;
    return ::fcntl(handle, F_GETFD) == -1 && errno == EBADF;
#endif
}

#ifndef _WIN32
static int find_fd_for_path(const std::string& path) {
    struct stat expected {};
    if (::stat(path.c_str(), &expected) != 0) {
        return -1;
    }
    for (int descriptor = 0; descriptor < 1024; ++descriptor) {
        struct stat observed {};
        int result = -1;
        do {
            result = ::fstat(descriptor, &observed);
        } while (result != 0 && errno == EINTR);
        if (result == 0 && observed.st_dev == expected.st_dev &&
            observed.st_ino == expected.st_ino) {
            return descriptor;
        }
    }
    return -1;
}

static bool descriptor_has_cloexec(int descriptor) {
    int flags = -1;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}
#endif

static void resize_file(const std::string& path, std::uintmax_t size) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    if (error) {
        throw std::system_error(error, "cannot resize test file");
    }
}

static void replace_file(const std::string& source, const std::string& destination) {
#ifdef _WIN32
    const std::filesystem::path source_path(source);
    const std::filesystem::path destination_path(destination);
    if (::ReplaceFileW(destination_path.c_str(), source_path.c_str(), nullptr, 0, nullptr,
                       nullptr) == 0) {
        const DWORD error = ::GetLastError();
        throw std::system_error(std::error_code(static_cast<int>(error), std::system_category()),
                                "cannot replace test file");
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw std::system_error(error, "cannot replace test file");
    }
#endif
}

static OOCSnapshotDescriptor write_finalized_pair(const std::string& base_path,
                                                  const std::vector<Relation>& relations) {
    OOCRelationWriter writer(base_path);
    for (const auto& relation : relations) {
        writer.write(relation);
    }
    return writer.finalize();
}

// ============================================================================
// Tests
// ============================================================================

void test_write_read_single() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_single"));

    Relation orig = make_relation(42, 7, 5, 3, 1, 2);
    orig.extra_ab_pairs.push_back({10, 20});

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(orig);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    TEST_ASSERT(reader.count() == 1, "should have 1 relation");

    auto loaded = reader.read(0);
    TEST_ASSERT(relations_equal(orig, loaded), "loaded relation should match original");

    TEST_PASS("write/read single relation");
}

void test_write_read_batch() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_batch"));

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 1000; ++i) {
            auto rel =
                make_relation(static_cast<int64_t>(i * 3 - 500), static_cast<uint64_t>(i + 1),
                              static_cast<size_t>(3 + i % 5), static_cast<size_t>(2 + i % 4),
                              static_cast<size_t>(i % 3), static_cast<size_t>(i % 2));
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    TEST_ASSERT(reader.count() == 1000, "should have 1000 relations");

    // Verify all relations round-trip correctly
    for (size_t i = 0; i < 1000; ++i) {
        auto loaded = reader.read(i);
        TEST_ASSERT(relations_equal(originals[i], loaded),
                    "relation " + std::to_string(i) + " should match");
    }

    TEST_PASS("write/read 1000 relations batch");
}

void test_random_access() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_random"));

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 500; ++i) {
            auto rel = make_relation(i, static_cast<uint64_t>((i + 1) * 7), 4, 3, 1, 1);
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);

    // Access in random order
    std::mt19937 rng(12345);
    for (int trial = 0; trial < 200; ++trial) {
        size_t idx = rng() % 500;
        auto loaded = reader.read(idx);
        TEST_ASSERT(relations_equal(originals[idx], loaded),
                    "random access idx=" + std::to_string(idx));
    }

    TEST_PASS("random access 200 reads from 500 relations");
}

void test_read_all() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_readall"));

    std::vector<Relation> originals;
    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 100; ++i) {
            auto rel = make_relation(i, static_cast<uint64_t>(i + 1), 3, 2);
            originals.push_back(rel);
            writer.write(rel);
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto all = reader.read_all();
    TEST_ASSERT(all.size() == 100, "read_all should return 100");

    for (size_t i = 0; i < 100; ++i) {
        TEST_ASSERT(relations_equal(originals[i], all[i]),
                    "read_all[" + std::to_string(i) + "] mismatch");
    }

    TEST_PASS("read_all() round-trip 100 relations");
}

void test_read_range() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_range"));

    {
        OOCRelationWriter writer(tmp.base);
        for (int i = 0; i < 50; ++i) {
            writer.write(make_relation(i, static_cast<uint64_t>(i + 1), 2, 2));
        }
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto range = reader.read_range(10, 20);
    TEST_ASSERT(range.size() == 10, "range [10,20) should have 10");
    TEST_ASSERT(range[0].a == 10, "first in range should be a=10");
    TEST_ASSERT(range[9].a == 19, "last in range should be a=19");

    TEST_PASS("read_range [10,20) from 50 relations");
}

void test_empty_relations() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_empty"));

    // Relation with no factors, no LPs, no extras
    Relation empty_rel;
    empty_rel.a = 1;
    empty_rel.b = 2;

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(empty_rel);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto loaded = reader.read(0);
    TEST_ASSERT(loaded.a == 1 && loaded.b == 2, "core fields match");
    TEST_ASSERT(loaded.rational_factors.empty(), "no rational factors");
    TEST_ASSERT(loaded.algebraic_factors.empty(), "no algebraic factors");
    TEST_ASSERT(loaded.rational_large_prime.empty(), "no rational LPs");
    TEST_ASSERT(loaded.algebraic_large_prime.empty(), "no algebraic LPs");
    TEST_ASSERT(loaded.extra_ab_pairs.empty(), "no extra pairs");

    TEST_PASS("empty relation (no factors/LPs/extras)");
}

void test_merged_relation_with_extras() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_merged"));

    Relation merged = make_relation(100, 200, 10, 8, 2, 3);
    merged.extra_ab_pairs = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};

    {
        OOCRelationWriter writer(tmp.base);
        writer.write(merged);
        writer.close();
    }

    OOCRelationReader reader(tmp.base);
    auto loaded = reader.read(0);
    TEST_ASSERT(loaded.extra_ab_pairs.size() == 4, "should have 4 extra pairs");
    TEST_ASSERT(loaded.extra_ab_pairs[2].first == 5, "extra pair [2] a=5");
    TEST_ASSERT(loaded.extra_ab_pairs[2].second == 6, "extra pair [2] b=6");
    TEST_ASSERT(relations_equal(merged, loaded), "merged relation round-trip");

    TEST_PASS("merged relation with 4 extra (a,b) pairs");
}

void test_owned_pair_reader_survives_path_replacement() {
    TempFiles pair_a(gnfs::util::temp_path("gnfs_test_ooc_owned_pair_a"));
    TempFiles pair_b(gnfs::util::temp_path("gnfs_test_ooc_owned_pair_b"));

    const std::vector<Relation> relations_a{
        make_relation(101, 11, 2, 1),
        make_relation(102, 12, 3, 2),
    };
    const std::vector<Relation> relations_b{
        make_relation(201, 21, 1, 3),
        make_relation(202, 22, 2, 4),
    };
    const auto descriptor_a = write_finalized_pair(pair_a.base, relations_a);
    const auto descriptor_b = write_finalized_pair(pair_b.base, relations_b);

    const TestNativeHandle index_handle = open_native_read_only(pair_a.base + ".relidx");
    OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
    const TestNativeHandle data_handle = open_native_read_only(pair_a.base + ".reldata");
    OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
    TEST_ASSERT(native_handle_is_open(index_handle), "owned index handle should be open");
    TEST_ASSERT(native_handle_is_open(data_handle), "owned data handle should be open");

    replace_file(pair_b.base + ".relidx", pair_a.base + ".relidx");
    replace_file(pair_b.base + ".reldata", pair_a.base + ".reldata");

    OOCRelationReader owned_reader(std::move(index), std::move(data), descriptor_a);
    TEST_ASSERT(!index.valid() && !data.valid(), "owned reader should consume both handles");
    TEST_ASSERT(owned_reader.count() == 2, "owned reader should expose the two A rows");
    TEST_ASSERT(relations_equal(owned_reader.read(0), relations_a[0]),
                "owned reader should read A row 0 after path replacement");
    TEST_ASSERT(relations_equal(owned_reader.read(1), relations_a[1]),
                "owned reader should read A row 1 after path replacement");

    OOCRelationReader path_reader(pair_a.base, descriptor_b);
    TEST_ASSERT(path_reader.count() == 2, "path reader should expose the two replacement B rows");
    TEST_ASSERT(relations_equal(path_reader.read(0), relations_b[0]),
                "path reader should read replacement B row 0");
    TEST_ASSERT(relations_equal(path_reader.read(1), relations_b[1]),
                "path reader should read replacement B row 1");

    TEST_PASS("owned exact pair remains bound after path replacement");
}

void test_owned_pair_reader_zero_and_two_rows() {
    {
        TempFiles empty(gnfs::util::temp_path("gnfs_test_ooc_owned_zero"));
        const auto descriptor = write_finalized_pair(empty.base, {});
        OwnedNativeFile index =
            OwnedNativeFile::adopt_ownership(open_native_read_only(empty.base + ".relidx"));
        OwnedNativeFile data =
            OwnedNativeFile::adopt_ownership(open_native_read_only(empty.base + ".reldata"));

        OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        TEST_ASSERT(reader.count() == 0, "owned zero-row pair should remain empty");
        bool out_of_range = false;
        try {
            (void)reader.read(0);
        } catch (const std::out_of_range&) {
            out_of_range = true;
        }
        TEST_ASSERT(out_of_range, "owned zero-row pair should reject ordinal zero");
    }

    {
        TempFiles two(gnfs::util::temp_path("gnfs_test_ooc_owned_two"));
        const std::vector<Relation> expected{
            make_relation(-17, 31, 2, 2),
            make_relation(18, 32, 3, 1),
        };
        const auto descriptor = write_finalized_pair(two.base, expected);
        OwnedNativeFile index =
            OwnedNativeFile::adopt_ownership(open_native_read_only(two.base + ".relidx"));
        OwnedNativeFile data =
            OwnedNativeFile::adopt_ownership(open_native_read_only(two.base + ".reldata"));

        OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        TEST_ASSERT(reader.count() == 2, "owned two-row pair should report two rows");
        TEST_ASSERT(relations_equal(reader.read(0), expected[0]),
                    "owned two-row pair row 0 should match");
        TEST_ASSERT(relations_equal(reader.read(1), expected[1]),
                    "owned two-row pair row 1 should match");
    }

    TEST_PASS("owned exact pair supports zero and two rows");
}

void test_reader_validity_and_move_state() {
    TempFiles pair(gnfs::util::temp_path("gnfs_test_ooc_reader_move_state"));
    const std::vector<Relation> expected{
        make_relation(-91, 17, 2, 1),
        make_relation(92, 18, 1, 2),
    };
    const auto descriptor = write_finalized_pair(pair.base, expected);

    OOCRelationReader unbound;
    TEST_ASSERT(!unbound.valid(), "default reader should be invalid");

    OOCRelationReader source(pair.base, descriptor);
    TEST_ASSERT(source.valid(), "initialized reader should be valid");

    OOCRelationReader moved(std::move(source));
    TEST_ASSERT(!source.valid() && source.count() == 0,
                "move construction should clear the source reader state");
    TEST_ASSERT(moved.valid() && moved.count() == expected.size(),
                "move construction should preserve the mapped corpus");
    TEST_ASSERT(relations_equal(moved.read(1), expected[1]),
                "move-constructed reader should preserve row access");

    unbound = std::move(moved);
    TEST_ASSERT(!moved.valid() && moved.count() == 0,
                "move assignment should clear the source reader state");
    TEST_ASSERT(unbound.valid() && unbound.count() == expected.size(),
                "move assignment should transfer the mapped corpus");
    TEST_ASSERT(relations_equal(unbound.read(0), expected[0]),
                "move-assigned reader should preserve row access");

    TEST_PASS("reader validity distinguishes initialized and moved-from state");
}

void test_owned_pair_reader_rejects_cross_pair() {
    TempFiles pair_a(gnfs::util::temp_path("gnfs_test_ooc_owned_cross_a"));
    TempFiles pair_b(gnfs::util::temp_path("gnfs_test_ooc_owned_cross_b"));
    const auto descriptor_a = write_finalized_pair(pair_a.base, {make_relation(301, 41, 2, 2)});
    (void)write_finalized_pair(pair_b.base, {make_relation(401, 51, 2, 2)});

    {
        const TestNativeHandle index_handle = open_native_read_only(pair_a.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair_b.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor_a);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned reader should reject index/data from different stores");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "cross-pair validation failure should consume transferred wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle),
                    "cross-pair failure should close transferred index handle");
        TEST_ASSERT(native_handle_is_closed(data_handle),
                    "cross-pair failure should close transferred data handle");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair_a.base + ".reldata");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair_a.base + ".relidx");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor_a);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned reader should reject swapped index/data handles");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "swapped-pair validation failure should consume both wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "swapped-pair failure should close both transferred handles");
    }

    TEST_PASS("owned exact pair rejects cross-pair and swapped handles");
}

void test_owned_pair_reader_rejects_wrong_descriptors() {
    TempFiles pair(gnfs::util::temp_path("gnfs_test_ooc_owned_wrong_descriptor"));
    const auto descriptor = write_finalized_pair(pair.base, {make_relation(501, 61, 2, 1)});

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
        auto legacy_descriptor = descriptor;
        legacy_descriptor.format_version = OOCRelationWriter::FORMAT_VERSION_V2;

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), legacy_descriptor);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject V2 descriptors");
        TEST_ASSERT(index.valid() && data.valid(),
                    "pre-transfer V2 rejection should retain caller ownership");
        TEST_ASSERT(native_handle_is_open(index_handle) && native_handle_is_open(data_handle),
                    "pre-transfer V2 rejection should leave both handles open");
        index.close();
        data.close();
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "caller should be able to close retained V2-rejected handles once");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
        auto zero_generation = descriptor;
        zero_generation.generation = 0;

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), zero_generation);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject zero generation");
        TEST_ASSERT(index.valid() && data.valid(),
                    "zero-generation prevalidation should retain both wrappers");
        TEST_ASSERT(native_handle_is_open(index_handle) && native_handle_is_open(data_handle),
                    "zero-generation prevalidation should leave both handles open");
        index.close();
        data.close();
    }

    {
        OwnedNativeFile index =
            OwnedNativeFile::adopt_ownership(open_native_read_only(pair.base + ".relidx"));
        OwnedNativeFile data =
            OwnedNativeFile::adopt_ownership(open_native_read_only(pair.base + ".reldata"));
        auto different_generation = descriptor;
        different_generation.generation =
            descriptor.generation == UINT64_MAX ? 1 : descriptor.generation + 1;

        OOCRelationReader reader(std::move(index), std::move(data), different_generation);
        TEST_ASSERT(reader.count() == descriptor.count,
                    "nonzero generation is caller metadata, not persisted V3 identity");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
        auto wrong_store = descriptor;
        ++wrong_store.store_id;
        if (wrong_store.store_id == 0) {
            ++wrong_store.store_id;
        }

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), wrong_store);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject wrong store identity");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "post-transfer descriptor failure should consume both wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "wrong-descriptor failure should close both transferred handles");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
        auto wrong_count = descriptor;
        ++wrong_count.count;

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), wrong_count);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject wrong relation count");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "wrong-count failure should consume both wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "wrong-count failure should close both transferred handles");
    }

    {
        TempFiles truncated(gnfs::util::temp_path("gnfs_test_ooc_owned_truncated_data"));
        const auto truncated_descriptor =
            write_finalized_pair(truncated.base, {make_relation(601, 71, 2, 1)});
        TEST_ASSERT(truncated_descriptor.data_end > OOCRelationWriter::DATA_HEADER_BYTES,
                    "one-row V3 data should extend beyond its header");
        resize_file(truncated.base + ".reldata", truncated_descriptor.data_end - 1);

        const TestNativeHandle index_handle = open_native_read_only(truncated.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(truncated.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), truncated_descriptor);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject truncated data extent");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "truncated-data failure should consume both wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "truncated-data failure should close both transferred handles");
    }

    {
        TempFiles extended(gnfs::util::temp_path("gnfs_test_ooc_owned_extended_index"));
        const auto extended_descriptor = write_finalized_pair(extended.base, {});
        resize_file(extended.base + ".relidx",
                    OOCRelationWriter::index_size_for_count(extended_descriptor.count) + 1);

        const TestNativeHandle index_handle = open_native_read_only(extended.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(extended.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), extended_descriptor);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject extended index extent");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "extended-index failure should consume both wrappers");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "extended-index failure should close both transferred handles");
    }

    TEST_PASS("owned exact pair validates descriptor identity and exact extents");
}

void test_owned_pair_reader_rejects_invalid_handles_safely() {
    TempFiles pair(gnfs::util::temp_path("gnfs_test_ooc_owned_invalid_handles"));
    const auto descriptor = write_finalized_pair(pair.base, {});

    {
        OwnedNativeFile index;
        OwnedNativeFile data;
        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject default handles");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "default handles should remain invalid after rejection");
    }

    {
        const TestNativeHandle shared_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile shared = OwnedNativeFile::adopt_ownership(shared_handle);
        bool threw = false;
        try {
            OOCRelationReader reader(std::move(shared), std::move(shared), descriptor);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject one wrapper passed twice");
        TEST_ASSERT(shared.valid(), "aliased-wrapper prevalidation should retain caller ownership");
        TEST_ASSERT(native_handle_is_open(shared_handle),
                    "aliased-wrapper prevalidation should leave its handle open");
        shared.close();
        TEST_ASSERT(native_handle_is_closed(shared_handle),
                    "caller should retain close responsibility after alias rejection");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        OwnedNativeFile data;
        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject an invalid data handle");
        TEST_ASSERT(index.valid() && !data.valid(),
                    "invalid data prevalidation should not consume the valid index wrapper");
        TEST_ASSERT(native_handle_is_open(index_handle),
                    "invalid data prevalidation should leave the index handle open");
        index.close();
        TEST_ASSERT(native_handle_is_closed(index_handle),
                    "caller should retain close responsibility after invalid data rejection");
    }

    {
        OwnedNativeFile index;
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);
        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should reject an invalid index handle");
        TEST_ASSERT(!index.valid() && data.valid(),
                    "invalid index prevalidation should not consume the valid data wrapper");
        TEST_ASSERT(native_handle_is_open(data_handle),
                    "invalid index prevalidation should leave the data handle open");
        data.close();
        TEST_ASSERT(native_handle_is_closed(data_handle),
                    "caller should retain close responsibility after invalid index rejection");
    }

    {
        const TestNativeHandle index_handle = open_native_write_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_read_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should report index mapping failure");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "index mapping failure should happen after dual-handle commit");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "index mapping failure should close both committed handles");
    }

    {
        const TestNativeHandle index_handle = open_native_read_only(pair.base + ".relidx");
        OwnedNativeFile index = OwnedNativeFile::adopt_ownership(index_handle);
        const TestNativeHandle data_handle = open_native_write_only(pair.base + ".reldata");
        OwnedNativeFile data = OwnedNativeFile::adopt_ownership(data_handle);

        bool threw = false;
        try {
            OOCRelationReader reader(std::move(index), std::move(data), descriptor);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        TEST_ASSERT(threw, "owned exact-pair constructor should report data mapping failure");
        TEST_ASSERT(!index.valid() && !data.valid(),
                    "data mapping failure should happen after dual-handle commit");
        TEST_ASSERT(native_handle_is_closed(index_handle) && native_handle_is_closed(data_handle),
                    "data mapping failure should close both committed handles");
    }

    TEST_PASS("owned exact pair prevalidates and closes both handles after commit failures");
}

// Writer 析构时若有 in-flight exception,只写入 MAGIC_INCOMPLETE,
// reader 必须拒绝读(避免 idx/data 不一致)。
void test_writer_exception_path() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_exc"));

    bool reader_threw = false;
    try {
        try {
            OOCRelationWriter writer(tmp.base);
            Relation r = make_relation(1, 2, 3, 2);
            writer.write(r);
            writer.write(r);
            // 模拟 write 中途异常:抛出后,析构期间 std::uncaught_exceptions()
            // 比 ctor 时多 1,close() 走异常分支不写 MAGIC。
            throw std::runtime_error("simulated disk-full mid-write");
        } catch (const std::runtime_error&) {
            // swallow — Writer 析构已发生
        }

        try {
            OOCRelationReader reader(tmp.base);
            (void)reader;
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            if (msg.find("invalid magic") != std::string::npos) {
                reader_threw = true;
            }
        }
    } catch (...) {
    }
    TEST_ASSERT(reader_threw, "reader should reject MAGIC_INCOMPLETE file");
    TEST_PASS("writer exception path → reader rejects incomplete file");
}

void test_large_prime_semantic_contract() {
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_lp_contract"));

    Relation valid = make_relation(701, 31, 1, 1, 1, 1);
    OOCRelationWriter writer(tmp.base);
    writer.write(valid);

    const auto expect_rejected_without_poisoning = [&](Relation invalid, const char* description) {
        bool rejected = false;
        try {
            writer.write(invalid);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        TEST_ASSERT(rejected, description);
        TEST_ASSERT(writer.state() == gnfs::relation::OOCWriterState::Open,
                    "semantic rejection should keep OOC writer appendable");
        TEST_ASSERT(writer.count() == 1,
                    "semantic rejection should not append an index or data record");
    };

    Relation bad_rational_prime = valid;
    bad_rational_prime.rational_large_prime[0].p = 1;
    expect_rejected_without_poisoning(std::move(bad_rational_prime),
                                      "rational p < 2 should be rejected");

    Relation bad_root = valid;
    bad_root.algebraic_large_prime[0].r = bad_root.algebraic_large_prime[0].p;
    expect_rejected_without_poisoning(std::move(bad_root),
                                      "algebraic root >= p should be rejected");

    writer.write(valid);
    const auto descriptor = writer.finalize();
    TEST_ASSERT(descriptor.count == 2, "valid records should remain writable after rejection");
    TEST_PASS("compact large-prime semantic contract rejects invalid writes");

    // Mutate the persisted p field of the first record. Reader construction
    // validates the exact committed records, so malformed bytes cannot enter
    // recovery or ordinary random-access consumers.
    {
        std::fstream data(tmp.base + ".reldata", std::ios::binary | std::ios::in | std::ios::out);
        TEST_ASSERT(data.good(), "semantic contract test should reopen data file");
        constexpr std::streamoff rational_prime_offset =
            static_cast<std::streamoff>(OOCRelationWriter::DATA_HEADER_BYTES) +
            static_cast<std::streamoff>(sizeof(int64_t) + sizeof(uint64_t) + 3 * sizeof(uint32_t));
        const uint64_t invalid_prime = 1;
        data.seekp(rational_prime_offset);
        data.write(reinterpret_cast<const char*>(&invalid_prime), sizeof(invalid_prime));
        TEST_ASSERT(data.good(), "semantic contract test should corrupt p field");
    }
    bool reader_rejected = false;
    try {
        OOCRelationReader reader(tmp.base);
        (void)reader.read(0);
    } catch (const std::runtime_error&) {
        reader_rejected = true;
    }
    TEST_ASSERT(reader_rejected, "reader should reject persisted invalid large-prime values");
    TEST_PASS("compact reader rejects malformed large-prime values");
}

void test_writer_descriptors_use_cloexec() {
#ifdef _WIN32
    TEST_PASS("writer descriptors use close-on-exec (Windows handles are non-inheritable)");
#else
    TempFiles tmp(gnfs::util::temp_path("gnfs_test_ooc_cloexec"));
    {
        OOCRelationWriter writer(tmp.base);
        const int data_descriptor = find_fd_for_path(tmp.base + ".reldata");
        const int index_descriptor = find_fd_for_path(tmp.base + ".relidx");
        TEST_ASSERT(data_descriptor >= 0 && index_descriptor >= 0,
                    "writer should retain both relation file descriptors");
        TEST_ASSERT(descriptor_has_cloexec(data_descriptor),
                    "relation data descriptor should be close-on-exec");
        TEST_ASSERT(descriptor_has_cloexec(index_descriptor),
                    "relation index descriptor should be close-on-exec");
    }
    TEST_PASS("writer descriptors use close-on-exec");
#endif
}

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  Out-of-core Relations Unit Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_write_read_single();
    test_write_read_batch();
    test_random_access();
    test_read_all();
    test_read_range();
    test_empty_relations();
    test_merged_relation_with_extras();
    test_owned_pair_reader_survives_path_replacement();
    test_owned_pair_reader_zero_and_two_rows();
    test_reader_validity_and_move_state();
    test_owned_pair_reader_rejects_cross_pair();
    test_owned_pair_reader_rejects_wrong_descriptors();
    test_owned_pair_reader_rejects_invalid_handles_safely();
    test_writer_exception_path();
    test_large_prime_semantic_contract();
    test_writer_descriptors_use_cloexec();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
