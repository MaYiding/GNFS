#include "gnfs/linalg/krylov_sequence_compressed.hpp"

#include "gnfs/linalg/krylov_compress.hpp"
#include "gnfs/util/native_random_access_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gnfs::linalg {
namespace {

constexpr std::uint64_t kIndexEntrySize = 16;

[[noreturn]] void throw_size_overflow(const char* detail) {
    throw std::overflow_error(std::string("KrylovSequenceCompressed: ") + detail);
}

[[nodiscard]] std::uint64_t checked_add(std::uint64_t left, std::uint64_t right,
                                        const char* detail) {
    if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
        throw_size_overflow(detail);
    }
    return left + right;
}

[[nodiscard]] std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right,
                                             const char* detail) {
    if (left != 0 && right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        throw_size_overflow(detail);
    }
    return left * right;
}

[[nodiscard]] std::uint64_t codec_worst_case(std::uint64_t uncompressed_size) {
    const std::uint64_t literal_headers =
        uncompressed_size / KrylovCompressor::MAX_LITERAL_RUN +
        (uncompressed_size % KrylovCompressor::MAX_LITERAL_RUN != 0 ? 1U : 0U);
    return checked_add(checked_add(static_cast<std::uint64_t>(KrylovCompressor::HEADER_BYTES),
                                   uncompressed_size, "compressed chunk bound overflow"),
                       literal_headers, "compressed chunk bound overflow");
}

struct WriterLayout final {
    std::uint64_t chunk_count = 0;
    std::uint64_t total_uncompressed_bytes = 0;
    std::uint64_t max_file_size = 0;
    std::size_t max_chunk_bytes = 0;
    std::size_t index_bytes = 0;
};

[[nodiscard]] WriterLayout checked_layout(std::uint64_t L, std::uint64_t entry_size,
                                          std::uint64_t chunk_blocks) {
    if (L == 0 || entry_size == 0 || chunk_blocks == 0) {
        throw std::invalid_argument(
            "KrylovSequenceCompressed: L/entry_size/chunk_blocks must be > 0");
    }

    const std::uint64_t full_chunks = L / chunk_blocks;
    const std::uint64_t remainder_entries = L % chunk_blocks;
    const std::uint64_t chunk_count =
        checked_add(full_chunks, remainder_entries != 0 ? 1U : 0U, "chunk count overflow");
    const std::uint64_t total_uncompressed =
        checked_multiply(L, entry_size, "uncompressed size overflow");
    const std::uint64_t full_chunk_bytes =
        full_chunks != 0 ? checked_multiply(chunk_blocks, entry_size, "chunk buffer size overflow")
                         : 0;
    const std::uint64_t max_chunk_bytes =
        checked_multiply(std::min(L, chunk_blocks), entry_size, "chunk buffer size overflow");
    const std::uint64_t index_bytes =
        checked_multiply(chunk_count, kIndexEntrySize, "chunk index size overflow");

    std::uint64_t payload_bound =
        full_chunks != 0 ? checked_multiply(full_chunks, codec_worst_case(full_chunk_bytes),
                                            "compressed payload bound overflow")
                         : 0;
    if (remainder_entries != 0) {
        const std::uint64_t remainder_bytes =
            checked_multiply(remainder_entries, entry_size, "last chunk size overflow");
        payload_bound = checked_add(payload_bound, codec_worst_case(remainder_bytes),
                                    "compressed payload bound overflow");
    }
    const std::uint64_t file_bound =
        checked_add(checked_add(static_cast<std::uint64_t>(KrylovSequenceCompressed::HEADER_SIZE),
                                payload_bound, "file size overflow"),
                    index_bytes, "file size overflow");

    const auto size_max = static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)());
    if (static_cast<std::uintmax_t>(max_chunk_bytes) > size_max ||
        static_cast<std::uintmax_t>(index_bytes) > size_max ||
        static_cast<std::uintmax_t>(file_bound) > size_max) {
        throw_size_overflow("layout exceeds size_t");
    }
    constexpr auto native_max =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (file_bound > native_max) {
        throw_size_overflow("layout exceeds native file offset");
    }

    return {
        chunk_count,
        total_uncompressed,
        file_bound,
        static_cast<std::size_t>(max_chunk_bytes),
        static_cast<std::size_t>(index_bytes),
    };
}

void store_u64_le(std::byte* destination, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        destination[byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint64_t load_u64_le(const std::byte* source) noexcept {
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(source[byte]))
                 << (byte * 8U);
    }
    return value;
}

