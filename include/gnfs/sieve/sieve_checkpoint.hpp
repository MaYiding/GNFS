#pragma once

#include "sieve_run_identity.hpp"

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
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
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gnfs::sieve {

namespace sieve_checkpoint_detail {

inline std::runtime_error io_error(const std::string& operation, const std::string& path,
                                   int error_number = errno) {
    return std::runtime_error("SieveCheckpoint::save: " + operation + " " + path + ": " +
                              std::strerror(error_number));
}

inline void sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
    const int fd = ::_wopen(path.c_str(), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        throw io_error("cannot reopen temporary file", path.string());
    }
    if (::_commit(fd) != 0) {
        const int saved_errno = errno;
        ::_close(fd);
        throw io_error("cannot flush temporary file", path.string(), saved_errno);
    }
    if (::_close(fd) != 0) {
        throw io_error("cannot close temporary file", path.string());
    }
#else
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDWR);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        throw io_error("cannot reopen temporary file", path.string());
    }

    int sync_result = -1;
    do {
        sync_result = ::fsync(fd);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        const int saved_errno = errno;
        ::close(fd);
        throw io_error("cannot flush temporary file", path.string(), saved_errno);
    }
    if (::close(fd) != 0) {
        throw io_error("cannot close temporary file", path.string());
    }
#endif
}

inline void atomic_replace(const std::filesystem::path& temporary,
                           const std::filesystem::path& target) {
#ifdef _WIN32
    if (!::MoveFileExW(temporary.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = static_cast<unsigned long>(::GetLastError());
        throw std::runtime_error("SieveCheckpoint::save: cannot publish " + target.string() +
                                 " (Win32 error " + std::to_string(code) + ")");
    }
#else
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        throw io_error("cannot publish", target.string());
    }

    // Persist the directory entry as well as the file contents. A completed
    // save therefore survives a process crash and, on durable filesystems, a
    // machine crash after save() returns.
    auto parent = target.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    int dir_fd = -1;
    do {
        dir_fd = ::open(parent.c_str(), O_RDONLY);
    } while (dir_fd < 0 && errno == EINTR);
    if (dir_fd < 0) {
        throw io_error("cannot open checkpoint directory", parent.string());
    }

    int sync_result = -1;
    do {
        sync_result = ::fsync(dir_fd);
    } while (sync_result != 0 && errno == EINTR);
    if (sync_result != 0) {
        const int saved_errno = errno;
        ::close(dir_fd);
        throw io_error("cannot flush checkpoint directory", parent.string(), saved_errno);
    }
    if (::close(dir_fd) != 0) {
        throw io_error("cannot close checkpoint directory", parent.string());
    }
#endif
}

inline void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
    }
}

inline void append_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xffULL));
    }
}

inline uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t& cursor, size_t limit) {
    if (cursor > limit || 4 > limit - cursor) {
        throw std::runtime_error("SieveCheckpoint::load: truncated u32 field");
    }
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
    }
    return value;
}

inline uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t& cursor, size_t limit) {
    if (cursor > limit || 8 > limit - cursor) {
        throw std::runtime_error("SieveCheckpoint::load: truncated u64 field");
    }
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(bytes[cursor++]) << shift;
    }
    return value;
}

