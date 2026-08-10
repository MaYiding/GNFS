#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace gnfs::linalg {

/// Out-of-core, chunk-compressed Krylov sequence storage.
///
/// Writers accept entries in strict index order and publish a file only after
/// exactly L complete entries have been copied in. Readers validate the fixed
/// header, index extent, and every compressed payload range before allocating
/// header-controlled storage. The v1 wire representation remains the existing
/// little-endian GNFSKRYZ scratch format on supported hosts.
///
/// The object is move-only. Do not race write, move, close, or removal with any
/// other operation. read_entry() copies while the cache is protected and may
/// be called concurrently with other read_entry() calls on the same reader.
class KrylovSequenceCompressed {
public:
    static constexpr std::uint64_t MAGIC = 0x5A594B52534647ULL;
    static constexpr std::uint64_t MAGIC_UNIQUE = 0x5A594B52535A4E47ULL;
    static constexpr std::uint64_t VERSION = 1;
    static constexpr std::size_t HEADER_SIZE = 64;
    static constexpr std::uint64_t DEFAULT_CHUNK_BLOCKS = 64;
    static constexpr std::size_t DEFAULT_CACHE_LIMIT_BYTES = 64ULL * 1024ULL * 1024ULL;

    KrylovSequenceCompressed() noexcept;

    KrylovSequenceCompressed(const std::string& path, std::uint64_t L, std::uint64_t entry_size,
                             std::uint64_t chunk_blocks = DEFAULT_CHUNK_BLOCKS,
                             std::size_t cache_limit_bytes = DEFAULT_CACHE_LIMIT_BYTES);
    KrylovSequenceCompressed(const char* path, std::uint64_t L, std::uint64_t entry_size,
                             std::uint64_t chunk_blocks = DEFAULT_CHUNK_BLOCKS,
                             std::size_t cache_limit_bytes = DEFAULT_CACHE_LIMIT_BYTES);
    KrylovSequenceCompressed(const std::filesystem::path& path, std::uint64_t L,
                             std::uint64_t entry_size,
                             std::uint64_t chunk_blocks = DEFAULT_CHUNK_BLOCKS,
                             std::size_t cache_limit_bytes = DEFAULT_CACHE_LIMIT_BYTES);

    /// Preserve the v0.1 RAII contract for a writer that received exactly L
    /// entries. An underfilled or failed writer is abandoned as INCOMPLETE.
    ~KrylovSequenceCompressed();

    KrylovSequenceCompressed(const KrylovSequenceCompressed&) = delete;
    KrylovSequenceCompressed& operator=(const KrylovSequenceCompressed&) = delete;
    KrylovSequenceCompressed(KrylovSequenceCompressed&&) noexcept;
    KrylovSequenceCompressed& operator=(KrylovSequenceCompressed&&) = delete;

    static KrylovSequenceCompressed open_readonly(const std::string& path);
    static KrylovSequenceCompressed open_readonly(const char* path);
    static KrylovSequenceCompressed open_readonly(const std::filesystem::path& path);

    static KrylovSequenceCompressed open_readonly(const std::string& path,
                                                  std::size_t cache_limit_bytes);
    static KrylovSequenceCompressed open_readonly(const char* path, std::size_t cache_limit_bytes);
    static KrylovSequenceCompressed open_readonly(const std::filesystem::path& path,
                                                  std::size_t cache_limit_bytes);

    /// Copy one complete entry into the next sequential writer slot.
    void write_entry(std::uint64_t k, std::span<const std::byte> source);

    /// Copy one complete entry out of a validated reader cache.
    ///
    /// All fallible I/O and decompression happens before destination is
    /// modified, so destination remains unchanged when this operation throws.
    void read_entry(std::uint64_t k, std::span<std::byte> destination);

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void write_entry(std::uint64_t k, const T& value) {
        write_entry(k, std::as_bytes(std::span<const T>(&value, 1)));
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] T read_entry(std::uint64_t k) {
        std::array<std::byte, sizeof(T)> bytes{};
        read_entry(k, bytes);
        return std::bit_cast<T>(bytes);
    }

    /// Legacy reservation API. The returned pointer must be filled before the
    /// next writer operation. A reservation cannot prove that the caller
    /// actually copied a complete entry.
    [[nodiscard]] std::uint8_t* write_at(std::uint64_t k);

    /// Legacy cache-pointer API. The pointer is invalidated by the next cache
    /// mutation, move, close, removal, or destruction. It is not concurrency
    /// safe.
    [[nodiscard]] const std::uint8_t* read_at(std::uint64_t k);

    template <typename T> [[nodiscard]] T* write_at_typed(std::uint64_t k) {
        if (sizeof(T) != entry_size()) {
            throw std::invalid_argument(
                "KrylovSequenceCompressed::write_at_typed: entry size mismatch");
        }
        return reinterpret_cast<T*>(write_at(k));
    }

    template <typename T> [[nodiscard]] const T* read_at_typed(std::uint64_t k) {
        if (sizeof(T) != entry_size()) {
            throw std::invalid_argument(
                "KrylovSequenceCompressed::read_at_typed: entry size mismatch");
        }
        return reinterpret_cast<const T*>(read_at(k));
    }

    /// Commit a complete writer or close a reader. An underfilled writer is
    /// terminally closed and rejected without publishing the completion flag.
    void close();
    void remove_file() noexcept;

    [[nodiscard]] std::uint64_t length() const noexcept;
    [[nodiscard]] std::uint64_t entry_size() const noexcept;
    [[nodiscard]] std::uint64_t chunk_blocks() const noexcept;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept;
    [[nodiscard]] std::uint64_t total_compressed_bytes() const noexcept;
    [[nodiscard]] std::uint64_t total_uncompressed_bytes() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::filesystem::path& filesystem_path() const noexcept;
    [[nodiscard]] std::uint64_t cache_hits() const noexcept;
    [[nodiscard]] std::uint64_t cache_misses() const noexcept;

private:
    struct State;

    void initialize_writer(std::uint64_t L, std::uint64_t entry_size, std::uint64_t chunk_blocks,
                           std::size_t cache_limit_bytes);
    [[nodiscard]] static KrylovSequenceCompressed
    open_readonly_prepared(std::filesystem::path filesystem_path, std::string path,
                           std::size_t cache_limit_bytes);

    std::filesystem::path filesystem_path_;
    std::string path_;
    std::unique_ptr<State> state_;
};

} // namespace gnfs::linalg
