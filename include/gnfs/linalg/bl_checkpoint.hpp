#pragma once

#include "gnfs/util/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gnfs::linalg {

/// Mid-flight Block Lanczos checkpoint for long-running 50d+/60d matrix solves.
///
/// Design note: the Montgomery Block Lanczos solver was removed (see
/// `src/linalg/block_lanczos.cpp` comments). `BlockLanczos::find_dependencies()`
/// dispatches to a packed-GF(2) Gaussian elimination for matrices that fit under
/// the 4 GiB augmented-matrix cap; that path is what this checkpoint serializes.
///
/// What is persisted:
///   - The packed augmented matrix `[I | M]` (m rows, m+n cols, packed by uint64)
///   - The scan cursor: `pivot_row` and `cur_col`
///   - `iteration` (pivots completed; informational)
///
/// What is NOT persisted (acceptable replay):
///   - The post-loop null-space extraction (fast, runs from final aug state)
///   - ThreadPool / std::future state (recreated on resume)
///
/// File layout (`<base>.bl_ckpt`, V4; every u64 is little-endian):
///   [u64 magic]            'BLCKPT01' bytes, or INCOMPLETE = 0 before completion
///   [u64 version]          4
///   [u64 rows]             m
///   [u64 cols]             n
///   [u64 aug_words_per_row] ceil((m+n)/64)
///   [u64 pivot_row]        next pivot row index
///   [u64 cur_col]          next column to scan (m <= cur_col <= m+n)
///   [u64 iteration]        pivots already completed
///   [32 bytes digest]      SHA-256 of the bytes in [8, 136 + 8 * aug_word_count)
///   [32 bytes matrix_fingerprint] SHA-256 of canonical source matrix
///   [u64 aug_word_count]   rows * aug_words_per_row
///   [u64[] aug_payload]    packed augmented matrix data
///
/// A complete file is `136 + 8 * aug_word_count` bytes. The digest starts at the
/// version field, skips its own 32 bytes, and ends at the last payload word, so
/// it binds the cursor, the matrix identity, and the exact order of the payload.
///
/// Version history: V1 used native-endian u64 fields. V2 moved to little-endian
/// fields with XOR-fold header and body checksums. V3 added the source-matrix
/// fingerprint and kept both XOR folds. All three remain decodable: load()
/// returns V1 and V2 as unbound states and V3 as a bound state, but only for
/// diagnostics. The solver resumes V4 only, so an older checkpoint is discarded
/// and elimination restarts. New writes always produce V4.
///
/// Publication protocol (save):
///   1. Create `<path>.tmp` exclusively. If the name already exists, fail
///      without touching it: another writer, or an interrupted save, owns it.
///   2. Write the complete file with INCOMPLETE magic, hashing while writing.
///   3. Patch the digest, then the MAGIC, flush the file to disk, and close it.
///   4. Rename the temporary file over the target, then flush the directory.
/// The target is never truncated or written in place. A failure before step 4
/// leaves the previous checkpoint byte for byte intact and removes the
/// temporary file. A crash can leave only an unpublished temporary file
/// (INCOMPLETE, or complete but never renamed); readers never consult that name.
struct BlockLanczosCheckpoint {
    // MAGIC = "BLCKPT01" little-endian bytes
    static constexpr uint64_t MAGIC = 0x3130'5450'4B43'4C42ULL;
    static constexpr uint64_t MAGIC_INCOMPLETE = 0ULL;
    static constexpr uint64_t VERSION_V1 = 1;
    static constexpr uint64_t VERSION_V2 = 2;
    static constexpr uint64_t VERSION_V3 = 3;
    static constexpr uint64_t VERSION_V4 = 4;
    /// Current on-disk version emitted by save().
    static constexpr uint64_t VERSION = VERSION_V4;
    /// Maximum packed payload accepted by load() (64 GiB).
    static constexpr uint64_t MAX_AUG_WORDS = (64ULL * 1024 * 1024 * 1024) / 8;
    /// Width of one encoded word. Byte offsets below are written with this constant
    /// rather than with sizeof inside pointer arithmetic: the buffers are byte
    /// sequences, and `bytes + k * sizeof(uint64_t)` reads like the common mistake
    /// of scaling an element pointer by its own element size.
    static constexpr size_t WORD_BYTES = sizeof(uint64_t);