[[nodiscard]] std::array<std::byte, KrylovSequenceCompressed::HEADER_SIZE>
initial_header(std::uint64_t L, std::uint64_t entry_size, std::uint64_t chunk_blocks) noexcept {
    std::array<std::byte, KrylovSequenceCompressed::HEADER_SIZE> header{};
    store_u64_le(header.data(), KrylovSequenceCompressed::MAGIC_UNIQUE);
    store_u64_le(header.data() + 8, KrylovSequenceCompressed::VERSION);
    store_u64_le(header.data() + 16, 1);
    store_u64_le(header.data() + 24, L);
    store_u64_le(header.data() + 32, entry_size);
    store_u64_le(header.data() + 40, chunk_blocks);
    return header;
}

[[nodiscard]] std::string require_path_string(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument("KrylovSequenceCompressed: path must not be null");
    }
    return std::string(path);
}

[[nodiscard]] std::filesystem::path native_path_from_string(const std::string& path) {
#ifdef _WIN32
    std::u8string utf8;
    utf8.reserve(path.size());
    for (const char byte : path) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(utf8);
#else
    return std::filesystem::path(path);
#endif
}

[[nodiscard]] std::string cached_path_string(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t byte : utf8) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
#else
    return path.native();
#endif
}

template <typename Operation> [[nodiscard]] auto validate_file_format(Operation&& operation) {
    try {
        return std::forward<Operation>(operation)();
    } catch (const std::invalid_argument&) {
        throw std::runtime_error("KrylovSequenceCompressed: invalid size metadata");
    } catch (const std::overflow_error&) {
        throw std::runtime_error("KrylovSequenceCompressed: size metadata overflow");
    }
}

} // namespace

struct KrylovSequenceCompressed::State final {
    enum class Mode : std::uint8_t { writing, reading, closed, failed };

    struct ChunkEntry final {
        std::uint64_t file_offset = 0;
        std::uint64_t compressed_size = 0;
    };

    struct CacheNode final {
        std::vector<std::uint8_t> data;
        std::list<std::uint64_t>::iterator list_iterator;
    };

    void fail() noexcept {
        file.close();
        mode = Mode::failed;
    }