inline uint64_t checksum(const uint8_t* data, size_t size) noexcept {
    // FNV-1a is not a cryptographic checksum. It is intentionally used only to
    // reject torn writes and accidental corruption before applying resume state.
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace sieve_checkpoint_detail

/// Paired sieve/OOC checkpoint for long-running 50d+/60d factorizations.
///
/// V2 binds the Special-Q cursor to one exact run identity and one exact,
/// durable relation-store prefix. The official checkpoint is replaced only
/// after a complete temporary file has been flushed, protected by its
/// checksum, and marked complete.
struct SieveCheckpoint {
    // MAGIC = 'GNFSSCKP'; MAGIC_INCOMPLETE is used only in the temporary file.
    static constexpr uint64_t MAGIC = 0x474E465353434B50ULL;
    static constexpr uint64_t MAGIC_INCOMPLETE = 0x474E465353434B4EULL;
    static constexpr uint64_t VERSION = 2;
    static constexpr uint32_t MAX_PATH_LENGTH = 4096;
    static constexpr uint32_t MAX_RUN_N_LENGTH = 1U << 20;

    // Stable V2 byte offsets used by format-level tests and future readers.
    // All values are little-endian. Variable strings are run_n then OOC path.
    static constexpr size_t WIRE_VERSION_OFFSET = 8;
    static constexpr size_t WIRE_RUN_FINGERPRINT_LO_OFFSET = 48;
    static constexpr size_t WIRE_RUN_FINGERPRINT_HI_OFFSET = 56;
    static constexpr size_t WIRE_OOC_GENERATION_OFFSET = 80;
    static constexpr size_t WIRE_RUN_N_LENGTH_OFFSET = 104;
    static constexpr size_t WIRE_OOC_PATH_LENGTH_OFFSET = 108;
    static constexpr size_t WIRE_STRINGS_OFFSET = 112;
    static constexpr size_t WIRE_PAYLOAD_FIXED_SIZE = 104;

    enum class SaveStage {
        PayloadFlushed,
        TemporaryFileCompleted,
        Published,
    };
    using SaveStageHook = void (*)(SaveStage);

    uint64_t sq_count = 0;
    uint32_t current_index = 0;
    int32_t round = 0;
    uint64_t batch_target = 0;
    uint64_t candidates_total = 0;

    std::string run_n;
    uint64_t run_fingerprint_lo = 0;
    uint64_t run_fingerprint_hi = 0;

    uint64_t ooc_format_version = 0;
    uint64_t ooc_store_id = 0;
    uint64_t ooc_generation = 0;
    uint64_t ooc_relation_count = 0;
    uint64_t ooc_data_end = 0;
    std::string ooc_base_path;

    friend bool operator==(const SieveCheckpoint&, const SieveCheckpoint&) = default;

    [[nodiscard]] bool matches_run_identity(const SieveRunIdentity& identity) const noexcept {
        return run_n == identity.run_n && run_fingerprint_lo == identity.fingerprint_lo &&
               run_fingerprint_hi == identity.fingerprint_hi;
    }

    [[nodiscard]] static std::string temporary_path(const std::string& path) {
        return path + ".tmp";
    }

    /// Publish a new checkpoint without truncating the previous official file.
    /// The optional hook is for deterministic crash testing; PayloadFlushed and
    /// TemporaryFileCompleted both run before publication, while Published runs
    /// only after the atomic replacement and directory flush have succeeded.
    void save(const std::string& path, SaveStageHook hook = nullptr) const {
        validate("save");
        if (path.empty()) {
            throw std::runtime_error("SieveCheckpoint::save: empty checkpoint path");
        }

        std::vector<uint8_t> payload;
        payload.reserve(WIRE_PAYLOAD_FIXED_SIZE + run_n.size() + ooc_base_path.size());
        using namespace sieve_checkpoint_detail;
        append_u64(payload, VERSION);
        append_u64(payload, sq_count);
        append_u32(payload, current_index);
        append_u32(payload, std::bit_cast<uint32_t>(round));
        append_u64(payload, batch_target);
        append_u64(payload, candidates_total);
        append_u64(payload, run_fingerprint_lo);
        append_u64(payload, run_fingerprint_hi);
        append_u64(payload, ooc_format_version);
        append_u64(payload, ooc_store_id);
        append_u64(payload, ooc_generation);
        append_u64(payload, ooc_relation_count);
        append_u64(payload, ooc_data_end);
        append_u32(payload, static_cast<uint32_t>(run_n.size()));
        append_u32(payload, static_cast<uint32_t>(ooc_base_path.size()));
        payload.insert(payload.end(), run_n.begin(), run_n.end());
        payload.insert(payload.end(), ooc_base_path.begin(), ooc_base_path.end());

        std::vector<uint8_t> bytes;
        bytes.reserve(8 + payload.size() + 8);
        append_u64(bytes, MAGIC_INCOMPLETE);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        append_u64(bytes, checksum(payload.data(), payload.size()));

        const std::filesystem::path target(path);
        const std::filesystem::path temporary(temporary_path(path));
        try {
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out) {
                    throw std::runtime_error("SieveCheckpoint::save: cannot open temporary file " +
                                             temporary.string());
                }
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (!out) {
                    throw std::runtime_error(
                        "SieveCheckpoint::save: temporary payload write failed");
                }
            }
            sync_file(temporary);
            if (hook != nullptr) {
                hook(SaveStage::PayloadFlushed);
            }

            std::vector<uint8_t> complete_magic;
            complete_magic.reserve(8);
            append_u64(complete_magic, MAGIC);
            {
                std::fstream out(temporary, std::ios::binary | std::ios::in | std::ios::out);
                if (!out) {
                    throw std::runtime_error(
                        "SieveCheckpoint::save: cannot reopen temporary file " +
                        temporary.string());
                }
                out.write(reinterpret_cast<const char*>(complete_magic.data()), 8);
                out.flush();
                if (!out) {
                    throw std::runtime_error("SieveCheckpoint::save: temporary magic write failed");
                }
            }
            sync_file(temporary);
            if (hook != nullptr) {
                hook(SaveStage::TemporaryFileCompleted);
            }

            atomic_replace(temporary, target);
            if (hook != nullptr) {
                hook(SaveStage::Published);
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    /// Load only a complete, exact V2 file. V1, incomplete, truncated,
    /// checksummed-but-malformed, and trailing-byte files are all rejected.
    static SieveCheckpoint load(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw std::runtime_error("SieveCheckpoint::load: cannot open " + path);
        }

        const auto end = in.tellg();
        constexpr size_t fixed_size = 8 + WIRE_PAYLOAD_FIXED_SIZE + 8;
        constexpr size_t max_size = fixed_size + MAX_RUN_N_LENGTH + MAX_PATH_LENGTH;
        if (end < 0 || static_cast<uint64_t>(end) < fixed_size) {
            throw std::runtime_error("SieveCheckpoint::load: file too small");
        }
        if (static_cast<uint64_t>(end) > max_size) {
            throw std::runtime_error("SieveCheckpoint::load: file too large");
        }

        std::vector<uint8_t> bytes(static_cast<size_t>(end));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in || in.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw std::runtime_error("SieveCheckpoint::load: file read failed");
        }

        using namespace sieve_checkpoint_detail;
        size_t cursor = 0;
        const uint64_t magic = read_u64(bytes, cursor, bytes.size());
        if (magic != MAGIC) {
            throw std::runtime_error("SieveCheckpoint::load: invalid or incomplete magic");
        }

        const size_t checksum_offset = bytes.size() - 8;
        size_t checksum_cursor = checksum_offset;
        const uint64_t stored_checksum = read_u64(bytes, checksum_cursor, bytes.size());
        const uint64_t actual_checksum = checksum(bytes.data() + 8, checksum_offset - 8);
        if (stored_checksum != actual_checksum) {
            throw std::runtime_error("SieveCheckpoint::load: checksum mismatch");
        }

        const uint64_t version = read_u64(bytes, cursor, checksum_offset);
        if (version != VERSION) {
            throw std::runtime_error("SieveCheckpoint::load: version mismatch (got " +
                                     std::to_string(version) + ", expected " +
                                     std::to_string(VERSION) + ")");
        }

        SieveCheckpoint checkpoint;
        checkpoint.sq_count = read_u64(bytes, cursor, checksum_offset);
        checkpoint.current_index = read_u32(bytes, cursor, checksum_offset);
        checkpoint.round = std::bit_cast<int32_t>(read_u32(bytes, cursor, checksum_offset));
        checkpoint.batch_target = read_u64(bytes, cursor, checksum_offset);
        checkpoint.candidates_total = read_u64(bytes, cursor, checksum_offset);
        checkpoint.run_fingerprint_lo = read_u64(bytes, cursor, checksum_offset);
        checkpoint.run_fingerprint_hi = read_u64(bytes, cursor, checksum_offset);
        checkpoint.ooc_format_version = read_u64(bytes, cursor, checksum_offset);
        checkpoint.ooc_store_id = read_u64(bytes, cursor, checksum_offset);
        checkpoint.ooc_generation = read_u64(bytes, cursor, checksum_offset);
        checkpoint.ooc_relation_count = read_u64(bytes, cursor, checksum_offset);
        checkpoint.ooc_data_end = read_u64(bytes, cursor, checksum_offset);

        const uint32_t run_n_length = read_u32(bytes, cursor, checksum_offset);
        const uint32_t path_length = read_u32(bytes, cursor, checksum_offset);
        if (run_n_length > MAX_RUN_N_LENGTH) {
            throw std::runtime_error("SieveCheckpoint::load: run N length exceeds limit");
        }
        if (path_length > MAX_PATH_LENGTH) {
            throw std::runtime_error("SieveCheckpoint::load: path length exceeds limit");
        }
        if (cursor > checksum_offset || run_n_length > checksum_offset - cursor ||
            path_length != checksum_offset - cursor - run_n_length) {
            throw std::runtime_error(
                "SieveCheckpoint::load: string lengths do not match exact file length");
        }
        checkpoint.run_n.assign(reinterpret_cast<const char*>(bytes.data() + cursor), run_n_length);
        cursor += run_n_length;
        checkpoint.ooc_base_path.assign(reinterpret_cast<const char*>(bytes.data() + cursor),
                                        path_length);
        cursor += path_length;
        if (cursor != checksum_offset || checksum_cursor != bytes.size()) {
            throw std::runtime_error("SieveCheckpoint::load: unexpected trailing bytes");
        }

        checkpoint.validate("load");
        return checkpoint;
    }

    /// Remove both the published checkpoint and a temporary file left by a
    /// process crash. This intentionally ignores absent files and I/O errors.
    static void remove(const std::string& path) noexcept {
        try {
            std::error_code ignored;
            std::filesystem::remove(std::filesystem::path(path), ignored);
            ignored.clear();
            std::filesystem::remove(std::filesystem::path(temporary_path(path)), ignored);
        } catch (...) {
            // Cleanup is best-effort by contract, including allocation errors.
        }
    }

    /// Raw file presence, independent of validity. Pipeline resume uses this to
    /// fail closed if an on-disk checkpoint exists but cannot be validated.
    [[nodiscard]] static bool exists(const std::string& path) noexcept {
        try {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(std::filesystem::path(path), error);
            if (!error) {
                return status.type() != std::filesystem::file_type::not_found;
            }
            // Any error other than a proven absence must make pipeline resume
            // attempt a strict load and fail closed.
            return error != std::errc::no_such_file_or_directory &&
                   error != std::errc::not_a_directory;
        } catch (...) {
            return true;
        }
    }

    /// Strict validity probe for tests and diagnostics.
    [[nodiscard]] static bool exists_and_valid(const std::string& path) noexcept {
        try {
            (void)load(path);
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    void validate(const char* operation) const {
        const std::string prefix = std::string("SieveCheckpoint::") + operation + ": ";
        if (round < 0) {
            throw std::runtime_error(prefix + "round must be non-negative");
        }
        if (batch_target == 0) {
            throw std::runtime_error(prefix + "batch_target must be positive");
        }
        if (run_n.empty()) {
            throw std::runtime_error(prefix + "run_n must not be empty");
        }
        if (run_n.size() > MAX_RUN_N_LENGTH ||
            run_n.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error(prefix + "run_n exceeds length limit");
        }
        if (run_n.front() < '1' || run_n.front() > '9') {
            throw std::runtime_error(prefix + "run_n must be a canonical positive decimal integer");
        }
        for (const char digit : run_n) {
            if (digit < '0' || digit > '9') {
                throw std::runtime_error(prefix +
                                         "run_n must be a canonical positive decimal integer");
            }
        }
        if (run_fingerprint_lo == 0 || run_fingerprint_hi == 0) {
            throw std::runtime_error(prefix + "run fingerprints must both be nonzero");
        }
        if (ooc_format_version == 0) {
            throw std::runtime_error(prefix + "ooc_format_version must be positive");
        }
        if (ooc_store_id == 0) {
            throw std::runtime_error(prefix + "ooc_store_id must be nonzero");
        }
        if (ooc_generation == 0) {
            throw std::runtime_error(prefix + "ooc_generation must be nonzero");
        }
        if ((ooc_relation_count == 0) != (ooc_data_end == 0)) {
            throw std::runtime_error(prefix + "empty relation prefix must have data_end zero");
        }
        if (ooc_base_path.empty()) {
            throw std::runtime_error(prefix + "ooc_base_path must not be empty");
        }
        if (ooc_base_path.size() > MAX_PATH_LENGTH ||
            ooc_base_path.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error(prefix + "ooc_base_path exceeds length limit");
        }
        if (ooc_base_path.find('\0') != std::string::npos) {
            throw std::runtime_error(prefix + "ooc_base_path contains NUL");
        }
    }
};

} // namespace gnfs::sieve