    // Stable V4 byte offsets used by format-level tests. The digest replaces
    // the V3 header checksum word and is 32 bytes wide, so every later field
    // moves 24 bytes down; the trailing body checksum word is gone.
    static constexpr size_t DIGEST_OFFSET = 8 * sizeof(uint64_t);
    static constexpr size_t FINGERPRINT_OFFSET = DIGEST_OFFSET + util::SHA256_DIGEST_BYTES;
    static constexpr size_t AUG_WORD_COUNT_OFFSET = FINGERPRINT_OFFSET + util::SHA256_DIGEST_BYTES;
    /// Fixed size of a V4 file: everything except the payload words.
    static constexpr size_t PAYLOAD_OFFSET = AUG_WORD_COUNT_OFFSET + sizeof(uint64_t);
    static_assert(DIGEST_OFFSET == 64 && FINGERPRINT_OFFSET == 96 && AUG_WORD_COUNT_OFFSET == 128 &&
                  PAYLOAD_OFFSET == 136);

    /// On-disk version of the file that load() decoded. Zero for a state built
    /// in memory. save() always writes VERSION, whatever this field holds.
    uint64_t format_version = 0;
    uint64_t rows = 0;
    uint64_t cols = 0;
    uint64_t aug_words_per_row = 0;
    uint64_t pivot_row = 0;
    uint64_t cur_col = 0;
    uint64_t iteration = 0;
    std::optional<util::Sha256Digest> matrix_fingerprint;
    std::vector<uint64_t> aug; // size = rows * aug_words_per_row

    /// Name of the temporary file that save() writes before the rename.
    [[nodiscard]] static std::string temporary_path(const std::string& path) {
        return path + ".tmp";
    }

    /// Serialize to path. Returns false on I/O or allocation failure; never throws.
    ///
    /// The state is published through `<path>.tmp` and a rename, so the previous
    /// checkpoint stays readable until the new one is complete on disk. A
    /// temporary file that already exists is never reused or removed; the save
    /// fails instead. When the directory flush in the last step fails, the new
    /// checkpoint is already in place although this function returns false.
    bool save(const std::string& path) const noexcept {
        try {
            return save_impl(path);
        } catch (...) {
            return false;
        }
    }

    /// Deserialize from path. Returns nullopt on any failure (invalid magic,
    /// version mismatch, truncation, digest or checksum mismatch, INCOMPLETE,
    /// I/O error, or allocation failure).
    /// Caller may distinguish via `exists_and_valid()` if they need a reason.
    static std::optional<BlockLanczosCheckpoint> load(const std::string& path) noexcept {
        // Checkpoint input is optional recovery data. Keep the noexcept API
        // fail-closed even when the stream/vector implementation cannot allocate
        // its bookkeeping or payload buffers.
        try {
            return load_impl(path);
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        } catch (const std::length_error&) {
            return std::nullopt;
        } catch (...) {
            // Recovery data is optional. Any unexpected parser or stream
            // exception must fail closed instead of escaping this noexcept API.
            return std::nullopt;
        }
    }

    /// Cheap check: does the file exist with a valid (finalized) magic?
    /// Returns false on I/O/allocation failure or INCOMPLETE; never throws.
    static bool exists_and_valid(const std::string& path) noexcept {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::array<std::byte, sizeof(uint64_t)> magic_bytes{};
            std::array<std::byte, sizeof(uint64_t)> version_bytes{};
            if (!read_exact(in, magic_bytes.data(), magic_bytes.size()) ||
                !read_exact(in, version_bytes.data(), version_bytes.size())) {
                return false;
            }
            uint64_t native_magic = 0;
            uint64_t native_version = 0;
            std::memcpy(&native_magic, magic_bytes.data(), sizeof(native_magic));
            std::memcpy(&native_version, version_bytes.data(), sizeof(native_version));
            const uint64_t little_version = decode_u64_le(version_bytes.data());
            return (decode_u64_le(magic_bytes.data()) == MAGIC &&
                    is_little_endian_version(little_version)) ||
                   (native_magic == MAGIC && native_version == VERSION_V1);
        } catch (...) {
            return false;
        }
    }

    /// Remove the checkpoint and any temporary file of an interrupted save
    /// (called on successful BL completion and when a checkpoint is rejected).
    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
        remove_temporary(path);
    }

    /// Remove only `<path>.tmp`. A solve calls this once when it starts using a
    /// checkpoint path: a temporary file that exists before the first save of
    /// this process cannot belong to it, and keeping it would block every save.
    static void remove_temporary(const std::string& path) noexcept {
        try {
            std::remove(temporary_path(path).c_str());
        } catch (...) {
            // Cleanup is best-effort by contract, including allocation errors.
        }
    }