    void flush_write_buffer() {
        if (write_buffer_count == 0) {
            return;
        }
        const std::uint64_t uncompressed_size64 =
            checked_multiply(write_buffer_count, entry_size, "buffered chunk size overflow");
        const auto uncompressed_size = static_cast<std::size_t>(uncompressed_size64);
        const auto stride = static_cast<std::size_t>(entry_size);
        auto compressed =
            KrylovCompressor::compress_chunk(write_buffer.data(), uncompressed_size, stride);
        if (static_cast<std::uint64_t>(compressed.size()) > codec_worst_case(uncompressed_size64)) {
            throw std::runtime_error(
                "KrylovSequenceCompressed: codec exceeded its declared size bound");
        }
        file.write_exact_at(write_offset, std::as_bytes(std::span<const std::uint8_t>(compressed)));
        chunk_index.push_back({write_offset, static_cast<std::uint64_t>(compressed.size())});
        write_offset = checked_add(write_offset, static_cast<std::uint64_t>(compressed.size()),
                                   "write offset overflow");
        total_compressed_bytes =
            checked_add(total_compressed_bytes, static_cast<std::uint64_t>(compressed.size()),
                        "compressed byte count overflow");
        write_buffer_count = 0;
        std::fill(write_buffer.begin(), write_buffer.end(), 0);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& load_chunk_locked(std::uint64_t chunk_id) {
        const auto found = cache_map.find(chunk_id);
        if (found != cache_map.end()) {
            cache_hits.fetch_add(1, std::memory_order_relaxed);
            cache_list.splice(cache_list.begin(), cache_list, found->second.list_iterator);
            found->second.list_iterator = cache_list.begin();
            return found->second.data;
        }

        cache_misses.fetch_add(1, std::memory_order_relaxed);
        if (chunk_id >= chunk_index.size()) {
            throw std::out_of_range(
                "KrylovSequenceCompressed::read_entry: chunk index out of range");
        }
        const ChunkEntry& entry = chunk_index[static_cast<std::size_t>(chunk_id)];
        std::vector<std::uint8_t> compressed(static_cast<std::size_t>(entry.compressed_size));
        file.read_exact_at(entry.file_offset,
                           std::as_writable_bytes(std::span<std::uint8_t>(compressed)));

        const std::uint64_t first_entry =
            checked_multiply(chunk_id, chunk_blocks, "chunk entry offset overflow");
        const std::uint64_t entries_in_chunk = std::min(chunk_blocks, L - first_entry);
        const std::uint64_t uncompressed_size64 =
            checked_multiply(entries_in_chunk, entry_size, "decompressed chunk size overflow");
        auto uncompressed_size = static_cast<std::size_t>(uncompressed_size64);
        std::vector<std::uint8_t> decompressed(uncompressed_size);
        if (!KrylovCompressor::decompress_chunk(compressed.data(), compressed.size(),
                                                decompressed.data(), decompressed.size(),
                                                static_cast<std::size_t>(entry_size))) {
            throw std::runtime_error(
                "KrylovSequenceCompressed: compressed chunk validation failed");
        }

        cache_list.push_front(chunk_id);
        try {
            const auto [inserted, did_insert] =
                cache_map.emplace(chunk_id, CacheNode{std::move(decompressed), cache_list.begin()});
            if (!did_insert) {
                cache_list.pop_front();
                throw std::logic_error("KrylovSequenceCompressed: duplicate cache insertion");
            }
            cache_bytes = checked_add(static_cast<std::uint64_t>(cache_bytes),
                                      static_cast<std::uint64_t>(inserted->second.data.size()),
                                      "cache byte count overflow");

            while (cache_bytes > cache_limit_bytes && cache_list.size() > 1) {
                const std::uint64_t victim_id = cache_list.back();
                const auto victim = cache_map.find(victim_id);
                if (victim == cache_map.end()) {
                    throw std::logic_error(
                        "KrylovSequenceCompressed: cache index invariant failed");
                }
                cache_bytes -= victim->second.data.size();
                cache_map.erase(victim);
                cache_list.pop_back();
            }
            return inserted->second.data;
        } catch (...) {
            if (!cache_list.empty() && cache_list.front() == chunk_id &&
                cache_map.find(chunk_id) == cache_map.end()) {
                cache_list.pop_front();
            }
            throw;
        }
    }

    gnfs::util::NativeRandomAccessFile file;
    Mode mode = Mode::closed;
    std::uint64_t L = 0;
    std::uint64_t entry_size = 0;
    std::uint64_t chunk_blocks = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t total_uncompressed_bytes = 0;
    std::uint64_t total_compressed_bytes = 0;
    std::uint64_t entries_written = 0;
    std::uint64_t write_buffer_count = 0;
    std::uint64_t write_offset = HEADER_SIZE;
    std::vector<std::uint8_t> write_buffer;
    std::vector<std::byte> index_buffer;
    std::vector<ChunkEntry> chunk_index;
    std::mutex cache_mutex;
    std::list<std::uint64_t> cache_list;
    std::unordered_map<std::uint64_t, CacheNode> cache_map;
    std::uint64_t cache_limit_bytes = DEFAULT_CACHE_LIMIT_BYTES;
    std::uint64_t cache_bytes = 0;
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};
};

namespace {

struct DecodedIndexEntry final {
    std::uint64_t file_offset = 0;
    std::uint64_t compressed_size = 0;
};

[[nodiscard]] DecodedIndexEntry read_index_entry(const gnfs::util::NativeRandomAccessFile& file,
                                                 std::uint64_t offset) {
    std::array<std::byte, kIndexEntrySize> encoded{};
    file.read_exact_at(offset, encoded);
    return {load_u64_le(encoded.data()), load_u64_le(encoded.data() + 8)};
}

} // namespace

KrylovSequenceCompressed::KrylovSequenceCompressed() noexcept = default;

KrylovSequenceCompressed::KrylovSequenceCompressed(const std::string& path, std::uint64_t L,
                                                   std::uint64_t entry_size,
                                                   std::uint64_t chunk_blocks,
                                                   std::size_t cache_limit_bytes)
    : filesystem_path_(native_path_from_string(path)), path_(path) {
    initialize_writer(L, entry_size, chunk_blocks, cache_limit_bytes);
}

KrylovSequenceCompressed::KrylovSequenceCompressed(const char* path, std::uint64_t L,
                                                   std::uint64_t entry_size,
                                                   std::uint64_t chunk_blocks,
                                                   std::size_t cache_limit_bytes)
    : KrylovSequenceCompressed(require_path_string(path), L, entry_size, chunk_blocks,
                               cache_limit_bytes) {}

KrylovSequenceCompressed::KrylovSequenceCompressed(const std::filesystem::path& path,
                                                   std::uint64_t L, std::uint64_t entry_size,
                                                   std::uint64_t chunk_blocks,
                                                   std::size_t cache_limit_bytes)
    : filesystem_path_(path), path_(cached_path_string(path)) {
    initialize_writer(L, entry_size, chunk_blocks, cache_limit_bytes);
}

KrylovSequenceCompressed::~KrylovSequenceCompressed() {
    if (state_ == nullptr || state_->mode != State::Mode::writing) {
        return;
    }
    if (state_->entries_written != state_->L) {
        state_->fail();
        return;
    }
    try {
        close();
    } catch (...) {
        // close() has already closed the native handle and removed any
        // uncertain publication. Destructors must not throw.
    }
}

KrylovSequenceCompressed::KrylovSequenceCompressed(KrylovSequenceCompressed&& other) noexcept
    : filesystem_path_(std::move(other.filesystem_path_)), path_(std::move(other.path_)),
      state_(std::move(other.state_)) {
    other.filesystem_path_.clear();
    other.path_.clear();
}

void KrylovSequenceCompressed::initialize_writer(std::uint64_t L, std::uint64_t entry_size,
                                                 std::uint64_t chunk_blocks,
                                                 std::size_t cache_limit_bytes) {
    const WriterLayout layout = checked_layout(L, entry_size, chunk_blocks);
    auto state = std::make_unique<State>();
    state->L = L;
    state->entry_size = entry_size;
    state->chunk_blocks = chunk_blocks;
    state->chunk_count = layout.chunk_count;
    state->total_uncompressed_bytes = layout.total_uncompressed_bytes;
    state->cache_limit_bytes = cache_limit_bytes;
    state->write_buffer.assign(layout.max_chunk_bytes, 0);
    state->index_buffer.assign(layout.index_bytes, std::byte{0});
    state->chunk_index.reserve(static_cast<std::size_t>(layout.chunk_count));

    state->file = gnfs::util::NativeRandomAccessFile::create_truncated(filesystem_path_);
    try {
        const auto header = initial_header(L, entry_size, chunk_blocks);
        state->file.write_exact_at(0, header);
        state->mode = State::Mode::writing;
    } catch (...) {
        state->file.close();
        std::error_code ignored;
        (void)std::filesystem::remove(filesystem_path_, ignored);
        throw;
    }
    state_ = std::move(state);
}

KrylovSequenceCompressed KrylovSequenceCompressed::open_readonly(const std::string& path) {
    return open_readonly(path, DEFAULT_CACHE_LIMIT_BYTES);
}

KrylovSequenceCompressed KrylovSequenceCompressed::open_readonly(const char* path) {
    return open_readonly(require_path_string(path), DEFAULT_CACHE_LIMIT_BYTES);
}

KrylovSequenceCompressed
KrylovSequenceCompressed::open_readonly(const std::filesystem::path& path) {
    return open_readonly(path, DEFAULT_CACHE_LIMIT_BYTES);
}

KrylovSequenceCompressed KrylovSequenceCompressed::open_readonly(const std::string& path,
                                                                 std::size_t cache_limit_bytes) {
    return open_readonly_prepared(native_path_from_string(path), path, cache_limit_bytes);
}

KrylovSequenceCompressed KrylovSequenceCompressed::open_readonly(const char* path,
                                                                 std::size_t cache_limit_bytes) {
    return open_readonly(require_path_string(path), cache_limit_bytes);
}

KrylovSequenceCompressed KrylovSequenceCompressed::open_readonly(const std::filesystem::path& path,
                                                                 std::size_t cache_limit_bytes) {
    return open_readonly_prepared(path, cached_path_string(path), cache_limit_bytes);
}