private:
    using NativeHandle = int;
    static constexpr NativeHandle INVALID_NATIVE_HANDLE = -1;
    /// Payload words staged per hash update and write. One update per chunk
    /// keeps the digest a single sequential scan instead of a call per word.
    static constexpr size_t CHUNK_WORDS = 8192;

    /// RAII owner of one native file descriptor; it is neither copyable nor
    /// movable. A descriptor that is still open when the owner dies is closed
    /// without reporting an error.
    class NativeFile final {
    public:
        explicit NativeFile(NativeHandle handle) noexcept : handle_(handle) {}
        NativeFile(const NativeFile&) = delete;
        NativeFile& operator=(const NativeFile&) = delete;
        ~NativeFile() {
            if (valid())
                (void)close();
        }

        [[nodiscard]] bool valid() const noexcept {
            return handle_ >= 0;
        }

        /// Write the whole range. A failed or stalled write returns false.
        [[nodiscard]] bool write(const std::byte* data, size_t size) const noexcept {
            while (size != 0) {
#ifdef _WIN32
                const int written = ::_write(handle_, data, static_cast<unsigned int>(size));
                if (written <= 0)
                    return false;
#else
                const ssize_t written = ::write(handle_, data, size);
                if (written < 0) {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (written == 0)
                    return false;
#endif
                data += static_cast<size_t>(written);
                size -= static_cast<size_t>(written);
            }
            return true;
        }

        [[nodiscard]] bool seek(long offset) const noexcept {
#ifdef _WIN32
            return ::_lseek(handle_, offset, SEEK_SET) != -1L;
#else
            return ::lseek(handle_, offset, SEEK_SET) != static_cast<off_t>(-1);
#endif
        }

#ifdef _WIN32
        // _commit() flushes the user buffers and calls FlushFileBuffers(). The
        // CRT reports every failure as -1 and defines no EINTR retry for it.
        [[nodiscard]] bool sync() const noexcept {
            return ::_commit(handle_) == 0;
        }
#else
        /// Flush the file content to the device. This is the same boundary that
        /// durable_immutable_file uses: F_FULLFSYNC on Apple, fsync elsewhere.
        [[nodiscard]] bool sync() const noexcept {
            for (;;) {
#if defined(__APPLE__)
                const int result = ::fcntl(handle_, F_FULLFSYNC);
#else
                const int result = ::fsync(handle_);
#endif
                if (result == 0)
                    return true;
                if (errno != EINTR)
                    return false;
            }
        }
#endif

        [[nodiscard]] bool close() noexcept {
            const NativeHandle handle = std::exchange(handle_, INVALID_NATIVE_HANDLE);
#ifdef _WIN32
            return ::_close(handle) == 0;
#else
            return ::close(handle) == 0;
#endif
        }

    private:
        NativeHandle handle_ = INVALID_NATIVE_HANDLE;
    };

    /// Deletes the temporary file when a save fails after creating it. The
    /// removal uses only the C library, so it cannot throw during unwinding.
    struct TemporaryFileCleanup final {
        std::string path;
        bool armed = false;

        ~TemporaryFileCleanup() {
            if (armed)
                std::remove(path.c_str());
        }
    };

    bool save_impl(const std::string& path) const {
        // Do not touch the file system for a state without provenance, for a
        // payload that load() would reject, or for an empty target name.
        if (!matrix_fingerprint || path.empty())
            return false;
        const uint64_t aug_word_count = static_cast<uint64_t>(aug.size());
        if (!word_count_matches(rows, aug_words_per_row, aug_word_count))
            return false;

        // Every allocation happens before the temporary file exists, so an
        // allocation failure cannot leave a partial temporary file behind.
        TemporaryFileCleanup cleanup{temporary_path(path)};
        const std::filesystem::path target(path);
        const std::filesystem::path temporary(cleanup.path);
        std::filesystem::path parent = target.parent_path();
        if (parent.empty())
            parent = std::filesystem::path(".");

        NativeFile file(open_exclusive(temporary));
        if (!file.valid())
            // Another writer or an interrupted save owns the name.
            return false;
        cleanup.armed = true;

        // The digest covers bytes [8, PAYLOAD_OFFSET + 8 * aug_word_count) in
        // file order: the version word, the six fields, the fingerprint, the
        // word count, and the payload. It skips only its own 32 bytes and the
        // magic, so the final magic flip does not change it.
        util::Sha256Accumulator hasher;
        std::array<std::byte, PAYLOAD_OFFSET> header{};
        encode_u64_le(header.data(), MAGIC_INCOMPLETE);
        encode_u64_le(header.data() + WORD_BYTES, VERSION);
        encode_u64_le(header.data() + 2 * WORD_BYTES, rows);
        encode_u64_le(header.data() + 3 * WORD_BYTES, cols);
        encode_u64_le(header.data() + 4 * WORD_BYTES, aug_words_per_row);
        encode_u64_le(header.data() + 5 * WORD_BYTES, pivot_row);
        encode_u64_le(header.data() + 6 * WORD_BYTES, cur_col);
        encode_u64_le(header.data() + 7 * WORD_BYTES, iteration);
        // [DIGEST_OFFSET, FINGERPRINT_OFFSET) stays zero until the payload is hashed.
        std::memcpy(header.data() + FINGERPRINT_OFFSET, matrix_fingerprint->bytes.data(),
                    matrix_fingerprint->bytes.size());
        encode_u64_le(header.data() + AUG_WORD_COUNT_OFFSET, aug_word_count);

        if (!hasher.update(std::span<const std::byte>(header.data() + WORD_BYTES,
                                                      DIGEST_OFFSET - sizeof(uint64_t))) ||
            !hasher.update(std::span<const std::byte>(header.data() + FINGERPRINT_OFFSET,
                                                      PAYLOAD_OFFSET - FINGERPRINT_OFFSET)) ||
            !file.write(header.data(), header.size())) {
            return false;
        }

        std::array<std::byte, CHUNK_WORDS * sizeof(uint64_t)> staging{};
        size_t index = 0;
        while (index < aug.size()) {
            const size_t words = std::min(CHUNK_WORDS, aug.size() - index);
            const size_t bytes = words * sizeof(uint64_t);
            for (size_t word = 0; word < words; ++word)
                encode_u64_le(staging.data() + word * WORD_BYTES, aug[index + word]);
            index += words;
            if (!hasher.update(std::span<const std::byte>(staging.data(), bytes)) ||
                !file.write(staging.data(), bytes)) {
                return false;
            }
        }

        const std::optional<util::Sha256Digest> digest = hasher.finalize();
        if (!digest)
            return false;
        std::array<std::byte, sizeof(uint64_t)> magic{};
        encode_u64_le(magic.data(), MAGIC);
        if (!file.seek(static_cast<long>(DIGEST_OFFSET)) ||
            !file.write(digest->bytes.data(), digest->bytes.size()) || !file.seek(0) ||
            !file.write(magic.data(), magic.size()) || !file.sync() || !file.close()) {
            return false;
        }

        std::error_code rename_error;
        std::filesystem::rename(temporary, target, rename_error);
        if (rename_error)
            return false;
        // Nothing after the rename allocates or throws, and the temporary name
        // is gone. Only the directory entry still has to reach the disk.
        cleanup.armed = false;
        return sync_directory(parent);
    }

    static std::optional<BlockLanczosCheckpoint> load_impl(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;

        in.seekg(0, std::ios::end);
        const auto end_pos = in.tellg();
        if (end_pos == std::ifstream::pos_type(-1))
            return std::nullopt;
        const uint64_t file_size = static_cast<uint64_t>(end_pos);
        in.seekg(0, std::ios::beg);
        if (!in)
            return std::nullopt;

        std::array<std::byte, sizeof(uint64_t)> magic_bytes{};
        std::array<std::byte, sizeof(uint64_t)> version_bytes{};
        if (!read_exact(in, magic_bytes.data(), magic_bytes.size()) ||
            !read_exact(in, version_bytes.data(), version_bytes.size())) {
            return std::nullopt;
        }

        uint64_t native_magic = 0;
        uint64_t native_version = 0;
        std::memcpy(&native_magic, magic_bytes.data(), sizeof(native_magic));
        std::memcpy(&native_version, version_bytes.data(), sizeof(native_version));
        const uint64_t little_magic = decode_u64_le(magic_bytes.data());
        const uint64_t little_version = decode_u64_le(version_bytes.data());
        const bool little_endian =
            little_magic == MAGIC && is_little_endian_version(little_version);
        const bool native_endian_v1 = native_magic == MAGIC && native_version == VERSION_V1;
        if (!little_endian && !native_endian_v1)
            return std::nullopt;

        const uint64_t version = little_endian ? little_version : VERSION_V1;
        if (version == VERSION_V4)
            return load_v4(in, version_bytes, file_size);

        // V1 to V3: the original field order and XOR-fold checksums.
        auto read_u64 = [&in, little_endian](uint64_t& value) {
            return little_endian ? read_u64_le(in, value) : read_u64_native(in, value);
        };

        BlockLanczosCheckpoint ck;
        ck.format_version = version;
        if (!read_u64(ck.rows) || !read_u64(ck.cols) || !read_u64(ck.aug_words_per_row) ||
            !read_u64(ck.pivot_row) || !read_u64(ck.cur_col) || !read_u64(ck.iteration)) {
            return std::nullopt;
        }

        uint64_t header_csum = 0;
        if (!read_u64(header_csum))
            return std::nullopt;
        if (version == VERSION_V3) {
            ck.matrix_fingerprint.emplace();
            if (!read_exact(in, ck.matrix_fingerprint->bytes.data(),
                            ck.matrix_fingerprint->bytes.size())) {
                return std::nullopt;
            }
        }
        const uint64_t expected_hcsum =
            version ^ ck.rows ^ ck.cols ^ ck.aug_words_per_row ^ ck.pivot_row ^ ck.cur_col ^
            ck.iteration ^
            (ck.matrix_fingerprint ? fingerprint_checksum(*ck.matrix_fingerprint) : 0);
        if (header_csum != expected_hcsum)
            return std::nullopt;

        uint64_t aug_word_count = 0;
        if (!read_u64(aug_word_count))
            return std::nullopt;

        if (!word_count_matches(ck.rows, ck.aug_words_per_row, aug_word_count))
            return std::nullopt;

        const uint64_t fixed_file_bytes =
            11ULL * 8ULL + (version == VERSION_V3 ? util::SHA256_DIGEST_BYTES : 0);
        if (!payload_frame_is_acceptable(aug_word_count, fixed_file_bytes, file_size) ||
            aug_word_count > static_cast<uint64_t>(ck.aug.max_size())) {
            return std::nullopt;
        }

        // The size checks above reject malformed/absurd files, but a
        // valid-at-the-wire payload can still exceed available memory. The
        // enclosing catch converts that failure into the documented nullopt.
        ck.aug.resize(static_cast<size_t>(aug_word_count));
        for (uint64_t& word : ck.aug) {
            if (!read_u64(word))
                return std::nullopt;
        }

        uint64_t body_csum = 0;
        if (!read_u64(body_csum))
            return std::nullopt;
        uint64_t expected_bcsum = 0;
        for (uint64_t w : ck.aug)
            expected_bcsum ^= w;
        if (body_csum != expected_bcsum)
            return std::nullopt;

        return ck;
    }

    /// Decode and verify a V4 file. The stream is positioned right after the
    /// version word. Only the cheap size guards run before the payload
    /// allocation; every decoded value is trusted only after the digest match.
    static std::optional<BlockLanczosCheckpoint>
    load_v4(std::ifstream& in, const std::array<std::byte, sizeof(uint64_t)>& version_bytes,
            uint64_t file_size) {
        BlockLanczosCheckpoint ck;
        ck.format_version = VERSION_V4;

        // One sequential scan: version word, six fields, fingerprint, word
        // count, then the payload in staging chunks.
        util::Sha256Accumulator hasher;
        std::array<std::byte, 6 * sizeof(uint64_t)> fields{};
        util::Sha256Digest stored_digest;
        util::Sha256Digest fingerprint;
        std::array<std::byte, sizeof(uint64_t)> count_bytes{};
        if (!hasher.update(version_bytes) || !read_exact(in, fields.data(), fields.size()) ||
            !hasher.update(fields) ||
            !read_exact(in, stored_digest.bytes.data(), stored_digest.bytes.size()) ||
            !read_exact(in, fingerprint.bytes.data(), fingerprint.bytes.size()) ||
            !hasher.update(fingerprint.bytes) ||
            !read_exact(in, count_bytes.data(), count_bytes.size()) ||
            !hasher.update(count_bytes)) {
            return std::nullopt;
        }

        ck.rows = decode_u64_le(fields.data());
        ck.cols = decode_u64_le(fields.data() + WORD_BYTES);
        ck.aug_words_per_row = decode_u64_le(fields.data() + 2 * WORD_BYTES);
        ck.pivot_row = decode_u64_le(fields.data() + 3 * WORD_BYTES);
        ck.cur_col = decode_u64_le(fields.data() + 4 * WORD_BYTES);
        ck.iteration = decode_u64_le(fields.data() + 5 * WORD_BYTES);
        ck.matrix_fingerprint = fingerprint;
        const uint64_t aug_word_count = decode_u64_le(count_bytes.data());

        if (!word_count_matches(ck.rows, ck.aug_words_per_row, aug_word_count) ||
            !payload_frame_is_acceptable(aug_word_count, static_cast<uint64_t>(PAYLOAD_OFFSET),
                                         file_size) ||
            aug_word_count > static_cast<uint64_t>(ck.aug.max_size())) {
            return std::nullopt;
        }

        ck.aug.resize(static_cast<size_t>(aug_word_count));
        std::array<std::byte, CHUNK_WORDS * sizeof(uint64_t)> staging{};
        size_t decoded = 0;
        while (decoded < ck.aug.size()) {
            const size_t words = std::min(CHUNK_WORDS, ck.aug.size() - decoded);
            const size_t bytes = words * sizeof(uint64_t);
            if (!read_exact(in, staging.data(), bytes) ||
                !hasher.update(std::span<const std::byte>(staging.data(), bytes))) {
                return std::nullopt;
            }
            for (size_t word = 0; word < words; ++word)
                ck.aug[decoded + word] = decode_u64_le(staging.data() + word * WORD_BYTES);
            decoded += words;
        }

        const std::optional<util::Sha256Digest> digest = hasher.finalize();
        if (!digest || !(*digest == stored_digest))
            return std::nullopt;
        return ck;
    }

    /// Create the file exclusively. An existing name is neither opened nor
    /// truncated, so two writers can never share one temporary file.
    [[nodiscard]] static NativeHandle open_exclusive(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
        return ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                        _S_IREAD | _S_IWRITE);
#else
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    }

    /// Flush the directory entry that the rename produced. durable_immutable_file
    /// cannot be reused here: its directory sync is reachable only through
    /// publish(), which takes the whole content in memory and runs before its
    /// own creation step, never after a rename. These are the same platform
    /// calls its production FileOps uses.
    [[nodiscard]] static bool sync_directory(const std::filesystem::path& directory) noexcept {
#ifdef _WIN32
        const HANDLE handle = ::CreateFileW(
            directory.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        const BOOL flushed = ::FlushFileBuffers(handle);
        return ::CloseHandle(handle) && flushed;
#else
        const NativeHandle handle = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (handle < 0)
            return false;
        for (;;) {
#if defined(__APPLE__)
            const int result = ::fcntl(handle, F_FULLFSYNC);
#else
            const int result = ::fsync(handle);
#endif
            if (result == 0)
                break;
            if (errno != EINTR) {
                (void)::close(handle);
                return false;
            }
        }
        return ::close(handle) == 0;
#endif
    }

    /// rows * aug_words_per_row must equal the word count, without overflow.
    /// A zero row count or word width requires an empty payload.
    static bool word_count_matches(uint64_t row_count, uint64_t words_per_row,
                                   uint64_t aug_word_count) noexcept {
        if (row_count == 0 || words_per_row == 0)
            return aug_word_count == 0;
        if (row_count > UINT64_MAX / words_per_row)
            return false;
        return aug_word_count == row_count * words_per_row;
    }

    /// Guard against absurd allocations (caps at 64 GiB packed payload) and
    /// require the exact frame size `fixed_file_bytes + 8 * aug_word_count`.
    static bool payload_frame_is_acceptable(uint64_t aug_word_count, uint64_t fixed_file_bytes,
                                            uint64_t file_size) noexcept {
        if (aug_word_count > MAX_AUG_WORDS)
            return false;
        if (aug_word_count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)()))
            return false;
        if (aug_word_count > (UINT64_MAX - fixed_file_bytes) / 8ULL)
            return false;
        return file_size == fixed_file_bytes + aug_word_count * 8ULL;
    }

    static bool is_little_endian_version(uint64_t version) noexcept {
        return version == VERSION_V2 || version == VERSION_V3 || version == VERSION_V4;
    }

    static uint64_t fingerprint_checksum(const util::Sha256Digest& digest) noexcept {
        uint64_t result = 0;
        for (size_t index = 0; index < digest.bytes.size(); ++index) {
            result ^= static_cast<uint64_t>(std::to_integer<uint8_t>(digest.bytes[index]))
                      << ((index % sizeof(uint64_t)) * 8);
        }
        return result;
    }

    static void encode_u64_le(std::byte* destination, uint64_t value) noexcept {
        for (size_t i = 0; i < sizeof(uint64_t); ++i)
            destination[i] = static_cast<std::byte>(value >> (i * 8));
    }

    static bool read_exact(std::ifstream& in, void* destination, size_t size) {
        in.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        return in.gcount() == static_cast<std::streamsize>(size);
    }

    static uint64_t decode_u64_le(const std::byte* bytes) noexcept {
        uint64_t value = 0;
        for (size_t i = 0; i < sizeof(uint64_t); ++i) {
            value |= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[i])) << (i * 8);
        }
        return value;
    }

    static bool read_u64_le(std::ifstream& in, uint64_t& value) {
        std::array<std::byte, sizeof(uint64_t)> bytes{};
        if (!read_exact(in, bytes.data(), bytes.size()))
            return false;
        value = decode_u64_le(bytes.data());
        return true;
    }

    static bool read_u64_native(std::ifstream& in, uint64_t& value) {
        return read_exact(in, &value, sizeof(value));
    }
};