KrylovSequenceCompressed
KrylovSequenceCompressed::open_readonly_prepared(std::filesystem::path filesystem_path,
                                                 std::string path, std::size_t cache_limit_bytes) {
    KrylovSequenceCompressed result;
    result.filesystem_path_ = std::move(filesystem_path);
    result.path_ = std::move(path);
    auto state = std::make_unique<State>();
    state->file = gnfs::util::NativeRandomAccessFile::open_read_only(result.filesystem_path_);

    const std::uint64_t file_size = state->file.size();
    if (file_size < HEADER_SIZE) {
        throw std::runtime_error("KrylovSequenceCompressed: file is shorter than its header");
    }
    std::array<std::byte, HEADER_SIZE> header{};
    state->file.read_exact_at(0, header);

    const std::uint64_t magic = load_u64_le(header.data());
    const std::uint64_t version = load_u64_le(header.data() + 8);
    const std::uint64_t incomplete_flag = load_u64_le(header.data() + 16);
    if (magic != MAGIC_UNIQUE) {
        throw std::runtime_error("KrylovSequenceCompressed: bad magic");
    }
    if (version != VERSION) {
        throw std::runtime_error("KrylovSequenceCompressed: version mismatch");
    }
    if (incomplete_flag != 0) {
        throw std::runtime_error("KrylovSequenceCompressed: INCOMPLETE file");
    }

    const std::uint64_t L = load_u64_le(header.data() + 24);
    const std::uint64_t entry_size = load_u64_le(header.data() + 32);
    const std::uint64_t chunk_blocks = load_u64_le(header.data() + 40);
    const std::uint64_t chunk_count = load_u64_le(header.data() + 48);
    const std::uint64_t index_offset = load_u64_le(header.data() + 56);
    const WriterLayout layout =
        validate_file_format([&] { return checked_layout(L, entry_size, chunk_blocks); });
    if (chunk_count != layout.chunk_count) {
        throw std::runtime_error("KrylovSequenceCompressed: chunk count does not match dimensions");
    }
    if (index_offset < HEADER_SIZE) {
        throw std::runtime_error("KrylovSequenceCompressed: chunk index overlaps the header");
    }
    const std::uint64_t index_end = validate_file_format([&] {
        return checked_add(index_offset, static_cast<std::uint64_t>(layout.index_bytes),
                           "chunk index extent overflow");
    });
    if (index_end != file_size) {
        throw std::runtime_error("KrylovSequenceCompressed: chunk index must end at file size");
    }

    const auto validate_index = [&](std::vector<State::ChunkEntry>* destination) {
        std::uint64_t expected_payload_offset = HEADER_SIZE;
        std::uint64_t remaining_entries = L;
        std::uint64_t compressed_bytes = 0;
        for (std::uint64_t chunk = 0; chunk < chunk_count; ++chunk) {
            const std::uint64_t encoded_offset = validate_file_format([&] {
                return checked_add(
                    index_offset,
                    checked_multiply(chunk, kIndexEntrySize, "chunk index entry offset overflow"),
                    "chunk index entry offset overflow");
            });
            const DecodedIndexEntry entry = read_index_entry(state->file, encoded_offset);
            if (entry.file_offset != expected_payload_offset) {
                throw std::runtime_error(
                    "KrylovSequenceCompressed: chunk payloads are not contiguous");
            }
            const std::uint64_t entries_in_chunk = std::min(chunk_blocks, remaining_entries);
            const std::uint64_t uncompressed_size = validate_file_format([&] {
                return checked_multiply(entries_in_chunk, entry_size,
                                        "chunk payload size overflow");
            });
            if (entry.compressed_size < KrylovCompressor::HEADER_BYTES ||
                entry.compressed_size >
                    validate_file_format([&] { return codec_worst_case(uncompressed_size); })) {
                throw std::runtime_error(
                    "KrylovSequenceCompressed: compressed chunk size is invalid");
            }
            expected_payload_offset = validate_file_format([&] {
                return checked_add(expected_payload_offset, entry.compressed_size,
                                   "chunk payload extent overflow");
            });
            if (expected_payload_offset > index_offset) {
                throw std::runtime_error(
                    "KrylovSequenceCompressed: chunk payload overlaps the index");
            }
            compressed_bytes = validate_file_format([&] {
                return checked_add(compressed_bytes, entry.compressed_size,
                                   "compressed byte count overflow");
            });
            if (destination != nullptr) {
                (*destination)[static_cast<std::size_t>(chunk)] = {entry.file_offset,
                                                                   entry.compressed_size};
            }
            remaining_entries -= entries_in_chunk;
        }
        if (remaining_entries != 0 || expected_payload_offset != index_offset) {
            throw std::runtime_error(
                "KrylovSequenceCompressed: chunk payload extent does not match the index");
        }
        return compressed_bytes;
    };

    // Validate once without header-controlled allocation, then validate again
    // while copying. The second pass must never accept unvalidated metadata if
    // another process modifies the shared scratch file between passes.
    (void)validate_index(nullptr);
    state->chunk_index.resize(static_cast<std::size_t>(chunk_count));
    state->total_compressed_bytes = validate_index(&state->chunk_index);

    state->mode = State::Mode::reading;
    state->L = L;
    state->entry_size = entry_size;
    state->chunk_blocks = chunk_blocks;
    state->chunk_count = chunk_count;
    state->index_offset = index_offset;
    state->total_uncompressed_bytes = layout.total_uncompressed_bytes;
    state->cache_limit_bytes = cache_limit_bytes;
    result.state_ = std::move(state);
    return result;
}