/// Parse `GNFS_BL_CHECKPOINT` env var. Returns empty string when unset.
inline std::string bl_checkpoint_base_path() noexcept {
    const char* env = std::getenv("GNFS_BL_CHECKPOINT");
    return (env != nullptr) ? std::string(env) : std::string();
}

/// Compose full checkpoint file path: `<base>.bl_ckpt`. Returns "" when disabled.
inline std::string bl_checkpoint_full_path() noexcept {
    auto base = bl_checkpoint_base_path();
    if (base.empty())
        return {};
    return base + ".bl_ckpt";
}

/// Parse `GNFS_BL_CHECKPOINT_INTERVAL` env. Defaults to 50 when unset/invalid.
/// Clamped to [1, 1'000'000] to avoid pathological config.
inline uint64_t bl_checkpoint_interval() noexcept {
    constexpr uint64_t DEFAULT_INTERVAL = 50;
    constexpr uint64_t MIN_INTERVAL = 1;
    constexpr uint64_t MAX_INTERVAL = 1'000'000;
    const char* env = std::getenv("GNFS_BL_CHECKPOINT_INTERVAL");
    if (env == nullptr || env[0] == '\0')
        return DEFAULT_INTERVAL;
    char* endp = nullptr;
    unsigned long long v = std::strtoull(env, &endp, 10);
    if (endp == env || v == 0)
        return DEFAULT_INTERVAL;
    if (v < MIN_INTERVAL)
        return MIN_INTERVAL;
    if (v > MAX_INTERVAL)
        return MAX_INTERVAL;
    return static_cast<uint64_t>(v);
}

} // namespace gnfs::linalg