void KrylovSequenceCompressed::write_entry(std::uint64_t k, std::span<const std::byte> source) {
    if (state_ == nullptr || state_->mode != State::Mode::writing || !state_->file.is_open()) {
        throw std::logic_error("KrylovSequenceCompressed::write_entry: writer is closed");
    }
    if (source.size() != state_->entry_size) {
        throw std::invalid_argument("KrylovSequenceCompressed::write_entry: entry size mismatch");
    }
    if (k >= state_->L) {
        throw std::out_of_range("KrylovSequenceCompressed::write_entry: k >= L");
    }
    if (k != state_->entries_written) {
        throw std::logic_error("KrylovSequenceCompressed::write_entry: out-of-order write");
    }

    try {
        if (state_->write_buffer_count == state_->chunk_blocks) {
            state_->flush_write_buffer();
        }
        auto* destination =
            state_->write_buffer.data() +
            static_cast<std::size_t>(state_->write_buffer_count * state_->entry_size);
        std::memcpy(destination, source.data(), source.size());
        ++state_->write_buffer_count;
        ++state_->entries_written;
    } catch (...) {
        state_->fail();
        std::error_code ignored;
        (void)std::filesystem::remove(filesystem_path_, ignored);
        throw;
    }
}

void KrylovSequenceCompressed::read_entry(std::uint64_t k, std::span<std::byte> destination) {
    if (state_ == nullptr) {
        throw std::logic_error("KrylovSequenceCompressed::read_entry: reader is closed");
    }
    std::lock_guard lock(state_->cache_mutex);
    if (state_->mode != State::Mode::reading || !state_->file.is_open()) {
        throw std::logic_error("KrylovSequenceCompressed::read_entry: reader is closed");
    }
    if (destination.size() != state_->entry_size) {
        throw std::invalid_argument("KrylovSequenceCompressed::read_entry: entry size mismatch");
    }
    if (k >= state_->L) {
        throw std::out_of_range("KrylovSequenceCompressed::read_entry: k >= L");
    }
    try {
        const std::uint64_t chunk_id = k / state_->chunk_blocks;
        const std::uint64_t slot = k % state_->chunk_blocks;
        const auto& chunk = state_->load_chunk_locked(chunk_id);
        const auto source_offset = static_cast<std::size_t>(slot * state_->entry_size);
        std::memcpy(destination.data(), chunk.data() + source_offset, destination.size());
    } catch (...) {
        state_->fail();
        throw;
    }
}

std::uint8_t* KrylovSequenceCompressed::write_at(std::uint64_t k) {
    if (state_ == nullptr || state_->mode != State::Mode::writing || !state_->file.is_open()) {
        throw std::logic_error("KrylovSequenceCompressed::write_at: writer is closed");
    }
    if (k >= state_->L) {
        throw std::out_of_range("KrylovSequenceCompressed::write_at: k >= L");
    }
    if (k != state_->entries_written) {
        throw std::logic_error("KrylovSequenceCompressed::write_at: out-of-order write");
    }
    try {
        if (state_->write_buffer_count == state_->chunk_blocks) {
            state_->flush_write_buffer();
        }
        auto* slot = state_->write_buffer.data() +
                     static_cast<std::size_t>(state_->write_buffer_count * state_->entry_size);
        ++state_->write_buffer_count;
        ++state_->entries_written;
        return slot;
    } catch (...) {
        state_->fail();
        throw;
    }
}

const std::uint8_t* KrylovSequenceCompressed::read_at(std::uint64_t k) {
    if (state_ == nullptr || state_->mode != State::Mode::reading || !state_->file.is_open()) {
        throw std::logic_error("KrylovSequenceCompressed::read_at: reader is closed");
    }
    if (k >= state_->L) {
        throw std::out_of_range("KrylovSequenceCompressed::read_at: k >= L");
    }

    std::lock_guard lock(state_->cache_mutex);
    try {
        const std::uint64_t chunk_id = k / state_->chunk_blocks;
        const std::uint64_t slot = k % state_->chunk_blocks;
        const auto& chunk = state_->load_chunk_locked(chunk_id);
        return chunk.data() + static_cast<std::size_t>(slot * state_->entry_size);
    } catch (...) {
        state_->fail();
        throw;
    }
}

void KrylovSequenceCompressed::close() {
    if (state_ == nullptr || state_->mode == State::Mode::closed ||
        state_->mode == State::Mode::failed) {
        return;
    }
    if (state_->mode == State::Mode::reading) {
        state_->file.close();
        state_->mode = State::Mode::closed;
        return;
    }
    if (state_->entries_written != state_->L) {
        state_->fail();
        throw std::logic_error(
            "KrylovSequenceCompressed::close: writer did not receive exactly L entries");
    }

    try {
        state_->flush_write_buffer();
        if (state_->chunk_index.size() != state_->chunk_count) {
            throw std::logic_error("KrylovSequenceCompressed::close: chunk count invariant failed");
        }

        state_->index_offset = state_->write_offset;
        for (std::size_t index = 0; index < state_->chunk_index.size(); ++index) {
            store_u64_le(state_->index_buffer.data() + index * kIndexEntrySize,
                         state_->chunk_index[index].file_offset);
            store_u64_le(state_->index_buffer.data() + index * kIndexEntrySize + 8,
                         state_->chunk_index[index].compressed_size);
        }
        state_->file.write_exact_at(state_->index_offset, state_->index_buffer);

        std::array<std::byte, 16> header_tail{};
        store_u64_le(header_tail.data(), state_->chunk_count);
        store_u64_le(header_tail.data() + 8, state_->index_offset);
        state_->file.write_exact_at(48, header_tail);
        state_->file.sync();

        std::array<std::byte, 8> complete{};
        store_u64_le(complete.data(), 0);
        state_->file.write_exact_at(16, complete);
        state_->file.sync();
        state_->file.close();
        state_->mode = State::Mode::closed;
    } catch (...) {
        state_->fail();
        std::error_code ignored;
        (void)std::filesystem::remove(filesystem_path_, ignored);
        throw;
    }
}

void KrylovSequenceCompressed::remove_file() noexcept {
    if (state_ != nullptr) {
        state_->file.close();
        state_->mode = State::Mode::closed;
    }
    if (!filesystem_path_.empty()) {
        std::error_code ignored;
        (void)std::filesystem::remove(filesystem_path_, ignored);
    }
}

std::uint64_t KrylovSequenceCompressed::length() const noexcept {
    return state_ != nullptr ? state_->L : 0;
}

std::uint64_t KrylovSequenceCompressed::entry_size() const noexcept {
    return state_ != nullptr ? state_->entry_size : 0;
}

std::uint64_t KrylovSequenceCompressed::chunk_blocks() const noexcept {
    return state_ != nullptr ? state_->chunk_blocks : 0;
}

std::uint64_t KrylovSequenceCompressed::chunk_count() const noexcept {
    return state_ != nullptr ? state_->chunk_count : 0;
}

std::uint64_t KrylovSequenceCompressed::total_compressed_bytes() const noexcept {
    return state_ != nullptr ? state_->total_compressed_bytes : 0;
}

std::uint64_t KrylovSequenceCompressed::total_uncompressed_bytes() const noexcept {
    return state_ != nullptr ? state_->total_uncompressed_bytes : 0;
}

bool KrylovSequenceCompressed::is_open() const noexcept {
    return state_ != nullptr && state_->file.is_open() &&
           (state_->mode == State::Mode::writing || state_->mode == State::Mode::reading);
}

const std::string& KrylovSequenceCompressed::path() const noexcept {
    return path_;
}

const std::filesystem::path& KrylovSequenceCompressed::filesystem_path() const noexcept {
    return filesystem_path_;
}

std::uint64_t KrylovSequenceCompressed::cache_hits() const noexcept {
    return state_ != nullptr ? state_->cache_hits.load(std::memory_order_relaxed) : 0;
}

std::uint64_t KrylovSequenceCompressed::cache_misses() const noexcept {
    return state_ != nullptr ? state_->cache_misses.load(std::memory_order_relaxed) : 0;
}

} // namespace gnfs::linalg
