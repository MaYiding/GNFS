#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include "distributed_sieve_work_identity_codec_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gnfs::sieve {
namespace distributed_sieve_protocol_detail {

constexpr std::array<char, 8> RECORD_MAGIC{'G', 'N', 'F', 'S', 'D', 'S', 'P', '1'};
constexpr std::string_view RECORD_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-RECORD-V1";
constexpr std::string_view WORK_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-WORK-V1";
constexpr std::string_view RANDOM_SEED_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-RANDOM-V1";
constexpr uint32_t FRAME_PREFIX_SIZE = 24;
constexpr uint32_t DIGEST_SIZE = static_cast<uint32_t>(util::SHA256_DIGEST_BYTES);
constexpr uint32_t MINIMUM_RECORD_SIZE = FRAME_PREFIX_SIZE + DIGEST_SIZE;

[[nodiscard]] constexpr DistributedSieveProtocolStatus
failure(DistributedSieveProtocolError error,
        uint64_t byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {error, byte_offset, element_index};
}

[[nodiscard]] constexpr bool digest_is_zero(const util::Sha256Digest& digest) noexcept {
    for (const std::byte byte : digest.bytes) {
        if (byte != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool wave_id_is_zero(const WaveIdV1& wave_id) noexcept {
    for (const std::byte byte : wave_id.bytes) {
        if (byte != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool lease_id_is_zero(const LeaseIdV1& lease_id) noexcept {
    return lease_id.limbs[0] == 0 && lease_id.limbs[1] == 0;
}

[[nodiscard]] constexpr bool native_identity_is_valid(const NativeIdentityV1& identity) noexcept {
    return identity.object != 0;
}

[[nodiscard]] constexpr char fold_ascii_case(char character) noexcept {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character + static_cast<char>('a' - 'A'))
               : character;
}

[[nodiscard]] bool ascii_casefold_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_ascii_case(left[index]) != fold_ascii_case(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool reserved_windows_device_stem(std::string_view stem) noexcept {
    if (ascii_casefold_equal(stem, "con") || ascii_casefold_equal(stem, "prn") ||
        ascii_casefold_equal(stem, "aux") || ascii_casefold_equal(stem, "nul")) {
        return true;
    }
    if (stem.size() != 4 || stem.back() < '1' || stem.back() > '9') {
        return false;
    }
    return ascii_casefold_equal(stem.substr(0, 3), "com") ||
           ascii_casefold_equal(stem.substr(0, 3), "lpt");
}

[[nodiscard]] bool canonical_artifact_stem(std::string_view stem) noexcept {
    if (stem.empty() || stem.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES ||
        reserved_windows_device_stem(stem)) {
        return false;
    }
    for (const char character : stem) {
        const auto byte = static_cast<unsigned char>(character);
        const bool alpha_numeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                   (byte >= '0' && byte <= '9');
        if (!alpha_numeric && byte != '_' && byte != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::size_t worker_attempt_stem_suffix_bytes() noexcept {
    return DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() +
           DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;
}

[[nodiscard]] bool worker_attempt_stem_is_representable(std::string_view chunk_stem) noexcept {
    return canonical_artifact_stem(chunk_stem) &&
           chunk_stem.size() <= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES -
                                    worker_attempt_stem_suffix_bytes();
}

[[nodiscard]] bool worker_attempt_relative_stem_matches(std::string_view chunk_stem,
                                                        uint32_t attempt_ordinal,
                                                        std::string_view candidate) noexcept {
    if (attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        !worker_attempt_stem_is_representable(chunk_stem) ||
        candidate.size() != chunk_stem.size() + worker_attempt_stem_suffix_bytes() ||
        candidate.substr(0, chunk_stem.size()) != chunk_stem ||
        candidate.substr(chunk_stem.size(), DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size()) !=
            DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1) {
        return false;
    }
    const std::size_t digits_offset =
        chunk_stem.size() + DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size();
    return candidate[digits_offset] == static_cast<char>('0' + attempt_ordinal / 10U) &&
           candidate[digits_offset + 1U] == static_cast<char>('0' + attempt_ordinal % 10U);
}

[[nodiscard]] bool canonical_integer(std::string_view value) noexcept {
    if (value.empty() || value.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) {
        return false;
    }
    std::size_t offset = 0;
    if (value.front() == '-') {
        if (value.size() == 1) {
            return false;
        }
        offset = 1;
    }
    if (value[offset] == '0') {
        return value.size() == 1;
    }
    if (value[offset] < '1' || value[offset] > '9') {
        return false;
    }
    for (++offset; offset < value.size(); ++offset) {
        if (value[offset] < '0' || value[offset] > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool finite_binary64(uint64_t bits) noexcept {
    return std::isfinite(std::bit_cast<double>(bits));
}

[[nodiscard]] bool positive_finite_binary64(uint64_t bits) noexcept {
    const double value = std::bit_cast<double>(bits);
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] constexpr bool positive_normal_square_binary64(uint64_t bits) noexcept {
    // A positive binary64 value has a normal, finite exact square for every
    // rounding/FTZ environment precisely within this exponent band:
    // [2^-511, nextafter(2^512, 0)]. Comparing encoded exponents keeps work
    // identity validation independent of mutable floating-point state.
    constexpr uint64_t sign_mask = uint64_t{1} << 63;
    constexpr uint64_t exponent_mask = 0x7ffU;
    constexpr uint64_t minimum_biased_exponent = 512;
    constexpr uint64_t maximum_biased_exponent = 1534;
    const uint64_t exponent = (bits >> 52) & exponent_mask;
    return (bits & sign_mask) == 0 && exponent >= minimum_biased_exponent &&
           exponent <= maximum_biased_exponent;
}

class Writer final {
public:
    explicit Writer(uint32_t limit = DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES) noexcept
        : limit_(limit) {}

    void put_u8(uint8_t value) {
        if (!reserve(1)) {
            return;
        }
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void put_u16(uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_i32(int32_t value) {
        put_u32(static_cast<uint32_t>(value));
    }

    void put_i64(int64_t value) {
        put_u64(static_cast<uint64_t>(value));
    }

    void put_bool(bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_bytes(std::span<const std::byte> bytes) {
        if (!reserve(bytes.size())) {
            return;
        }
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void put_digest(const util::Sha256Digest& digest) {
        put_bytes(digest.bytes);
    }

    void put_wave_id(const WaveIdV1& wave_id) {
        put_bytes(wave_id.bytes);
    }

    void put_string(std::string_view value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            set_error(DistributedSieveProtocolError::integer_out_of_range);
            return;
        }
        put_u32(static_cast<uint32_t>(value.size()));
        put_bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] bool patch_u32(std::size_t offset, uint32_t value) noexcept {
        if (offset > bytes_.size() || bytes_.size() - offset < sizeof(uint32_t)) {
            set_error(DistributedSieveProtocolError::invalid_value);
            return false;
        }
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes_[offset++] = static_cast<std::byte>(static_cast<uint8_t>(value >> shift));
        }
        return true;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept {
        return bytes_;
    }

    [[nodiscard]] DistributedSieveProtocolStatus status() const noexcept {
        return status_;
    }

private:
    [[nodiscard]] bool reserve(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (amount > limit_ || bytes_.size() > limit_ - amount) {
            set_error(DistributedSieveProtocolError::output_too_large);
            return false;
        }
        return true;
    }

    void set_error(DistributedSieveProtocolError error) noexcept {
        if (status_) {
            status_ = failure(error, bytes_.size());
        }
    }

    std::vector<std::byte> bytes_;
    std::size_t limit_;
    DistributedSieveProtocolStatus status_;
};

class Reader final {
public:
    Reader(std::span<const std::byte> bytes, uint64_t base_offset) noexcept
        : bytes_(bytes), base_offset_(base_offset) {}

    [[nodiscard]] bool get_u8(uint8_t& value) noexcept {
        if (!take(1)) {
            return false;
        }
        value = std::to_integer<uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool get_u16(uint16_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 16; shift += 8) {
            uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<uint16_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool get_u32(uint32_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<uint32_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool get_u64(uint64_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            uint8_t byte = 0;
            if (!get_u8(byte)) {
                return false;
            }
            value |= static_cast<uint64_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool get_i32(int32_t& value) noexcept {
        uint32_t bits = 0;
        if (!get_u32(bits)) {
            return false;
        }
        value = std::bit_cast<int32_t>(bits);
        return true;
    }

    [[nodiscard]] bool get_i64(int64_t& value) noexcept {
        uint64_t bits = 0;
        if (!get_u64(bits)) {
            return false;
        }
        value = std::bit_cast<int64_t>(bits);
        return true;
    }

    [[nodiscard]] bool get_bool(bool& value) noexcept {
        const std::size_t start = offset_;
        uint8_t raw = 0;
        if (!get_u8(raw)) {
            return false;
        }
        if (raw > 1) {
            set_error(DistributedSieveProtocolError::invalid_boolean, start);
            return false;
        }
        value = raw != 0;
        return true;
    }

    [[nodiscard]] bool get_bytes(std::span<std::byte> output) noexcept {
        if (!take(output.size())) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), output.size(),
                    output.begin());
        offset_ += output.size();
        return true;
    }

    [[nodiscard]] bool get_digest(util::Sha256Digest& digest) noexcept {
        return get_bytes(digest.bytes);
    }

    [[nodiscard]] bool get_wave_id(WaveIdV1& wave_id) noexcept {
        return get_bytes(wave_id.bytes);
    }

    [[nodiscard]] bool get_string(std::string& value, uint32_t max_size) {
        const std::size_t length_offset = offset_;
        uint32_t size = 0;
        if (!get_u32(size)) {
            return false;
        }
        if (size > max_size) {
            set_error(DistributedSieveProtocolError::collection_too_large, length_offset);
            return false;
        }
        if (!take(size)) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
        value.assign(begin, begin + size);
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool finish() noexcept {
        if (!status_) {
            return false;
        }
        if (offset_ != bytes_.size()) {
            set_error(DistributedSieveProtocolError::trailing_bytes, offset_);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] DistributedSieveProtocolStatus status() const noexcept {
        return status_;
    }

    void reject(DistributedSieveProtocolError error) noexcept {
        set_error(error, offset_);
    }

private:
    [[nodiscard]] bool take(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (amount > bytes_.size() - offset_) {
            set_error(DistributedSieveProtocolError::truncated, bytes_.size());
            return false;
        }
        return true;
    }

    void set_error(DistributedSieveProtocolError error, std::size_t relative_offset) noexcept {
        if (status_) {
            status_ = failure(error, base_offset_ + relative_offset);
        }
    }

    std::span<const std::byte> bytes_;
    uint64_t base_offset_;
    std::size_t offset_ = 0;
    DistributedSieveProtocolStatus status_;
};

template <typename Enum>
[[nodiscard]] bool read_closed_enum(Reader& reader, Enum& value,
                                    std::initializer_list<Enum> allowed) noexcept {
    using Raw = std::underlying_type_t<Enum>;
    Raw raw = 0;
    bool read = false;
    if constexpr (sizeof(Raw) == 1) {
        uint8_t temporary = 0;
        read = reader.get_u8(temporary);
        raw = static_cast<Raw>(temporary);
    } else if constexpr (sizeof(Raw) == 2) {
        uint16_t temporary = 0;
        read = reader.get_u16(temporary);
        raw = static_cast<Raw>(temporary);
    } else {
        static_assert(sizeof(Raw) <= 2, "unsupported enum width");
    }
    if (!read) {
        return false;
    }
    for (const Enum candidate : allowed) {
        if (static_cast<Raw>(candidate) == raw) {
            value = candidate;
            return true;
        }
    }
    reader.reject(DistributedSieveProtocolError::unknown_enum);
    return false;
}

template <typename Enum> void write_enum(Writer& writer, Enum value) {
    using Raw = std::underlying_type_t<Enum>;
    if constexpr (sizeof(Raw) == 1) {
        writer.put_u8(static_cast<uint8_t>(value));
    } else if constexpr (sizeof(Raw) == 2) {
        writer.put_u16(static_cast<uint16_t>(value));
    }
}

void write_native_identity(Writer& writer, const NativeIdentityV1& value) {
    writer.put_u64(value.volume);
    writer.put_u64(value.object);
    writer.put_u64(value.generation);
}

[[nodiscard]] bool read_native_identity(Reader& reader, NativeIdentityV1& value) noexcept {
    return reader.get_u64(value.volume) && reader.get_u64(value.object) &&
           reader.get_u64(value.generation);
}

void write_lease_id(Writer& writer, const LeaseIdV1& value) {
    writer.put_u64(value.limbs[0]);
    writer.put_u64(value.limbs[1]);
}

[[nodiscard]] bool read_lease_id(Reader& reader, LeaseIdV1& value) noexcept {
    return reader.get_u64(value.limbs[0]) && reader.get_u64(value.limbs[1]);
}

void write_lease_identity(Writer& writer, const LeaseIdentityV1& value) {
    write_lease_id(writer, value.lease_id);
    write_native_identity(writer, value.owner_marker);
    write_native_identity(writer, value.directory);
    writer.put_string(value.relative_stem);
}

[[nodiscard]] bool read_lease_identity(Reader& reader, LeaseIdentityV1& value) {
    return read_lease_id(reader, value.lease_id) &&
           read_native_identity(reader, value.owner_marker) &&
           read_native_identity(reader, value.directory) &&
           reader.get_string(value.relative_stem,
                             DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
}

void write_ooc_descriptor(Writer& writer, const OOCDescriptorV1& value) {
    writer.put_u64(value.format_version);
    writer.put_u64(value.store_id);
    writer.put_u64(value.generation);
    writer.put_u64(value.relation_count);
    writer.put_u64(value.data_end);
}

[[nodiscard]] bool read_ooc_descriptor(Reader& reader, OOCDescriptorV1& value) noexcept {
    return reader.get_u64(value.format_version) && reader.get_u64(value.store_id) &&
           reader.get_u64(value.generation) && reader.get_u64(value.relation_count) &&
           reader.get_u64(value.data_end);
}

void write_file_extent(Writer& writer, const NativeFileExtentV1& value) {
    write_native_identity(writer, value.identity);
    writer.put_u64(value.extent);
}

[[nodiscard]] bool read_file_extent(Reader& reader, NativeFileExtentV1& value) noexcept {
    return read_native_identity(reader, value.identity) && reader.get_u64(value.extent);
}

void write_sequence_receipt(Writer& writer, const RelationSequenceReceiptV1& value) {
    writer.put_u64(value.relation_count);
    writer.put_u64(value.low);
    writer.put_u64(value.high);
}

[[nodiscard]] bool read_sequence_receipt(Reader& reader,
                                         RelationSequenceReceiptV1& value) noexcept {
    return reader.get_u64(value.relation_count) && reader.get_u64(value.low) &&
           reader.get_u64(value.high);
}

void write_corpus_artifact(Writer& writer, const CorpusArtifactV1& value) {
    write_ooc_descriptor(writer, value.descriptor);
    write_file_extent(writer, value.index_file);
    write_file_extent(writer, value.data_file);
    write_sequence_receipt(writer, value.sequence_receipt);
    writer.put_digest(value.corpus_sha256);
}

[[nodiscard]] bool read_corpus_artifact(Reader& reader, CorpusArtifactV1& value) noexcept {
    return read_ooc_descriptor(reader, value.descriptor) &&
           read_file_extent(reader, value.index_file) &&
           read_file_extent(reader, value.data_file) &&
           read_sequence_receipt(reader, value.sequence_receipt) &&
           reader.get_digest(value.corpus_sha256);
}

void write_statistics(Writer& writer, const RelationStatisticsV1& value) {
    writer.put_u64(value.full_relations);
    writer.put_u64(value.partial_1lp);
    writer.put_u64(value.partial_2lp);
    writer.put_u64(value.partial_3lp);
}

[[nodiscard]] bool read_statistics(Reader& reader, RelationStatisticsV1& value) noexcept {
    return reader.get_u64(value.full_relations) && reader.get_u64(value.partial_1lp) &&
           reader.get_u64(value.partial_2lp) && reader.get_u64(value.partial_3lp);
}

void write_chunk_plan(Writer& writer, const ChunkPlanV1& value) {
    writer.put_u32(value.chunk_id);
    writer.put_u32(value.sq_begin);
    writer.put_u32(value.sq_end);
    writer.put_string(value.relative_artifact_stem);
}

[[nodiscard]] bool read_chunk_plan(Reader& reader, ChunkPlanV1& value) {
    return reader.get_u32(value.chunk_id) && reader.get_u32(value.sq_begin) &&
           reader.get_u32(value.sq_end) &&
           reader.get_string(value.relative_artifact_stem,
                             DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
}

void write_wait_facts(Writer& writer, const WorkerWaitFactsV1& value) {
    write_enum(writer, value.kind);
    writer.put_i32(value.exit_code);
    writer.put_u32(value.signal);
    writer.put_u32(value.native_error);
}

[[nodiscard]] bool read_wait_facts(Reader& reader, WorkerWaitFactsV1& value) noexcept {
    return read_closed_enum(reader, value.kind,
                            {WorkerWaitFactKindV1::unavailable, WorkerWaitFactKindV1::exited,
                             WorkerWaitFactKindV1::signaled,
                             WorkerWaitFactKindV1::native_wait_failure,
                             WorkerWaitFactKindV1::inherited_lock_quiescence}) &&
           reader.get_i32(value.exit_code) && reader.get_u32(value.signal) &&
           reader.get_u32(value.native_error);
}

void write_terminal_input(Writer& writer, const TerminalChunkInputV1& value) {
    writer.put_u32(value.chunk_id);
    write_enum(writer, value.disposition);
    writer.put_u32(value.sq_begin);
    writer.put_u32(value.sq_end);
    writer.put_u32(value.next_sq_index);
    writer.put_u64(value.processed_sq_count);
    write_enum(writer, value.completion_reason);
    writer.put_u32(value.durable_attempt_count);
    writer.put_digest(value.last_attempt_digest);
    write_lease_id(writer, value.lease_id);
    writer.put_digest(value.handoff_digest);
    writer.put_u64(value.raw_relation_count);
    write_sequence_receipt(writer, value.sequence_receipt);
    writer.put_digest(value.corpus_sha256);
}

[[nodiscard]] bool read_terminal_input(Reader& reader, TerminalChunkInputV1& value) noexcept {
    return reader.get_u32(value.chunk_id) &&
           read_closed_enum(reader, value.disposition,
                            {ChunkDispositionV1::handoff, ChunkDispositionV1::empty}) &&
           reader.get_u32(value.sq_begin) && reader.get_u32(value.sq_end) &&
           reader.get_u32(value.next_sq_index) && reader.get_u64(value.processed_sq_count) &&
           read_closed_enum(reader, value.completion_reason,
                            {WorkerCompletionReasonV1::range_exhausted,
                             WorkerCompletionReasonV1::sq_cap,
                             WorkerCompletionReasonV1::relation_cap,
                             WorkerCompletionReasonV1::zero_relations}) &&
           reader.get_u32(value.durable_attempt_count) &&
           reader.get_digest(value.last_attempt_digest) && read_lease_id(reader, value.lease_id) &&
           reader.get_digest(value.handoff_digest) && reader.get_u64(value.raw_relation_count) &&
           read_sequence_receipt(reader, value.sequence_receipt) &&
           reader.get_digest(value.corpus_sha256);
}

void write_diagnostic(Writer& writer, const NormalizedDiagnosticV1& value) {
    write_enum(writer, value.kind);
    writer.put_u32(value.code);
}

[[nodiscard]] bool read_diagnostic(Reader& reader, NormalizedDiagnosticV1& value) noexcept {
    return read_closed_enum(reader, value.kind,
                            {NormalizedDiagnosticKindV1::none,
                             NormalizedDiagnosticKindV1::recovered_handoff,
                             NormalizedDiagnosticKindV1::retried_after_exit,
                             NormalizedDiagnosticKindV1::retried_after_signal,
                             NormalizedDiagnosticKindV1::retried_after_invalid_handoff}) &&
           reader.get_u32(value.code);
}

void write_chunk_commit_summary(Writer& writer, const ChunkCommitSummaryV1& value) {
    write_terminal_input(writer, value.input);
    writer.put_u64(value.retained_relation_count);
    write_diagnostic(writer, value.diagnostic);
}

[[nodiscard]] bool read_chunk_commit_summary(Reader& reader, ChunkCommitSummaryV1& value) noexcept {
    return read_terminal_input(reader, value.input) &&
           reader.get_u64(value.retained_relation_count) &&
           read_diagnostic(reader, value.diagnostic);
}

void write_per_chunk_retained(Writer& writer, const PerChunkRetainedCountV1& value) {
    writer.put_u32(value.chunk_id);
    writer.put_u64(value.retained_relation_count);
}

[[nodiscard]] bool read_per_chunk_retained(Reader& reader,
                                           PerChunkRetainedCountV1& value) noexcept {
    return reader.get_u32(value.chunk_id) && reader.get_u64(value.retained_relation_count);
}

void write_cleanup_completion(Writer& writer, const CleanupCompletionSummaryV1& value) {
    write_enum(writer, value.artifact_kind);
    writer.put_u32(value.manifest_order_ordinal);
    writer.put_digest(value.authorization_digest);
    writer.put_digest(value.completion_digest);
}

[[nodiscard]] bool read_cleanup_completion(Reader& reader,
                                           CleanupCompletionSummaryV1& value) noexcept {
    return read_closed_enum(reader, value.artifact_kind,
                            {CleanupArtifactKindV1::worker, CleanupArtifactKindV1::merged}) &&
           reader.get_u32(value.manifest_order_ordinal) &&
           reader.get_digest(value.authorization_digest) &&
           reader.get_digest(value.completion_digest);
}

template <typename Value, typename WriteValue>
void write_vector(Writer& writer, const std::vector<Value>& values, WriteValue write_value) {
    if (values.size() > std::numeric_limits<uint32_t>::max()) {
        return;
    }
    writer.put_u32(static_cast<uint32_t>(values.size()));
    for (const auto& value : values) {
        write_value(writer, value);
    }
}

template <typename Value, typename ReadValue>
[[nodiscard]] bool read_vector(Reader& reader, std::vector<Value>& values, uint32_t maximum,
                               ReadValue read_value) {
    uint32_t count = 0;
    if (!reader.get_u32(count)) {
        return false;
    }
    if (count > maximum) {
        reader.reject(DistributedSieveProtocolError::collection_too_large);
        return false;
    }
    values.clear();
    values.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        Value value;
        if (!read_value(reader, value)) {
            return false;
        }
        values.push_back(std::move(value));
    }
    return true;
}

[[nodiscard]] DistributedSieveProtocolStatus validate_lease(const LeaseIdentityV1& lease) noexcept {
    if (lease_id_is_zero(lease.lease_id) || !native_identity_is_valid(lease.owner_marker) ||
        !native_identity_is_valid(lease.directory) || lease.owner_marker == lease.directory) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (!canonical_artifact_stem(lease.relative_stem)) {
        return failure(DistributedSieveProtocolError::invalid_string);
    }
    return {};
}

[[nodiscard]] bool lease_identities_conflict(const LeaseIdentityV1& left,
                                             const LeaseIdentityV1& right) noexcept {
    return left.lease_id == right.lease_id || left.owner_marker == right.owner_marker ||
           left.owner_marker == right.directory || left.directory == right.owner_marker ||
           left.directory == right.directory;
}

[[nodiscard]] constexpr bool
artifact_contains_native_identity(const CorpusArtifactV1& artifact,
                                  const NativeIdentityV1& identity) noexcept {
    return artifact.index_file.identity == identity || artifact.data_file.identity == identity;
}

[[nodiscard]] constexpr bool
lease_and_artifact_conflict(const LeaseIdentityV1& lease,
                            const CorpusArtifactV1& artifact) noexcept {
    return artifact_contains_native_identity(artifact, lease.owner_marker) ||
           artifact_contains_native_identity(artifact, lease.directory);
}

[[nodiscard]] DistributedSieveProtocolStatus validate_cleanup_authorization_identities(
    const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    const std::array<NativeIdentityV1, 7> identities{
        authorization.base_lock_identity,
        authorization.lease.directory,
        authorization.lease.owner_marker,
        authorization.owned_marker_identity,
        authorization.artifact.index_file.identity,
        authorization.artifact.data_file.identity,
        authorization.private_handoff_record.identity,
    };
    for (std::size_t left = 0; left < identities.size(); ++left) {
        if (!native_identity_is_valid(identities[left])) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        for (std::size_t right = left + 1; right < identities.size(); ++right) {
            if (identities[left] == identities[right]) {
                return failure(DistributedSieveProtocolError::duplicate_entry);
            }
        }
    }
    return {};
}

[[nodiscard]] constexpr bool
cleanup_authorization_contains_native_identity(const ArtifactCleanupAuthorizedV1& authorization,
                                               const NativeIdentityV1& identity) noexcept {
    return authorization.base_lock_identity == identity ||
           authorization.lease.directory == identity ||
           authorization.lease.owner_marker == identity ||
           authorization.owned_marker_identity == identity ||
           artifact_contains_native_identity(authorization.artifact, identity) ||
           authorization.private_handoff_record.identity == identity;
}

[[nodiscard]] constexpr bool
cleanup_authorization_bindings_conflict(const ArtifactCleanupAuthorizedV1& left,
                                        const ArtifactCleanupAuthorizedV1& right) noexcept {
    return cleanup_authorization_contains_native_identity(right, left.base_lock_identity) ||
           cleanup_authorization_contains_native_identity(right, left.lease.directory) ||
           cleanup_authorization_contains_native_identity(right, left.lease.owner_marker) ||
           cleanup_authorization_contains_native_identity(right, left.owned_marker_identity) ||
           cleanup_authorization_contains_native_identity(right,
                                                          left.artifact.index_file.identity) ||
           cleanup_authorization_contains_native_identity(right,
                                                          left.artifact.data_file.identity) ||
           cleanup_authorization_contains_native_identity(right,
                                                          left.private_handoff_record.identity) ||
           left.private_handoff_digest == right.private_handoff_digest;
}

[[nodiscard]] constexpr bool artifacts_conflict(const CorpusArtifactV1& left,
                                                const CorpusArtifactV1& right) noexcept {
    const bool store_identity_reused = left.descriptor.store_id == right.descriptor.store_id;
    const bool native_file_reused =
        artifact_contains_native_identity(right, left.index_file.identity) ||
        artifact_contains_native_identity(right, left.data_file.identity);
    return store_identity_reused || native_file_reused;
}

[[nodiscard]] bool artifact_bundles_conflict(const LeaseIdentityV1& left_lease,
                                             const CorpusArtifactV1& left_artifact,
                                             const LeaseIdentityV1& right_lease,
                                             const CorpusArtifactV1& right_artifact) noexcept {
    return lease_identities_conflict(left_lease, right_lease) ||
           lease_and_artifact_conflict(left_lease, right_artifact) ||
           lease_and_artifact_conflict(right_lease, left_artifact) ||
           artifacts_conflict(left_artifact, right_artifact);
}

[[nodiscard]] constexpr bool
manifest_control_contains_identity(const WaveManifestV1& manifest,
                                   const NativeIdentityV1& identity) noexcept {
    return manifest.wave_root_identity == identity || manifest.permanent_lock_identity == identity;
}

[[nodiscard]] constexpr bool
manifest_control_conflicts_with_lease(const WaveManifestV1& manifest,
                                      const LeaseIdentityV1& lease) noexcept {
    return manifest_control_contains_identity(manifest, lease.owner_marker) ||
           manifest_control_contains_identity(manifest, lease.directory);
}

[[nodiscard]] constexpr bool
manifest_control_conflicts_with_artifact(const WaveManifestV1& manifest,
                                         const CorpusArtifactV1& artifact) noexcept {
    return manifest_control_contains_identity(manifest, artifact.index_file.identity) ||
           manifest_control_contains_identity(manifest, artifact.data_file.identity);
}

[[nodiscard]] constexpr bool
manifest_control_conflicts_with_bundle(const WaveManifestV1& manifest, const LeaseIdentityV1& lease,
                                       const CorpusArtifactV1& artifact) noexcept {
    return manifest_control_conflicts_with_lease(manifest, lease) ||
           manifest_control_conflicts_with_artifact(manifest, artifact);
}

[[nodiscard]] constexpr bool manifest_control_conflicts_with_cleanup_authorization(
    const WaveManifestV1& manifest, const ArtifactCleanupAuthorizedV1& authorization) noexcept {
    return manifest_control_conflicts_with_bundle(manifest, authorization.lease,
                                                  authorization.artifact) ||
           manifest_control_contains_identity(manifest, authorization.base_lock_identity) ||
           manifest_control_contains_identity(manifest, authorization.owned_marker_identity) ||
           manifest_control_contains_identity(manifest,
                                              authorization.private_handoff_record.identity);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_artifact(const CorpusArtifactV1& artifact) noexcept {
    if (artifact.descriptor.format_version == 0 || artifact.descriptor.store_id == 0 ||
        artifact.descriptor.generation == 0 ||
        !native_identity_is_valid(artifact.index_file.identity) ||
        !native_identity_is_valid(artifact.data_file.identity) ||
        artifact.index_file.identity == artifact.data_file.identity ||
        artifact.index_file.extent == 0 || artifact.data_file.extent == 0 ||
        artifact.descriptor.data_end != artifact.data_file.extent ||
        artifact.descriptor.relation_count != artifact.sequence_receipt.relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_wait_facts(const WorkerWaitFactsV1& facts) noexcept {
    switch (facts.kind) {
    case WorkerWaitFactKindV1::unavailable:
        if (facts.exit_code != 0 || facts.signal != 0 || facts.native_error != 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    case WorkerWaitFactKindV1::exited:
        if (facts.exit_code < 0 || facts.exit_code > 255 || facts.signal != 0 ||
            facts.native_error != 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    case WorkerWaitFactKindV1::signaled:
        if (facts.exit_code != 0 || facts.signal == 0 || facts.signal > 255 ||
            facts.native_error != 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    case WorkerWaitFactKindV1::native_wait_failure:
        if (facts.exit_code != 0 || facts.signal != 0 || facts.native_error == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    case WorkerWaitFactKindV1::inherited_lock_quiescence:
        if (facts.exit_code != 0 || facts.signal != 0 || facts.native_error != 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    }
    return failure(DistributedSieveProtocolError::unknown_enum);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_chunk_plans(const std::vector<ChunkPlanV1>& chunks, uint32_t expected_count,
                     uint32_t range_begin, uint32_t range_end) noexcept {
    if (expected_count == 0 || expected_count > DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS ||
        chunks.size() != expected_count || range_end < range_begin) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    uint32_t cursor = range_begin;
    for (uint32_t index = 0; index < chunks.size(); ++index) {
        const auto& chunk = chunks[index];
        if (chunk.chunk_id != index) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (chunk.sq_begin < cursor) {
            return failure(DistributedSieveProtocolError::range_overlap,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (chunk.sq_begin > cursor) {
            return failure(DistributedSieveProtocolError::range_gap,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (chunk.sq_end < chunk.sq_begin ||
            !canonical_artifact_stem(chunk.relative_artifact_stem) ||
            (chunk.sq_begin != chunk.sq_end &&
             !worker_attempt_stem_is_representable(chunk.relative_artifact_stem))) {
            return failure(chunk.sq_end < chunk.sq_begin
                               ? DistributedSieveProtocolError::invalid_value
                               : DistributedSieveProtocolError::invalid_string,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (ascii_casefold_equal(chunks[previous].relative_artifact_stem,
                                     chunk.relative_artifact_stem)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        cursor = chunk.sq_end;
    }
    if (cursor != range_end) {
        return failure(cursor < range_end ? DistributedSieveProtocolError::range_gap
                                          : DistributedSieveProtocolError::range_overlap);
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_terminal_input(const TerminalChunkInputV1& input) noexcept {
    if (input.sq_end < input.sq_begin || input.next_sq_index < input.sq_begin ||
        input.next_sq_index > input.sq_end ||
        input.processed_sq_count > static_cast<uint64_t>(input.next_sq_index - input.sq_begin)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    switch (input.completion_reason) {
    case WorkerCompletionReasonV1::range_exhausted:
        if (input.next_sq_index != input.sq_end || input.processed_sq_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case WorkerCompletionReasonV1::sq_cap:
    case WorkerCompletionReasonV1::relation_cap:
        if (input.next_sq_index >= input.sq_end || input.processed_sq_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case WorkerCompletionReasonV1::zero_relations:
        if (input.next_sq_index != input.sq_end) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    default:
        return failure(DistributedSieveProtocolError::unknown_enum);
    }

    switch (input.disposition) {
    case ChunkDispositionV1::handoff:
        if (input.sq_begin == input.sq_end || input.durable_attempt_count == 0 ||
            input.durable_attempt_count > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
            lease_id_is_zero(input.lease_id) ||
            input.raw_relation_count != input.sequence_receipt.relation_count ||
            (input.completion_reason == WorkerCompletionReasonV1::zero_relations &&
             input.raw_relation_count != 0)) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if ((input.completion_reason == WorkerCompletionReasonV1::range_exhausted ||
             input.completion_reason == WorkerCompletionReasonV1::relation_cap) &&
            input.raw_relation_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    case ChunkDispositionV1::empty:
        if (input.sq_begin != input.sq_end || input.next_sq_index != input.sq_begin ||
            input.processed_sq_count != 0 ||
            input.completion_reason != WorkerCompletionReasonV1::zero_relations ||
            input.durable_attempt_count != 0 || !digest_is_zero(input.last_attempt_digest) ||
            !lease_id_is_zero(input.lease_id) || !digest_is_zero(input.handoff_digest) ||
            input.raw_relation_count != 0 ||
            input.sequence_receipt != RelationSequenceReceiptV1{} ||
            !digest_is_zero(input.corpus_sha256)) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return {};
    }
    return failure(DistributedSieveProtocolError::unknown_enum);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_terminal_inputs(const std::vector<TerminalChunkInputV1>& inputs) noexcept {
    if (inputs.empty() || inputs.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
        return failure(DistributedSieveProtocolError::collection_too_large);
    }
    uint32_t cursor = inputs.front().sq_begin;
    for (uint32_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (input.chunk_id != index) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (input.sq_begin != cursor) {
            return failure(input.sq_begin < cursor ? DistributedSieveProtocolError::range_overlap
                                                   : DistributedSieveProtocolError::range_gap,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (const auto status = validate_terminal_input(input); !status) {
            return failure(status.error, status.byte_offset, index);
        }
        cursor = input.sq_end;
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_chunk_summaries(const std::vector<ChunkCommitSummaryV1>& summaries) noexcept {
    if (summaries.empty() || summaries.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) {
        return failure(DistributedSieveProtocolError::collection_too_large);
    }
    uint32_t cursor = summaries.front().input.sq_begin;
    for (uint32_t index = 0; index < summaries.size(); ++index) {
        const auto& summary = summaries[index];
        if (summary.input.chunk_id != index) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (summary.input.sq_begin != cursor) {
            return failure(summary.input.sq_begin < cursor
                               ? DistributedSieveProtocolError::range_overlap
                               : DistributedSieveProtocolError::range_gap,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (const auto status = validate_terminal_input(summary.input); !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (summary.retained_relation_count > summary.input.raw_relation_count ||
            (summary.input.disposition == ChunkDispositionV1::empty &&
             summary.retained_relation_count != 0)) {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        switch (summary.diagnostic.kind) {
        case NormalizedDiagnosticKindV1::none:
        case NormalizedDiagnosticKindV1::recovered_handoff:
        case NormalizedDiagnosticKindV1::retried_after_invalid_handoff:
            if (summary.diagnostic.code != 0) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
            break;
        case NormalizedDiagnosticKindV1::retried_after_exit:
        case NormalizedDiagnosticKindV1::retried_after_signal:
            if (summary.diagnostic.code == 0 || summary.diagnostic.code > 255) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
            break;
        default:
            return failure(DistributedSieveProtocolError::unknown_enum,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        cursor = summary.input.sq_end;
    }
    return {};
}

[[nodiscard]] bool checked_add(uint64_t& accumulator, uint64_t value) noexcept {
    if (value > std::numeric_limits<uint64_t>::max() - accumulator) {
        return false;
    }
    accumulator += value;
    return true;
}

[[nodiscard]] DistributedSieveProtocolStatus validate_merge_counts(uint64_t input_count,
                                                                   uint64_t duplicate_count,
                                                                   uint64_t output_count) noexcept {
    if (duplicate_count > input_count || output_count > input_count ||
        input_count - duplicate_count != output_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus validate_value(const WaveManifestV1& value) noexcept {
    if (wave_id_is_zero(value.wave_id) || value.execution_contract_version == 0 ||
        !native_identity_is_valid(value.wave_root_identity) ||
        !native_identity_is_valid(value.permanent_lock_identity) ||
        value.wave_root_identity == value.permanent_lock_identity ||
        value.lock_semantics_version == 0 || value.effective_sq_end <= value.effective_sq_begin ||
        value.max_worker_attempts == 0 ||
        value.max_worker_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        value.max_merge_build_attempts == 0 ||
        value.max_merge_build_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        value.max_consumption_attempts == 0 ||
        value.max_consumption_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        value.canonical_naming_version != DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1 ||
        value.retry_policy_version == 0 || !value.durable_start_consumes_ordinal ||
        value.ooc_format_version == 0 || value.relation_serialization_version == 0 ||
        value.handoff_version == 0 || value.receipt_version == 0 || value.digest_version == 0 ||
        value.merge_policy_version == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return validate_chunk_plans(value.chunks, value.worker_count, value.effective_sq_begin,
                                value.effective_sq_end);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const AttemptStartedV1& value) noexcept {
    if (value.sq_end <= value.sq_begin ||
        value.attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        value.retry_policy_version == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return validate_lease(value.lease);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const ChunkTerminalFailureV1& value) noexcept {
    bool reason_is_valid = false;
    switch (value.reason) {
    case ChunkTerminalFailureReasonV1::spawn_failed:
    case ChunkTerminalFailureReasonV1::exited_unsuccessfully:
    case ChunkTerminalFailureReasonV1::signaled:
    case ChunkTerminalFailureReasonV1::wait_failed:
    case ChunkTerminalFailureReasonV1::invalid_handoff:
    case ChunkTerminalFailureReasonV1::attempt_budget_exhausted:
    case ChunkTerminalFailureReasonV1::no_handoff_after_inherited_lock_quiescence:
        reason_is_valid = true;
        break;
    }
    if (!reason_is_valid || value.sq_end <= value.sq_begin || value.exhausted_attempt_count == 0 ||
        value.exhausted_attempt_count > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        !value.no_canonical_handoff_confirmed || !value.exact_attempt_lease_absent_confirmed ||
        value.next_sq_index != value.sq_begin || value.processed_sq_count != 0 ||
        value.statistics != RelationStatisticsV1{}) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_wait_facts(value.wait_facts); !status) {
        return status;
    }
    switch (value.reason) {
    case ChunkTerminalFailureReasonV1::spawn_failed:
    case ChunkTerminalFailureReasonV1::attempt_budget_exhausted:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::unavailable) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case ChunkTerminalFailureReasonV1::exited_unsuccessfully:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::exited ||
            value.wait_facts.exit_code == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case ChunkTerminalFailureReasonV1::signaled:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::signaled) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case ChunkTerminalFailureReasonV1::wait_failed:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::native_wait_failure) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case ChunkTerminalFailureReasonV1::invalid_handoff:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::unavailable &&
            (value.wait_facts.kind != WorkerWaitFactKindV1::exited ||
             value.wait_facts.exit_code != 0)) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case ChunkTerminalFailureReasonV1::no_handoff_after_inherited_lock_quiescence:
        if (value.wait_facts.kind != WorkerWaitFactKindV1::inherited_lock_quiescence) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus validate_value(const WorkerHandoffV1& value) noexcept {
    if (value.sq_end <= value.sq_begin ||
        value.attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        value.next_sq_index < value.sq_begin || value.next_sq_index > value.sq_end ||
        value.processed_sq_count > static_cast<uint64_t>(value.next_sq_index - value.sq_begin) ||
        value.relation_count != value.artifact.descriptor.relation_count ||
        !value.cleanup_intent_absent) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_lease(value.lease); !status) {
        return status;
    }
    if (const auto status = validate_artifact(value.artifact); !status) {
        return status;
    }
    if (lease_and_artifact_conflict(value.lease, value.artifact)) {
        return failure(DistributedSieveProtocolError::duplicate_entry);
    }
    switch (value.completion_reason) {
    case WorkerCompletionReasonV1::range_exhausted:
        if (value.next_sq_index != value.sq_end || value.processed_sq_count == 0 ||
            value.relation_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case WorkerCompletionReasonV1::sq_cap:
        if (value.next_sq_index >= value.sq_end || value.processed_sq_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case WorkerCompletionReasonV1::relation_cap:
        if (value.next_sq_index >= value.sq_end || value.processed_sq_count == 0 ||
            value.relation_count == 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    case WorkerCompletionReasonV1::zero_relations:
        if (value.next_sq_index != value.sq_end || value.relation_count != 0) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    default:
        return failure(DistributedSieveProtocolError::unknown_enum);
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus validate_value(const MergeStartedV1& value) noexcept {
    if (value.merge_policy_version == 0 ||
        value.merge_attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_terminal_inputs(value.ordered_inputs); !status) {
        return status;
    }
    return validate_lease(value.merged_lease);
}

[[nodiscard]] DistributedSieveProtocolStatus validate_value(const MergePreparedV1& value) noexcept {
    if (value.merge_policy_version == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_terminal_inputs(value.ordered_inputs); !status) {
        return status;
    }
    if (const auto status =
            validate_merge_counts(value.input_relation_count, value.duplicate_relation_count,
                                  value.output_relation_count);
        !status) {
        return status;
    }
    if (value.per_chunk_retained_counts.size() != value.ordered_inputs.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    uint64_t observed_input = 0;
    uint64_t observed_output = 0;
    for (uint32_t index = 0; index < value.ordered_inputs.size(); ++index) {
        const auto& retained = value.per_chunk_retained_counts[index];
        if (retained.chunk_id != index ||
            retained.retained_relation_count > value.ordered_inputs[index].raw_relation_count ||
            !checked_add(observed_input, value.ordered_inputs[index].raw_relation_count) ||
            !checked_add(observed_output, retained.retained_relation_count)) {
            return failure(retained.chunk_id != index
                               ? DistributedSieveProtocolError::noncanonical_order
                               : DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }
    if (observed_input != value.input_relation_count ||
        observed_output != value.output_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_artifact(value.merged_artifact); !status) {
        return status;
    }
    if (value.merged_artifact.descriptor.relation_count != value.output_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_lease(value.merged_lease); !status) {
        return status;
    }
    return lease_and_artifact_conflict(value.merged_lease, value.merged_artifact)
               ? failure(DistributedSieveProtocolError::duplicate_entry)
               : DistributedSieveProtocolStatus{};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const WaveMergeCommitV1& value) noexcept {
    if (value.merge_policy_version == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_chunk_summaries(value.chunks); !status) {
        return status;
    }
    if (const auto status =
            validate_merge_counts(value.input_relation_count, value.duplicate_relation_count,
                                  value.output_relation_count);
        !status) {
        return status;
    }
    uint64_t observed_input = 0;
    uint64_t observed_output = 0;
    for (const auto& chunk : value.chunks) {
        if (!checked_add(observed_input, chunk.input.raw_relation_count) ||
            !checked_add(observed_output, chunk.retained_relation_count)) {
            return failure(DistributedSieveProtocolError::integer_out_of_range);
        }
    }
    if (observed_input != value.input_relation_count ||
        observed_output != value.output_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_lease(value.merged_lease); !status) {
        return status;
    }
    if (const auto status = validate_artifact(value.merged_artifact); !status) {
        return status;
    }
    if (value.merged_artifact.descriptor.relation_count != value.output_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return lease_and_artifact_conflict(value.merged_lease, value.merged_artifact)
               ? failure(DistributedSieveProtocolError::duplicate_entry)
               : DistributedSieveProtocolStatus{};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const ArtifactCleanupAuthorizedV1& value) noexcept {
    bool kind_pair_is_valid = false;
    switch (value.authorizer) {
    case CleanupAuthorizerKindV1::merge_commit_worker:
        kind_pair_is_valid = value.artifact_kind == CleanupArtifactKindV1::worker &&
                             value.manifest_order_ordinal < DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS;
        break;
    case CleanupAuthorizerKindV1::consumption_ack_merged:
        kind_pair_is_valid = value.artifact_kind == CleanupArtifactKindV1::merged &&
                             value.manifest_order_ordinal == 0;
        break;
    }
    if (!kind_pair_is_valid) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_lease(value.lease); !status) {
        return status;
    }
    if (const auto status = validate_artifact(value.artifact); !status) {
        return status;
    }
    if (digest_is_zero(value.private_handoff_digest) || value.private_handoff_record.extent == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return validate_cleanup_authorization_identities(value);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const ArtifactCleanupCompletedV1& value) noexcept {
    if (!value.parent_directory_durability_confirmed || !value.expected_namespace_absent ||
        (value.cleanup_intent_identity.has_value() &&
         !native_identity_is_valid(*value.cleanup_intent_identity))) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const ConsumptionStartedV1& value) noexcept {
    if (value.consumer_kind != ConsumerKindV1::structured_reduction_relation_corpus ||
        value.execution_contract_version == 0 || value.successor_format_version == 0 ||
        value.consumption_attempt_ordinal >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return validate_lease(value.successor_lease);
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const SuccessorPreparedV1& value) noexcept {
    if (value.output_relation_count > value.input_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_lease(value.successor_lease); !status) {
        return status;
    }
    if (const auto status = validate_artifact(value.successor_artifact); !status) {
        return status;
    }
    if (value.successor_artifact.descriptor.relation_count != value.output_relation_count) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return lease_and_artifact_conflict(value.successor_lease, value.successor_artifact)
               ? failure(DistributedSieveProtocolError::duplicate_entry)
               : DistributedSieveProtocolStatus{};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_value(const WaveConsumptionAckV1& value) noexcept {
    if (value.consumer_kind != ConsumerKindV1::structured_reduction_relation_corpus ||
        !native_identity_is_valid(value.successor_cleanup_authority_identity)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_artifact(value.successor_artifact); !status) {
        return status;
    }
    return artifact_contains_native_identity(value.successor_artifact,
                                             value.successor_cleanup_authority_identity)
               ? failure(DistributedSieveProtocolError::duplicate_entry)
               : DistributedSieveProtocolStatus{};
}

[[nodiscard]] DistributedSieveProtocolStatus validate_value(const WaveCompletedV1& value) noexcept {
    if (!native_identity_is_valid(value.wave_root_identity) ||
        !native_identity_is_valid(value.permanent_lock_identity) ||
        value.wave_root_identity == value.permanent_lock_identity) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_chunk_summaries(value.chunks); !status) {
        return status;
    }
    if (const auto status = validate_artifact(value.successor_artifact); !status) {
        return status;
    }
    if (artifact_contains_native_identity(value.successor_artifact, value.wave_root_identity) ||
        artifact_contains_native_identity(value.successor_artifact,
                                          value.permanent_lock_identity)) {
        return failure(DistributedSieveProtocolError::duplicate_entry);
    }

    uint32_t expected_cleanup_count = 1;
    for (const auto& chunk : value.chunks) {
        if (chunk.input.disposition == ChunkDispositionV1::handoff) {
            ++expected_cleanup_count;
        }
    }
    if (value.cleanup_confirmations.size() != expected_cleanup_count ||
        value.cleanup_confirmations.size() >
            static_cast<std::size_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) + 1U) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    std::size_t cleanup_index = 0;
    for (const auto& chunk : value.chunks) {
        if (chunk.input.disposition != ChunkDispositionV1::handoff) {
            continue;
        }
        const auto& cleanup = value.cleanup_confirmations[cleanup_index++];
        if (cleanup.artifact_kind != CleanupArtifactKindV1::worker ||
            cleanup.manifest_order_ordinal != chunk.input.chunk_id) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
                           static_cast<uint32_t>(cleanup_index - 1));
        }
    }
    const auto& merged = value.cleanup_confirmations[cleanup_index];
    if (merged.artifact_kind != CleanupArtifactKindV1::merged ||
        merged.manifest_order_ordinal != 0) {
        return failure(DistributedSieveProtocolError::noncanonical_order,
                       DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, static_cast<uint32_t>(cleanup_index));
    }
    return {};
}

template <typename Value>
[[nodiscard]] const util::Sha256Digest& self_digest(const Value& value) noexcept {
    return value.self_digest;
}

template <typename Value> util::Sha256Digest& self_digest(Value& value) noexcept {
    return value.self_digest;
}

void write_payload(Writer& writer, const WaveManifestV1& value) {
    writer.put_wave_id(value.wave_id);
    writer.put_u32(value.execution_contract_version);
    writer.put_digest(value.executable_sha256);
    writer.put_digest(value.work_sha256);
    write_native_identity(writer, value.wave_root_identity);
    write_native_identity(writer, value.permanent_lock_identity);
    writer.put_u32(value.lock_semantics_version);
    writer.put_u32(value.effective_sq_begin);
    writer.put_u32(value.effective_sq_end);
    writer.put_u32(value.worker_count);
    write_vector(writer, value.chunks, write_chunk_plan);
    writer.put_u64(value.sq_cap_per_worker);
    writer.put_u64(value.relation_cap_per_worker);
    writer.put_u32(value.max_worker_attempts);
    writer.put_u32(value.max_merge_build_attempts);
    writer.put_u32(value.max_consumption_attempts);
    writer.put_u32(value.canonical_naming_version);
    writer.put_u32(value.retry_policy_version);
    writer.put_bool(value.durable_start_consumes_ordinal);
    writer.put_u32(value.ooc_format_version);
    writer.put_u32(value.relation_serialization_version);
    writer.put_u32(value.handoff_version);
    writer.put_u32(value.receipt_version);
    writer.put_u32(value.digest_version);
    writer.put_u32(value.merge_policy_version);
}

void write_payload(Writer& writer, const AttemptStartedV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_u32(value.chunk_id);
    writer.put_u32(value.sq_begin);
    writer.put_u32(value.sq_end);
    writer.put_u32(value.attempt_ordinal);
    writer.put_digest(value.predecessor_digest);
    write_lease_identity(writer, value.lease);
    writer.put_u32(value.retry_policy_version);
}

void write_payload(Writer& writer, const ChunkTerminalFailureV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_u32(value.chunk_id);
    writer.put_u32(value.sq_begin);
    writer.put_u32(value.sq_end);
    writer.put_u32(value.exhausted_attempt_count);
    writer.put_digest(value.last_attempt_digest);
    writer.put_digest(value.predecessor_chain_digest);
    write_enum(writer, value.reason);
    write_wait_facts(writer, value.wait_facts);
    writer.put_bool(value.no_canonical_handoff_confirmed);
    writer.put_bool(value.exact_attempt_lease_absent_confirmed);
    writer.put_u32(value.next_sq_index);
    writer.put_u64(value.processed_sq_count);
    write_statistics(writer, value.statistics);
}

void write_payload(Writer& writer, const WorkerHandoffV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.work_digest);
    writer.put_wave_id(value.wave_id);
    writer.put_u32(value.chunk_id);
    writer.put_u32(value.sq_begin);
    writer.put_u32(value.sq_end);
    writer.put_u32(value.attempt_ordinal);
    writer.put_digest(value.attempt_started_digest);
    write_lease_identity(writer, value.lease);
    write_corpus_artifact(writer, value.artifact);
    writer.put_u64(value.processed_sq_count);
    writer.put_u32(value.next_sq_index);
    write_enum(writer, value.completion_reason);
    writer.put_u64(value.relation_count);
    writer.put_bool(value.cleanup_intent_absent);
}

void write_payload(Writer& writer, const MergeStartedV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.work_digest);
    write_vector(writer, value.ordered_inputs, write_terminal_input);
    writer.put_u32(value.merge_policy_version);
    write_lease_identity(writer, value.merged_lease);
    writer.put_u32(value.merge_attempt_ordinal);
    writer.put_digest(value.predecessor_digest);
}

void write_payload(Writer& writer, const MergePreparedV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.work_digest);
    writer.put_u32(value.merge_policy_version);
    writer.put_digest(value.merge_started_digest);
    write_vector(writer, value.ordered_inputs, write_terminal_input);
    writer.put_u64(value.input_relation_count);
    writer.put_u64(value.duplicate_relation_count);
    writer.put_u64(value.output_relation_count);
    write_vector(writer, value.per_chunk_retained_counts, write_per_chunk_retained);
    write_corpus_artifact(writer, value.merged_artifact);
    write_lease_identity(writer, value.merged_lease);
}

void write_payload(Writer& writer, const WaveMergeCommitV1& value) {
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.work_digest);
    write_vector(writer, value.chunks, write_chunk_commit_summary);
    writer.put_u32(value.merge_policy_version);
    writer.put_u64(value.input_relation_count);
    writer.put_u64(value.duplicate_relation_count);
    writer.put_u64(value.output_relation_count);
    writer.put_digest(value.merge_prepared_digest);
    write_lease_identity(writer, value.merged_lease);
    write_corpus_artifact(writer, value.merged_artifact);
}

void write_payload(Writer& writer, const ArtifactCleanupAuthorizedV1& value) {
    write_enum(writer, value.authorizer);
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.authorizer_record_digest);
    write_enum(writer, value.artifact_kind);
    writer.put_u32(value.manifest_order_ordinal);
    write_lease_identity(writer, value.lease);
    write_native_identity(writer, value.base_lock_identity);
    write_native_identity(writer, value.owned_marker_identity);
    writer.put_digest(value.handoff_digest);
    writer.put_digest(value.private_handoff_digest);
    write_file_extent(writer, value.private_handoff_record);
    write_corpus_artifact(writer, value.artifact);
}

void write_payload(Writer& writer, const ArtifactCleanupCompletedV1& value) {
    writer.put_digest(value.authorization_digest);
    writer.put_bool(value.cleanup_intent_identity.has_value());
    if (value.cleanup_intent_identity.has_value()) {
        write_native_identity(writer, *value.cleanup_intent_identity);
    }
    writer.put_bool(value.parent_directory_durability_confirmed);
    writer.put_bool(value.expected_namespace_absent);
}

void write_payload(Writer& writer, const ConsumptionStartedV1& value) {
    writer.put_digest(value.merge_commit_digest);
    writer.put_digest(value.manifest_digest);
    write_enum(writer, value.consumer_kind);
    writer.put_u32(value.execution_contract_version);
    write_lease_identity(writer, value.successor_lease);
    writer.put_u32(value.successor_format_version);
    writer.put_u32(value.consumption_attempt_ordinal);
    writer.put_digest(value.predecessor_digest);
}

void write_payload(Writer& writer, const SuccessorPreparedV1& value) {
    writer.put_digest(value.consumption_started_digest);
    write_lease_identity(writer, value.successor_lease);
    write_corpus_artifact(writer, value.successor_artifact);
    writer.put_digest(value.successor_semantic_digest);
    writer.put_u64(value.input_relation_count);
    writer.put_u64(value.output_relation_count);
}

void write_payload(Writer& writer, const WaveConsumptionAckV1& value) {
    writer.put_digest(value.merge_commit_digest);
    write_enum(writer, value.consumer_kind);
    writer.put_digest(value.consumption_started_digest);
    writer.put_digest(value.successor_prepared_digest);
    write_corpus_artifact(writer, value.successor_artifact);
    writer.put_digest(value.successor_semantic_digest);
    write_native_identity(writer, value.successor_cleanup_authority_identity);
}

void write_payload(Writer& writer, const WaveCompletedV1& value) {
    write_native_identity(writer, value.wave_root_identity);
    write_native_identity(writer, value.permanent_lock_identity);
    writer.put_digest(value.manifest_digest);
    writer.put_digest(value.merge_commit_digest);
    writer.put_digest(value.consumption_ack_digest);
    writer.put_digest(value.successor_prepared_digest);
    write_vector(writer, value.chunks, write_chunk_commit_summary);
    write_vector(writer, value.cleanup_confirmations, write_cleanup_completion);
    write_corpus_artifact(writer, value.successor_artifact);
    writer.put_digest(value.successor_semantic_digest);
}

[[nodiscard]] bool read_payload(Reader& reader, WaveManifestV1& value) {
    return reader.get_wave_id(value.wave_id) && reader.get_u32(value.execution_contract_version) &&
           reader.get_digest(value.executable_sha256) && reader.get_digest(value.work_sha256) &&
           read_native_identity(reader, value.wave_root_identity) &&
           read_native_identity(reader, value.permanent_lock_identity) &&
           reader.get_u32(value.lock_semantics_version) &&
           reader.get_u32(value.effective_sq_begin) && reader.get_u32(value.effective_sq_end) &&
           reader.get_u32(value.worker_count) &&
           read_vector(reader, value.chunks, DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                       read_chunk_plan) &&
           reader.get_u64(value.sq_cap_per_worker) &&
           reader.get_u64(value.relation_cap_per_worker) &&
           reader.get_u32(value.max_worker_attempts) &&
           reader.get_u32(value.max_merge_build_attempts) &&
           reader.get_u32(value.max_consumption_attempts) &&
           reader.get_u32(value.canonical_naming_version) &&
           reader.get_u32(value.retry_policy_version) &&
           reader.get_bool(value.durable_start_consumes_ordinal) &&
           reader.get_u32(value.ooc_format_version) &&
           reader.get_u32(value.relation_serialization_version) &&
           reader.get_u32(value.handoff_version) && reader.get_u32(value.receipt_version) &&
           reader.get_u32(value.digest_version) && reader.get_u32(value.merge_policy_version);
}

[[nodiscard]] bool read_payload(Reader& reader, AttemptStartedV1& value) {
    return reader.get_digest(value.manifest_digest) && reader.get_u32(value.chunk_id) &&
           reader.get_u32(value.sq_begin) && reader.get_u32(value.sq_end) &&
           reader.get_u32(value.attempt_ordinal) && reader.get_digest(value.predecessor_digest) &&
           read_lease_identity(reader, value.lease) && reader.get_u32(value.retry_policy_version);
}

[[nodiscard]] bool read_payload(Reader& reader, ChunkTerminalFailureV1& value) noexcept {
    return reader.get_digest(value.manifest_digest) && reader.get_u32(value.chunk_id) &&
           reader.get_u32(value.sq_begin) && reader.get_u32(value.sq_end) &&
           reader.get_u32(value.exhausted_attempt_count) &&
           reader.get_digest(value.last_attempt_digest) &&
           reader.get_digest(value.predecessor_chain_digest) &&
           read_closed_enum(
               reader, value.reason,
               {ChunkTerminalFailureReasonV1::spawn_failed,
                ChunkTerminalFailureReasonV1::exited_unsuccessfully,
                ChunkTerminalFailureReasonV1::signaled, ChunkTerminalFailureReasonV1::wait_failed,
                ChunkTerminalFailureReasonV1::invalid_handoff,
                ChunkTerminalFailureReasonV1::attempt_budget_exhausted,
                ChunkTerminalFailureReasonV1::no_handoff_after_inherited_lock_quiescence}) &&
           read_wait_facts(reader, value.wait_facts) &&
           reader.get_bool(value.no_canonical_handoff_confirmed) &&
           reader.get_bool(value.exact_attempt_lease_absent_confirmed) &&
           reader.get_u32(value.next_sq_index) && reader.get_u64(value.processed_sq_count) &&
           read_statistics(reader, value.statistics);
}

[[nodiscard]] bool read_payload(Reader& reader, WorkerHandoffV1& value) {
    return reader.get_digest(value.manifest_digest) && reader.get_digest(value.work_digest) &&
           reader.get_wave_id(value.wave_id) && reader.get_u32(value.chunk_id) &&
           reader.get_u32(value.sq_begin) && reader.get_u32(value.sq_end) &&
           reader.get_u32(value.attempt_ordinal) &&
           reader.get_digest(value.attempt_started_digest) &&
           read_lease_identity(reader, value.lease) &&
           read_corpus_artifact(reader, value.artifact) &&
           reader.get_u64(value.processed_sq_count) && reader.get_u32(value.next_sq_index) &&
           read_closed_enum(reader, value.completion_reason,
                            {WorkerCompletionReasonV1::range_exhausted,
                             WorkerCompletionReasonV1::sq_cap,
                             WorkerCompletionReasonV1::relation_cap,
                             WorkerCompletionReasonV1::zero_relations}) &&
           reader.get_u64(value.relation_count) && reader.get_bool(value.cleanup_intent_absent);
}

[[nodiscard]] bool read_payload(Reader& reader, MergeStartedV1& value) {
    return reader.get_digest(value.manifest_digest) && reader.get_digest(value.work_digest) &&
           read_vector(reader, value.ordered_inputs, DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                       read_terminal_input) &&
           reader.get_u32(value.merge_policy_version) &&
           read_lease_identity(reader, value.merged_lease) &&
           reader.get_u32(value.merge_attempt_ordinal) &&
           reader.get_digest(value.predecessor_digest);
}

[[nodiscard]] bool read_payload(Reader& reader, MergePreparedV1& value) {
    return reader.get_digest(value.manifest_digest) && reader.get_digest(value.work_digest) &&
           reader.get_u32(value.merge_policy_version) &&
           reader.get_digest(value.merge_started_digest) &&
           read_vector(reader, value.ordered_inputs, DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                       read_terminal_input) &&
           reader.get_u64(value.input_relation_count) &&
           reader.get_u64(value.duplicate_relation_count) &&
           reader.get_u64(value.output_relation_count) &&
           read_vector(reader, value.per_chunk_retained_counts,
                       DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS, read_per_chunk_retained) &&
           read_corpus_artifact(reader, value.merged_artifact) &&
           read_lease_identity(reader, value.merged_lease);
}

[[nodiscard]] bool read_payload(Reader& reader, WaveMergeCommitV1& value) {
    return reader.get_digest(value.manifest_digest) && reader.get_digest(value.work_digest) &&
           read_vector(reader, value.chunks, DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                       read_chunk_commit_summary) &&
           reader.get_u32(value.merge_policy_version) &&
           reader.get_u64(value.input_relation_count) &&
           reader.get_u64(value.duplicate_relation_count) &&
           reader.get_u64(value.output_relation_count) &&
           reader.get_digest(value.merge_prepared_digest) &&
           read_lease_identity(reader, value.merged_lease) &&
           read_corpus_artifact(reader, value.merged_artifact);
}

[[nodiscard]] bool read_payload(Reader& reader, ArtifactCleanupAuthorizedV1& value) {
    return read_closed_enum(reader, value.authorizer,
                            {CleanupAuthorizerKindV1::merge_commit_worker,
                             CleanupAuthorizerKindV1::consumption_ack_merged}) &&
           reader.get_digest(value.manifest_digest) &&
           reader.get_digest(value.authorizer_record_digest) &&
           read_closed_enum(reader, value.artifact_kind,
                            {CleanupArtifactKindV1::worker, CleanupArtifactKindV1::merged}) &&
           reader.get_u32(value.manifest_order_ordinal) &&
           read_lease_identity(reader, value.lease) &&
           read_native_identity(reader, value.base_lock_identity) &&
           read_native_identity(reader, value.owned_marker_identity) &&
           reader.get_digest(value.handoff_digest) &&
           reader.get_digest(value.private_handoff_digest) &&
           read_file_extent(reader, value.private_handoff_record) &&
           read_corpus_artifact(reader, value.artifact);
}

[[nodiscard]] bool read_payload(Reader& reader, ArtifactCleanupCompletedV1& value) noexcept {
    bool has_cleanup_intent = false;
    if (!reader.get_digest(value.authorization_digest) || !reader.get_bool(has_cleanup_intent)) {
        return false;
    }
    if (has_cleanup_intent) {
        NativeIdentityV1 identity;
        if (!read_native_identity(reader, identity)) {
            return false;
        }
        value.cleanup_intent_identity = identity;
    } else {
        value.cleanup_intent_identity.reset();
    }
    return reader.get_bool(value.parent_directory_durability_confirmed) &&
           reader.get_bool(value.expected_namespace_absent);
}

[[nodiscard]] bool read_payload(Reader& reader, ConsumptionStartedV1& value) {
    return reader.get_digest(value.merge_commit_digest) &&
           reader.get_digest(value.manifest_digest) &&
           read_closed_enum(reader, value.consumer_kind,
                            {ConsumerKindV1::structured_reduction_relation_corpus}) &&
           reader.get_u32(value.execution_contract_version) &&
           read_lease_identity(reader, value.successor_lease) &&
           reader.get_u32(value.successor_format_version) &&
           reader.get_u32(value.consumption_attempt_ordinal) &&
           reader.get_digest(value.predecessor_digest);
}

[[nodiscard]] bool read_payload(Reader& reader, SuccessorPreparedV1& value) {
    return reader.get_digest(value.consumption_started_digest) &&
           read_lease_identity(reader, value.successor_lease) &&
           read_corpus_artifact(reader, value.successor_artifact) &&
           reader.get_digest(value.successor_semantic_digest) &&
           reader.get_u64(value.input_relation_count) &&
           reader.get_u64(value.output_relation_count);
}

[[nodiscard]] bool read_payload(Reader& reader, WaveConsumptionAckV1& value) {
    return reader.get_digest(value.merge_commit_digest) &&
           read_closed_enum(reader, value.consumer_kind,
                            {ConsumerKindV1::structured_reduction_relation_corpus}) &&
           reader.get_digest(value.consumption_started_digest) &&
           reader.get_digest(value.successor_prepared_digest) &&
           read_corpus_artifact(reader, value.successor_artifact) &&
           reader.get_digest(value.successor_semantic_digest) &&
           read_native_identity(reader, value.successor_cleanup_authority_identity);
}

[[nodiscard]] bool read_payload(Reader& reader, WaveCompletedV1& value) {
    return read_native_identity(reader, value.wave_root_identity) &&
           read_native_identity(reader, value.permanent_lock_identity) &&
           reader.get_digest(value.manifest_digest) &&
           reader.get_digest(value.merge_commit_digest) &&
           reader.get_digest(value.consumption_ack_digest) &&
           reader.get_digest(value.successor_prepared_digest) &&
           read_vector(reader, value.chunks, DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS,
                       read_chunk_commit_summary) &&
           read_vector(reader, value.cleanup_confirmations,
                       DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS + 1U, read_cleanup_completion) &&
           read_corpus_artifact(reader, value.successor_artifact) &&
           reader.get_digest(value.successor_semantic_digest);
}

template <typename Value>
[[nodiscard]] constexpr DistributedSieveRecordKindV1 record_kind_for() noexcept {
    if constexpr (std::is_same_v<Value, WaveManifestV1>) {
        return DistributedSieveRecordKindV1::wave_manifest;
    } else if constexpr (std::is_same_v<Value, AttemptStartedV1>) {
        return DistributedSieveRecordKindV1::attempt_started;
    } else if constexpr (std::is_same_v<Value, ChunkTerminalFailureV1>) {
        return DistributedSieveRecordKindV1::chunk_terminal_failure;
    } else if constexpr (std::is_same_v<Value, WorkerHandoffV1>) {
        return DistributedSieveRecordKindV1::worker_handoff;
    } else if constexpr (std::is_same_v<Value, MergeStartedV1>) {
        return DistributedSieveRecordKindV1::merge_started;
    } else if constexpr (std::is_same_v<Value, MergePreparedV1>) {
        return DistributedSieveRecordKindV1::merge_prepared;
    } else if constexpr (std::is_same_v<Value, WaveMergeCommitV1>) {
        return DistributedSieveRecordKindV1::wave_merge_commit;
    } else if constexpr (std::is_same_v<Value, ArtifactCleanupAuthorizedV1>) {
        return DistributedSieveRecordKindV1::artifact_cleanup_authorized;
    } else if constexpr (std::is_same_v<Value, ArtifactCleanupCompletedV1>) {
        return DistributedSieveRecordKindV1::artifact_cleanup_completed;
    } else if constexpr (std::is_same_v<Value, ConsumptionStartedV1>) {
        return DistributedSieveRecordKindV1::consumption_started;
    } else if constexpr (std::is_same_v<Value, SuccessorPreparedV1>) {
        return DistributedSieveRecordKindV1::successor_prepared;
    } else if constexpr (std::is_same_v<Value, WaveConsumptionAckV1>) {
        return DistributedSieveRecordKindV1::wave_consumption_ack;
    } else {
        static_assert(std::is_same_v<Value, WaveCompletedV1>, "unknown protocol record");
        return DistributedSieveRecordKindV1::wave_completed;
    }
}

[[nodiscard]] std::optional<util::Sha256Digest>
hash_domain_and_bytes(std::string_view domain, std::span<const std::byte> bytes) noexcept {
    util::Sha256Accumulator accumulator;
    constexpr std::array<std::byte, 1> separator{std::byte{0}};
    if (!accumulator.update(domain) || !accumulator.update(separator) ||
        !accumulator.update(bytes)) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

[[nodiscard]] DistributedSieveProtocolStatus
write_record_preimage(const DistributedSieveProtocolRecordV1& record, Writer& writer) {
    const auto kind =
        std::visit([]<typename Value>(const Value&) { return record_kind_for<Value>(); }, record);
    for (const char character : RECORD_MAGIC) {
        writer.put_u8(static_cast<uint8_t>(static_cast<unsigned char>(character)));
    }
    writer.put_u32(DISTRIBUTED_SIEVE_PROTOCOL_WIRE_VERSION_V1);
    writer.put_u16(static_cast<uint16_t>(kind));
    writer.put_u16(0);
    writer.put_u32(DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);
    constexpr std::size_t DECLARED_SIZE_OFFSET = 20;
    writer.put_u32(0);
    std::visit([&writer](const auto& value) { write_payload(writer, value); }, record);
    if (!writer.status()) {
        return writer.status();
    }
    if (writer.bytes().size() >
        DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES - util::SHA256_DIGEST_BYTES) {
        return failure(DistributedSieveProtocolError::output_too_large, writer.bytes().size());
    }
    const auto total_size = writer.bytes().size() + util::SHA256_DIGEST_BYTES;
    if (total_size > std::numeric_limits<uint32_t>::max()) {
        return failure(DistributedSieveProtocolError::integer_out_of_range, DECLARED_SIZE_OFFSET);
    }
    if (!writer.patch_u32(DECLARED_SIZE_OFFSET, static_cast<uint32_t>(total_size))) {
        return writer.status();
    }
    return {};
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_record_semantics(const DistributedSieveProtocolRecordV1& record) noexcept {
    if (record.valueless_by_exception()) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    }
    return std::visit([](const auto& value) { return validate_value(value); }, record);
}

[[nodiscard]] std::optional<DistributedSieveRecordKindV1>
record_kind_from_raw(uint16_t raw) noexcept {
    switch (raw) {
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::wave_manifest):
        return DistributedSieveRecordKindV1::wave_manifest;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::attempt_started):
        return DistributedSieveRecordKindV1::attempt_started;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::chunk_terminal_failure):
        return DistributedSieveRecordKindV1::chunk_terminal_failure;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::worker_handoff):
        return DistributedSieveRecordKindV1::worker_handoff;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::merge_started):
        return DistributedSieveRecordKindV1::merge_started;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::merge_prepared):
        return DistributedSieveRecordKindV1::merge_prepared;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::wave_merge_commit):
        return DistributedSieveRecordKindV1::wave_merge_commit;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::artifact_cleanup_authorized):
        return DistributedSieveRecordKindV1::artifact_cleanup_authorized;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::artifact_cleanup_completed):
        return DistributedSieveRecordKindV1::artifact_cleanup_completed;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::consumption_started):
        return DistributedSieveRecordKindV1::consumption_started;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::successor_prepared):
        return DistributedSieveRecordKindV1::successor_prepared;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::wave_consumption_ack):
        return DistributedSieveRecordKindV1::wave_consumption_ack;
    case static_cast<uint16_t>(DistributedSieveRecordKindV1::wave_completed):
        return DistributedSieveRecordKindV1::wave_completed;
    default:
        return std::nullopt;
    }
}

template <typename Value>
[[nodiscard]] DistributedSieveProtocolDecodeResult
decode_payload_as(std::span<const std::byte> payload, uint64_t payload_offset,
                  const util::Sha256Digest& digest) {
    Reader reader(payload, payload_offset);
    Value value;
    if (!read_payload(reader, value) || !reader.finish()) {
        return {std::nullopt, reader.status()};
    }
    value.self_digest = digest;
    DistributedSieveProtocolRecordV1 record{std::move(value)};
    if (const auto status = validate_record_semantics(record); !status) {
        return {std::nullopt, status};
    }
    return {std::move(record), {}};
}

[[nodiscard]] DistributedSieveProtocolDecodeResult
decode_payload_by_kind(DistributedSieveRecordKindV1 kind, std::span<const std::byte> payload,
                       uint64_t payload_offset, const util::Sha256Digest& digest) {
    switch (kind) {
    case DistributedSieveRecordKindV1::wave_manifest:
        return decode_payload_as<WaveManifestV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::attempt_started:
        return decode_payload_as<AttemptStartedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::chunk_terminal_failure:
        return decode_payload_as<ChunkTerminalFailureV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::worker_handoff:
        return decode_payload_as<WorkerHandoffV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::merge_started:
        return decode_payload_as<MergeStartedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::merge_prepared:
        return decode_payload_as<MergePreparedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::wave_merge_commit:
        return decode_payload_as<WaveMergeCommitV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::artifact_cleanup_authorized:
        return decode_payload_as<ArtifactCleanupAuthorizedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::artifact_cleanup_completed:
        return decode_payload_as<ArtifactCleanupCompletedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::consumption_started:
        return decode_payload_as<ConsumptionStartedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::successor_prepared:
        return decode_payload_as<SuccessorPreparedV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::wave_consumption_ack:
        return decode_payload_as<WaveConsumptionAckV1>(payload, payload_offset, digest);
    case DistributedSieveRecordKindV1::wave_completed:
        return decode_payload_as<WaveCompletedV1>(payload, payload_offset, digest);
    }
    return {std::nullopt, failure(DistributedSieveProtocolError::unknown_record_kind)};
}

} // namespace distributed_sieve_protocol_detail

std::string_view
distributed_sieve_protocol_error_name(DistributedSieveProtocolError error) noexcept {
    switch (error) {
    case DistributedSieveProtocolError::none:
        return "none";
    case DistributedSieveProtocolError::input_too_large:
        return "input_too_large";
    case DistributedSieveProtocolError::output_too_large:
        return "output_too_large";
    case DistributedSieveProtocolError::truncated:
        return "truncated";
    case DistributedSieveProtocolError::trailing_bytes:
        return "trailing_bytes";
    case DistributedSieveProtocolError::invalid_magic:
        return "invalid_magic";
    case DistributedSieveProtocolError::unsupported_wire_version:
        return "unsupported_wire_version";
    case DistributedSieveProtocolError::unsupported_schema_version:
        return "unsupported_schema_version";
    case DistributedSieveProtocolError::declared_size_mismatch:
        return "declared_size_mismatch";
    case DistributedSieveProtocolError::unknown_record_kind:
        return "unknown_record_kind";
    case DistributedSieveProtocolError::unknown_enum:
        return "unknown_enum";
    case DistributedSieveProtocolError::invalid_boolean:
        return "invalid_boolean";
    case DistributedSieveProtocolError::invalid_value:
        return "invalid_value";
    case DistributedSieveProtocolError::invalid_string:
        return "invalid_string";
    case DistributedSieveProtocolError::collection_too_large:
        return "collection_too_large";
    case DistributedSieveProtocolError::duplicate_entry:
        return "duplicate_entry";
    case DistributedSieveProtocolError::noncanonical_order:
        return "noncanonical_order";
    case DistributedSieveProtocolError::range_gap:
        return "range_gap";
    case DistributedSieveProtocolError::range_overlap:
        return "range_overlap";
    case DistributedSieveProtocolError::integer_out_of_range:
        return "integer_out_of_range";
    case DistributedSieveProtocolError::digest_mismatch:
        return "digest_mismatch";
    case DistributedSieveProtocolError::digest_unavailable:
        return "digest_unavailable";
    case DistributedSieveProtocolError::resource_exhausted:
        return "resource_exhausted";
    case DistributedSieveProtocolError::record_type_mismatch:
        return "record_type_mismatch";
    }
    return "unknown";
}

std::string_view distributed_sieve_record_kind_name(DistributedSieveRecordKindV1 kind) noexcept {
    switch (kind) {
    case DistributedSieveRecordKindV1::wave_manifest:
        return "wave_manifest";
    case DistributedSieveRecordKindV1::attempt_started:
        return "attempt_started";
    case DistributedSieveRecordKindV1::chunk_terminal_failure:
        return "chunk_terminal_failure";
    case DistributedSieveRecordKindV1::worker_handoff:
        return "worker_handoff";
    case DistributedSieveRecordKindV1::merge_started:
        return "merge_started";
    case DistributedSieveRecordKindV1::merge_prepared:
        return "merge_prepared";
    case DistributedSieveRecordKindV1::wave_merge_commit:
        return "wave_merge_commit";
    case DistributedSieveRecordKindV1::artifact_cleanup_authorized:
        return "artifact_cleanup_authorized";
    case DistributedSieveRecordKindV1::artifact_cleanup_completed:
        return "artifact_cleanup_completed";
    case DistributedSieveRecordKindV1::consumption_started:
        return "consumption_started";
    case DistributedSieveRecordKindV1::successor_prepared:
        return "successor_prepared";
    case DistributedSieveRecordKindV1::wave_consumption_ack:
        return "wave_consumption_ack";
    case DistributedSieveRecordKindV1::wave_completed:
        return "wave_completed";
    }
    return "unknown";
}

DistributedSieveRecordKindV1
distributed_sieve_record_kind(const DistributedSieveProtocolRecordV1& record) {
    return std::visit(
        []<typename Value>(const Value&) {
            return distributed_sieve_protocol_detail::record_kind_for<Value>();
        },
        record);
}

DistributedSieveProtocolDigestResult
distributed_sieve_record_digest(const DistributedSieveProtocolRecordV1& record) noexcept {
    using namespace distributed_sieve_protocol_detail;
    try {
        if (const auto status = validate_record_semantics(record); !status) {
            return {std::nullopt, status};
        }
        Writer writer;
        if (const auto status = write_record_preimage(record, writer); !status) {
            return {std::nullopt, status};
        }
        auto digest = hash_domain_and_bytes(RECORD_DIGEST_DOMAIN, writer.bytes());
        if (!digest.has_value()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
        }
        return {digest, {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveProtocolStatus
validate_distributed_sieve_record(const DistributedSieveProtocolRecordV1& record,
                                  bool verify_self_digest) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_record_semantics(record); !status) {
        return status;
    }
    if (!verify_self_digest) {
        return {};
    }
    const auto digest = distributed_sieve_record_digest(record);
    if (!digest) {
        return digest.status;
    }
    const bool matches = std::visit(
        [&digest](const auto& value) { return value.self_digest == *digest.digest; }, record);
    return matches ? DistributedSieveProtocolStatus{}
                   : failure(DistributedSieveProtocolError::digest_mismatch);
}

DistributedSieveProtocolStatus
seal_distributed_sieve_record(DistributedSieveProtocolRecordV1& record) noexcept {
    const auto digest = distributed_sieve_record_digest(record);
    if (!digest) {
        return digest.status;
    }
    std::visit([&digest](auto& value) { value.self_digest = *digest.digest; }, record);
    return {};
}

DistributedSieveProtocolEncodeResult
encode_distributed_sieve_record(const DistributedSieveProtocolRecordV1& record) noexcept {
    using namespace distributed_sieve_protocol_detail;
    try {
        if (const auto status = validate_record_semantics(record); !status) {
            return {std::nullopt, status};
        }
        Writer writer;
        if (const auto status = write_record_preimage(record, writer); !status) {
            return {std::nullopt, status};
        }
        auto computed = hash_domain_and_bytes(RECORD_DIGEST_DOMAIN, writer.bytes());
        if (!computed.has_value()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
        }
        const auto& stored = std::visit(
            [](const auto& value) -> const util::Sha256Digest& { return value.self_digest; },
            record);
        if (*computed != stored) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_mismatch)};
        }
        writer.put_digest(stored);
        if (!writer.status()) {
            return {std::nullopt, writer.status()};
        }
        return {std::move(writer.bytes()), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveProtocolDecodeResult
decode_distributed_sieve_record(std::span<const std::byte> bytes) noexcept {
    using namespace distributed_sieve_protocol_detail;
    try {
        if (bytes.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::input_too_large, bytes.size())};
        }
        if (bytes.size() < FRAME_PREFIX_SIZE) {
            return {std::nullopt, failure(DistributedSieveProtocolError::truncated, bytes.size())};
        }

        Reader frame(bytes.first(FRAME_PREFIX_SIZE), 0);
        std::array<std::byte, RECORD_MAGIC.size()> magic{};
        uint32_t wire_version = 0;
        uint16_t raw_kind = 0;
        uint16_t reserved = 0;
        uint32_t schema_version = 0;
        uint32_t declared_size = 0;
        if (!frame.get_bytes(magic) || !frame.get_u32(wire_version) || !frame.get_u16(raw_kind) ||
            !frame.get_u16(reserved) || !frame.get_u32(schema_version) ||
            !frame.get_u32(declared_size)) {
            return {std::nullopt, frame.status()};
        }
        for (std::size_t index = 0; index < RECORD_MAGIC.size(); ++index) {
            if (magic[index] != static_cast<std::byte>(static_cast<uint8_t>(
                                    static_cast<unsigned char>(RECORD_MAGIC[index])))) {
                return {std::nullopt, failure(DistributedSieveProtocolError::invalid_magic, index)};
            }
        }
        if (wire_version != DISTRIBUTED_SIEVE_PROTOCOL_WIRE_VERSION_V1) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::unsupported_wire_version, 8)};
        }
        const auto kind = record_kind_from_raw(raw_kind);
        if (!kind.has_value()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::unknown_record_kind, 12)};
        }
        if (reserved != 0) {
            return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value, 14)};
        }
        if (schema_version != DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::unsupported_schema_version, 16)};
        }
        if (declared_size < MINIMUM_RECORD_SIZE ||
            declared_size > DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::declared_size_mismatch, 20)};
        }
        if (bytes.size() < declared_size) {
            return {std::nullopt, failure(DistributedSieveProtocolError::truncated, bytes.size())};
        }
        if (bytes.size() > declared_size) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::trailing_bytes, declared_size)};
        }

        const std::size_t digest_offset = declared_size - DIGEST_SIZE;
        util::Sha256Digest stored_digest;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset), DIGEST_SIZE,
                    stored_digest.bytes.begin());
        const auto payload = bytes.subspan(FRAME_PREFIX_SIZE, digest_offset - FRAME_PREFIX_SIZE);
        auto decoded = decode_payload_by_kind(*kind, payload, FRAME_PREFIX_SIZE, stored_digest);
        if (!decoded) {
            return decoded;
        }
        const auto computed =
            hash_domain_and_bytes(RECORD_DIGEST_DOMAIN, bytes.first(digest_offset));
        if (!computed.has_value()) {
            return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
        }
        if (*computed != stored_digest) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::digest_mismatch, digest_offset)};
        }
        return decoded;
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

namespace distributed_sieve_protocol_detail {

[[nodiscard]] constexpr ExecutionPolicyScalarKindV1
expected_policy_kind(ExecutionPolicyKeyV1 key) noexcept {
    switch (key) {
    case ExecutionPolicyKeyV1::lattice_lll:
    case ExecutionPolicyKeyV1::trial_div_simd:
    case ExecutionPolicyKeyV1::lattice_coords_simd:
    case ExecutionPolicyKeyV1::bucket_prefetch:
    case ExecutionPolicyKeyV1::sieve_saturated_sub_simd:
    case ExecutionPolicyKeyV1::sieve_count_above_threshold_simd:
        return ExecutionPolicyScalarKindV1::closed_mode;

    case ExecutionPolicyKeyV1::adaptive_lattice_threshold:
    case ExecutionPolicyKeyV1::survival_threshold:
        return ExecutionPolicyScalarKindV1::ieee754_binary64;

    case ExecutionPolicyKeyV1::adaptive_lattice_max_retries:
    case ExecutionPolicyKeyV1::adaptive_lattice_seed:
    case ExecutionPolicyKeyV1::ecm_bs_degree:
    case ExecutionPolicyKeyV1::ecm_sigma_pool_size:
    case ExecutionPolicyKeyV1::ecm_curve_pool:
    case ExecutionPolicyKeyV1::cofactor_batch_size:
    case ExecutionPolicyKeyV1::brent_pollard_rho_threads:
    case ExecutionPolicyKeyV1::ecm_b1_cache_size:
    case ExecutionPolicyKeyV1::ecm_stage1_parallel_threads:
    case ExecutionPolicyKeyV1::ecm_stage2_parallel:
    case ExecutionPolicyKeyV1::cofactor_result_cache_size:
    case ExecutionPolicyKeyV1::lattice_basis_parallel_threads:
    case ExecutionPolicyKeyV1::sieve_apply_tile_threads:
    case ExecutionPolicyKeyV1::sieve_ecore_threads:
    case ExecutionPolicyKeyV1::sieve_norm_tile_bits:
    case ExecutionPolicyKeyV1::sieve_region_tile_bits:
        return ExecutionPolicyScalarKindV1::unsigned_integer;

    case ExecutionPolicyKeyV1::lattice_skew:
    case ExecutionPolicyKeyV1::adaptive_lattice:
    case ExecutionPolicyKeyV1::survival_filter:
    case ExecutionPolicyKeyV1::cofactor_brent:
    case ExecutionPolicyKeyV1::ecm_brent_suyama:
    case ExecutionPolicyKeyV1::ecm_batch_inv:
    case ExecutionPolicyKeyV1::sieve_no_tiny_simd:
        return ExecutionPolicyScalarKindV1::boolean;
    }
    return ExecutionPolicyScalarKindV1::boolean;
}

[[nodiscard]] constexpr bool valid_policy_key_raw(uint16_t raw) noexcept {
    return raw >= static_cast<uint16_t>(ExecutionPolicyKeyV1::lattice_lll) &&
           raw <= static_cast<uint16_t>(ExecutionPolicyKeyV1::sieve_count_above_threshold_simd);
}

[[nodiscard]] bool valid_policy_setting(const ExecutionPolicySettingV1& setting) noexcept {
    const uint16_t raw_key = static_cast<uint16_t>(setting.key);
    if (!valid_policy_key_raw(raw_key) || setting.kind != expected_policy_kind(setting.key)) {
        return false;
    }
    switch (setting.kind) {
    case ExecutionPolicyScalarKindV1::boolean:
        return setting.canonical_bits <= 1;
    case ExecutionPolicyScalarKindV1::signed_integer:
        return false;
    case ExecutionPolicyScalarKindV1::ieee754_binary64: {
        const double value = std::bit_cast<double>(setting.canonical_bits);
        if (!std::isfinite(value)) {
            return false;
        }
        if (setting.key == ExecutionPolicyKeyV1::adaptive_lattice_threshold) {
            return value > 0.0 && value <= 100.0;
        }
        return setting.key == ExecutionPolicyKeyV1::survival_threshold && value >= 0.0 &&
               value <= 1.0;
    }
    case ExecutionPolicyScalarKindV1::closed_mode:
        if (setting.key == ExecutionPolicyKeyV1::lattice_lll) {
            return setting.canonical_bits >= 1 && setting.canonical_bits <= 2;
        }
        return setting.canonical_bits >= 1 && setting.canonical_bits <= 3;
    case ExecutionPolicyScalarKindV1::unsigned_integer:
        break;
    }

    switch (setting.key) {
    case ExecutionPolicyKeyV1::adaptive_lattice_max_retries:
        return setting.canonical_bits <= 16;
    case ExecutionPolicyKeyV1::adaptive_lattice_seed:
        return true;
    case ExecutionPolicyKeyV1::ecm_bs_degree:
        return setting.canonical_bits == 0 || setting.canonical_bits == 1 ||
               setting.canonical_bits == 2 || setting.canonical_bits == 6 ||
               setting.canonical_bits == 12 || setting.canonical_bits == 30;
    case ExecutionPolicyKeyV1::ecm_sigma_pool_size:
        return setting.canonical_bits <= 1024;
    case ExecutionPolicyKeyV1::ecm_curve_pool:
        return setting.canonical_bits == 0 ||
               (setting.canonical_bits >= 4 && setting.canonical_bits <= 1024);
    case ExecutionPolicyKeyV1::cofactor_batch_size:
        return setting.canonical_bits >= 1 && setting.canonical_bits <= 4096;
    case ExecutionPolicyKeyV1::ecm_b1_cache_size:
        return setting.canonical_bits <= 32;
    case ExecutionPolicyKeyV1::cofactor_result_cache_size:
        return setting.canonical_bits <= (UINT64_C(1) << 20U);
    case ExecutionPolicyKeyV1::sieve_norm_tile_bits:
    case ExecutionPolicyKeyV1::sieve_region_tile_bits:
        return setting.canonical_bits <= 8;
    case ExecutionPolicyKeyV1::brent_pollard_rho_threads:
    case ExecutionPolicyKeyV1::ecm_stage1_parallel_threads:
    case ExecutionPolicyKeyV1::ecm_stage2_parallel:
    case ExecutionPolicyKeyV1::lattice_basis_parallel_threads:
    case ExecutionPolicyKeyV1::sieve_apply_tile_threads:
        return setting.canonical_bits > 0 &&
               setting.canonical_bits <= std::numeric_limits<uint32_t>::max();
    case ExecutionPolicyKeyV1::sieve_ecore_threads:
        return setting.canonical_bits <= std::numeric_limits<uint32_t>::max();
    default:
        return setting.canonical_bits <= std::numeric_limits<uint32_t>::max();
    }
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_special_q_bounds(const SpecialQBoundsV1& bounds, bool require_nonempty) noexcept {
    if (bounds.end_index < bounds.start_index ||
        (require_nonempty && bounds.end_index == bounds.start_index) ||
        bounds.min_q > std::numeric_limits<uint32_t>::max() ||
        bounds.max_q > std::numeric_limits<uint32_t>::max() ||
        (bounds.max_q != 0 && bounds.max_q < bounds.min_q)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

class HashWriter final {
public:
    explicit HashWriter(std::string_view domain) noexcept {
        constexpr std::array<std::byte, 1> separator{std::byte{0}};
        if (!accumulator_.update(domain) || !accumulator_.update(separator)) {
            failed_ = true;
        }
    }

    void put_u8(uint8_t value) noexcept {
        const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
        put_bytes(bytes);
    }

    void put_u16(uint16_t value) noexcept {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<uint8_t>(value >> shift));
        }
    }

    void put_i64(int64_t value) noexcept {
        put_u64(std::bit_cast<uint64_t>(value));
    }

    void put_bool(bool value) noexcept {
        put_u8(value ? 1U : 0U);
    }

    void put_bytes(std::span<const std::byte> bytes) noexcept {
        if (!failed_ && !accumulator_.update(bytes)) {
            failed_ = true;
        }
    }

    void put_digest(const util::Sha256Digest& digest) noexcept {
        put_bytes(digest.bytes);
    }

    void put_string(std::string_view value) noexcept {
        put_u32(static_cast<uint32_t>(value.size()));
        put_bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    [[nodiscard]] std::optional<util::Sha256Digest> finish() noexcept {
        if (failed_) {
            return std::nullopt;
        }
        return accumulator_.finalize();
    }

    [[nodiscard]] bool good() const noexcept {
        return !failed_;
    }

private:
    util::Sha256Accumulator accumulator_;
    bool failed_ = false;
};

} // namespace distributed_sieve_protocol_detail

DistributedSieveProtocolStatus validate_distributed_sieve_execution_policy(
    const DistributedSieveExecutionPolicyV1& policy) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (policy.schema_version != DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1) {
        return failure(DistributedSieveProtocolError::unsupported_schema_version);
    }
    if (policy.settings.size() != DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1) {
        return failure(DistributedSieveProtocolError::collection_too_large);
    }
    for (uint32_t index = 0; index < policy.settings.size(); ++index) {
        const auto& setting = policy.settings[index];
        if (static_cast<uint16_t>(setting.key) != index + 1U) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (!valid_policy_setting(setting)) {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }
    return {};
}

DistributedSieveProtocolStatus
validate_distributed_sieve_work_identity(const DistributedSieveWorkIdentityV1& value) noexcept {
    using namespace distributed_sieve_protocol_detail;
    const auto& polynomial = value.polynomial;
    if (!canonical_integer(polynomial.n.decimal) || !canonical_integer(polynomial.m.decimal) ||
        polynomial.n.decimal == "0" || polynomial.m.decimal == "0" ||
        polynomial.n.decimal.front() == '-' || polynomial.m.decimal.front() == '-' ||
        polynomial.degree >= DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS ||
        polynomial.coefficients.size() < static_cast<std::size_t>(polynomial.degree) + 1U ||
        polynomial.coefficients.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS ||
        !positive_finite_binary64(polynomial.skewness_ieee754_bits)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    for (uint32_t index = 0; index < polynomial.coefficients.size(); ++index) {
        if (!canonical_integer(polynomial.coefficients[index].decimal)) {
            return failure(DistributedSieveProtocolError::invalid_string,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }
    if (polynomial.coefficients[polynomial.degree].decimal == "0") {
        return failure(DistributedSieveProtocolError::invalid_value,
                       DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, polynomial.degree);
    }
    for (uint32_t index = polynomial.degree + 1U; index < polynomial.coefficients.size(); ++index) {
        if (polynomial.coefficients[index].decimal != "0") {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }

    const auto& factor_base = value.factor_base;
    if (factor_base.rational_bound == 0 || factor_base.algebraic_bound == 0 ||
        factor_base.rational_bound > std::numeric_limits<uint32_t>::max() ||
        factor_base.algebraic_bound > std::numeric_limits<uint32_t>::max() ||
        factor_base.large_prime_bound == 0 || factor_base.log_scale == 0 ||
        factor_base.log_scale > std::numeric_limits<uint8_t>::max() ||
        factor_base.rational.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES ||
        factor_base.algebraic.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES ||
        factor_base.sieve_algebraic_count > factor_base.algebraic.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    uint64_t previous_rational_p = 0;
    for (uint32_t index = 0; index < factor_base.rational.size(); ++index) {
        const auto& entry = factor_base.rational[index];
        if (entry.p > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
            entry.log_p > std::numeric_limits<uint16_t>::max()) {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (entry.p <= 1 || entry.p <= previous_rational_p ||
            entry.p > factor_base.rational_bound ||
            entry.p > std::numeric_limits<uint32_t>::max()) {
            return failure(entry.p == previous_rational_p
                               ? DistributedSieveProtocolError::duplicate_entry
                               : DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        previous_rational_p = entry.p;
    }
    uint64_t previous_algebraic_p = 0;
    uint64_t previous_algebraic_r = 0;
    bool have_algebraic = false;
    for (uint32_t index = 0; index < factor_base.algebraic.size(); ++index) {
        const auto& entry = factor_base.algebraic[index];
        const bool greater = !have_algebraic || entry.p > previous_algebraic_p ||
                             (entry.p == previous_algebraic_p && entry.r > previous_algebraic_r);
        const bool in_sieve_prefix = index < factor_base.sieve_algebraic_count;
        const bool bound_partition_is_valid = in_sieve_prefix
                                                  ? entry.p <= factor_base.algebraic_bound
                                                  : entry.p > factor_base.algebraic_bound;
        const bool root_is_valid =
            entry.r == std::numeric_limits<uint32_t>::max() || entry.r < entry.p;
        const bool active_affine_entry =
            in_sieve_prefix && entry.r != std::numeric_limits<uint32_t>::max();
        if (!greater || entry.p <= 1 || entry.p > std::numeric_limits<uint32_t>::max() ||
            entry.r > std::numeric_limits<uint32_t>::max() || !root_is_valid ||
            !bound_partition_is_valid || entry.degree == 0 ||
            entry.degree > std::numeric_limits<uint8_t>::max() ||
            (active_affine_entry &&
             (entry.p > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
              entry.log_p > std::numeric_limits<uint16_t>::max()))) {
            return failure(!greater ? DistributedSieveProtocolError::noncanonical_order
                                    : DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        previous_algebraic_p = entry.p;
        previous_algebraic_r = entry.r;
        have_algebraic = true;
    }

    if (value.sieve.log_scale == 0 || value.sieve.log_scale > std::numeric_limits<uint8_t>::max() ||
        value.sieve.large_prime_bound > std::numeric_limits<uint32_t>::max() ||
        value.region.i_min < std::numeric_limits<int32_t>::min() ||
        value.region.i_max > std::numeric_limits<int32_t>::max() ||
        value.region.j_min < std::numeric_limits<int32_t>::min() ||
        value.region.j_max > std::numeric_limits<int32_t>::max() ||
        value.region.i_max < value.region.i_min || value.region.j_max < value.region.j_min) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_special_q_bounds(value.original_sq_bounds, true); !status) {
        return status;
    }
    if (const auto status = validate_special_q_bounds(value.effective_sq_bounds, true); !status) {
        return status;
    }
    const uint32_t algebraic_count = static_cast<uint32_t>(factor_base.algebraic.size());
    uint32_t resolved_begin = value.original_sq_bounds.start_index;
    if (value.original_sq_bounds.min_q > 0) {
        while (resolved_begin < algebraic_count &&
               factor_base.algebraic[resolved_begin].p < value.original_sq_bounds.min_q) {
            ++resolved_begin;
        }
    }
    uint32_t resolved_end = std::min(value.original_sq_bounds.end_index, algebraic_count);
    if (value.original_sq_bounds.max_q > 0 &&
        value.original_sq_bounds.max_q < std::numeric_limits<uint32_t>::max()) {
        uint32_t index = resolved_begin;
        while (index < resolved_end &&
               factor_base.algebraic[index].p <= value.original_sq_bounds.max_q) {
            ++index;
        }
        resolved_end = index;
    }
    if (resolved_end <= resolved_begin || value.effective_sq_bounds.start_index != resolved_begin ||
        value.effective_sq_bounds.end_index != resolved_end ||
        value.effective_sq_bounds.min_q != 0 ||
        value.effective_sq_bounds.max_q != std::numeric_limits<uint32_t>::max()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }

    const auto& distributed = value.distributed;
    if (distributed.max_worker_attempts == 0 ||
        distributed.max_worker_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        distributed.max_merge_build_attempts == 0 ||
        distributed.max_merge_build_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS ||
        distributed.max_consumption_attempts == 0 ||
        distributed.max_consumption_attempts > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_chunk_plans(distributed.chunks, distributed.worker_count,
                                                 value.effective_sq_bounds.start_index,
                                                 value.effective_sq_bounds.end_index);
        !status) {
        return status;
    }
    if (const auto status = validate_distributed_sieve_execution_policy(value.execution_policy);
        !status) {
        return status;
    }
    constexpr uint64_t canonical_lll_mode = 2;
    const auto policy_bits = [&value](ExecutionPolicyKeyV1 key) noexcept {
        const size_t index = static_cast<size_t>(static_cast<uint16_t>(key) - 1U);
        return value.execution_policy.settings[index].canonical_bits;
    };
    const bool skew_lll_enabled =
        policy_bits(ExecutionPolicyKeyV1::lattice_lll) == canonical_lll_mode &&
        policy_bits(ExecutionPolicyKeyV1::lattice_skew) != 0;
    if (skew_lll_enabled && !positive_normal_square_binary64(polynomial.skewness_ieee754_bits)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }

    const auto& versions = value.semantic_versions;
    if (versions.relation_serialization_version == 0 || versions.ooc_format_version == 0 ||
        versions.digest_version == 0 || versions.handoff_version == 0 ||
        versions.retry_policy_version == 0 || versions.chunking_version == 0 ||
        versions.completion_version == 0 || versions.deduplication_version == 0 ||
        versions.merge_policy_version == 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

DistributedSieveProtocolDigestResult
distributed_sieve_work_digest(const DistributedSieveWorkIdentityV1& identity) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }
    HashWriter writer(WORK_DIGEST_DOMAIN);
    if (!distributed_sieve_work_identity_codec_detail::emit_distributed_sieve_work_identity_v1(
            writer, identity)) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    auto digest = writer.finish();
    if (!digest.has_value()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    return {digest, {}};
}

DistributedSieveProtocolStatus
validate_manifest_work_identity(const WaveManifestV1& manifest,
                                const DistributedSieveWorkIdentityV1& identity) noexcept {
    using namespace distributed_sieve_protocol_detail;
    try {
        const DistributedSieveProtocolRecordV1 manifest_record{manifest};
        if (const auto status = validate_distributed_sieve_record(manifest_record, true); !status) {
            return status;
        }
    } catch (const std::bad_alloc&) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    } catch (...) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    }
    const auto work_digest = distributed_sieve_work_digest(identity);
    if (!work_digest) {
        return work_digest.status;
    }
    const auto& distributed = identity.distributed;
    const auto& versions = identity.semantic_versions;
    if (manifest.work_sha256 != *work_digest.digest ||
        manifest.effective_sq_begin != identity.effective_sq_bounds.start_index ||
        manifest.effective_sq_end != identity.effective_sq_bounds.end_index ||
        manifest.worker_count != distributed.worker_count ||
        manifest.chunks != distributed.chunks ||
        manifest.sq_cap_per_worker != distributed.sq_cap_per_worker ||
        manifest.relation_cap_per_worker != distributed.relation_cap_per_worker ||
        manifest.max_worker_attempts != distributed.max_worker_attempts ||
        manifest.max_merge_build_attempts != distributed.max_merge_build_attempts ||
        manifest.max_consumption_attempts != distributed.max_consumption_attempts ||
        manifest.ooc_format_version != versions.ooc_format_version ||
        manifest.relation_serialization_version != versions.relation_serialization_version ||
        manifest.handoff_version != versions.handoff_version ||
        manifest.digest_version != versions.digest_version ||
        manifest.retry_policy_version != versions.retry_policy_version ||
        manifest.merge_policy_version != versions.merge_policy_version) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

DistributedSieveProtocolStatus validate_manifest_executable_identity(
    const WaveManifestV1& manifest, const util::Sha256Digest& expected_executable_sha256) noexcept {
    using namespace distributed_sieve_protocol_detail;
    try {
        const DistributedSieveProtocolRecordV1 manifest_record{manifest};
        if (const auto status = validate_distributed_sieve_record(manifest_record, true); !status) {
            return status;
        }
    } catch (const std::bad_alloc&) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    } catch (...) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    }
    return manifest.executable_sha256 == expected_executable_sha256
               ? DistributedSieveProtocolStatus{}
               : failure(DistributedSieveProtocolError::invalid_value);
}

DistributedSieveProtocolDigestResult derive_distributed_sieve_deterministic_seed(
    const DeterministicRandomSeedRequestV1& request) noexcept {
    using namespace distributed_sieve_protocol_detail;
    switch (request.domain) {
    case DeterministicRandomDomainV1::adaptive_lattice:
    case DeterministicRandomDomainV1::ecm_sigma:
    case DeterministicRandomDomainV1::ecm_curve:
    case DeterministicRandomDomainV1::pollard_rho:
    case DeterministicRandomDomainV1::cofactor_choice:
        break;
    default:
        return {std::nullopt, failure(DistributedSieveProtocolError::unknown_enum)};
    }
    if (request.algorithm_identity == 0) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value)};
    }
    HashWriter writer(RANDOM_SEED_DOMAIN);
    writer.put_u32(DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);
    writer.put_digest(request.work_digest);
    writer.put_u8(static_cast<uint8_t>(request.domain));
    writer.put_u32(request.chunk_id);
    writer.put_u32(request.sq_index);
    writer.put_u64(request.candidate_ordinal);
    writer.put_u32(request.algorithm_identity);
    writer.put_digest(request.cofactor_input_digest);
    auto digest = writer.finish();
    if (!digest.has_value()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    return {digest, {}};
}

namespace distributed_sieve_protocol_detail {

constexpr std::string_view ATTEMPT_CHAIN_DIGEST_DOMAIN = "GNFS-DISTRIBUTED-SIEVE-ATTEMPT-CHAIN-V1";

template <typename Value>
[[nodiscard]] DistributedSieveProtocolStatus validate_sealed_copy(const Value& value) noexcept {
    try {
        const DistributedSieveProtocolRecordV1 record{value};
        return validate_distributed_sieve_record(record, true);
    } catch (const std::bad_alloc&) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    } catch (...) {
        return failure(DistributedSieveProtocolError::resource_exhausted);
    }
}

[[nodiscard]] bool same_terminal_inputs(const std::vector<TerminalChunkInputV1>& left,
                                        const std::vector<TerminalChunkInputV1>& right) noexcept {
    return left == right;
}

[[nodiscard]] bool commit_matches_prepared(const WaveMergeCommitV1& commit,
                                           const MergePreparedV1& prepared) noexcept {
    if (commit.chunks.size() != prepared.ordered_inputs.size() ||
        prepared.per_chunk_retained_counts.size() != prepared.ordered_inputs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < commit.chunks.size(); ++index) {
        if (commit.chunks[index].input != prepared.ordered_inputs[index] ||
            commit.chunks[index].retained_relation_count !=
                prepared.per_chunk_retained_counts[index].retained_relation_count) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] DistributedSieveProtocolStatus
validate_completion_against_manifest(const WaveManifestV1& manifest,
                                     const TerminalChunkInputV1& input) noexcept {
    if (input.next_sq_index == input.sq_end) {
        const auto expected = input.raw_relation_count == 0
                                  ? WorkerCompletionReasonV1::zero_relations
                                  : WorkerCompletionReasonV1::range_exhausted;
        return input.completion_reason == expected
                   ? DistributedSieveProtocolStatus{}
                   : failure(DistributedSieveProtocolError::invalid_value);
    }
    if (input.completion_reason == WorkerCompletionReasonV1::sq_cap) {
        return manifest.sq_cap_per_worker > 0 &&
                       input.processed_sq_count == manifest.sq_cap_per_worker
                   ? DistributedSieveProtocolStatus{}
                   : failure(DistributedSieveProtocolError::invalid_value);
    }
    if (input.completion_reason == WorkerCompletionReasonV1::relation_cap) {
        const bool relation_cap_reached =
            manifest.relation_cap_per_worker > 0 &&
            input.raw_relation_count >= manifest.relation_cap_per_worker;
        const bool sq_cap_has_priority = manifest.sq_cap_per_worker > 0 &&
                                         input.processed_sq_count >= manifest.sq_cap_per_worker;
        return relation_cap_reached && !sq_cap_has_priority
                   ? DistributedSieveProtocolStatus{}
                   : failure(DistributedSieveProtocolError::invalid_value);
    }
    return failure(DistributedSieveProtocolError::invalid_value);
}

} // namespace distributed_sieve_protocol_detail

bool distributed_sieve_worker_attempt_relative_stem_matches(
    std::string_view chunk_relative_artifact_stem, uint32_t attempt_ordinal,
    std::string_view candidate) noexcept {
    return distributed_sieve_protocol_detail::worker_attempt_relative_stem_matches(
        chunk_relative_artifact_stem, attempt_ordinal, candidate);
}

DistributedSieveProtocolDigestResult
distributed_sieve_attempt_chain_digest(const util::Sha256Digest& manifest_digest,
                                       std::span<const AttemptStartedV1> attempts) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (attempts.size() > DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS) {
        return {std::nullopt, failure(DistributedSieveProtocolError::collection_too_large)};
    }
    util::Sha256Digest predecessor = manifest_digest;
    for (uint32_t index = 0; index < attempts.size(); ++index) {
        const auto& attempt = attempts[index];
        if (const auto status = validate_sealed_copy(attempt); !status) {
            return {std::nullopt, failure(status.error, status.byte_offset, index)};
        }
        if (attempt.attempt_ordinal != index || attempt.predecessor_digest != predecessor ||
            attempt.manifest_digest != manifest_digest) {
            return {std::nullopt, failure(DistributedSieveProtocolError::noncanonical_order,
                                          DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index)};
        }
        predecessor = attempt.self_digest;
    }
    HashWriter writer(ATTEMPT_CHAIN_DIGEST_DOMAIN);
    writer.put_u32(DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1);
    writer.put_digest(manifest_digest);
    writer.put_u32(static_cast<uint32_t>(attempts.size()));
    for (uint32_t index = 0; index < attempts.size(); ++index) {
        writer.put_u32(index);
        writer.put_digest(attempts[index].self_digest);
    }
    auto digest = writer.finish();
    if (!digest.has_value()) {
        return {std::nullopt, failure(DistributedSieveProtocolError::digest_unavailable)};
    }
    return {digest, {}};
}

DistributedSieveProtocolStatus validate_worker_attempt_chain(
    const WaveManifestV1& manifest, uint32_t chunk_id, std::span<const AttemptStartedV1> attempts,
    const WorkerHandoffV1* handoff, const ChunkTerminalFailureV1* terminal_failure) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(manifest); !status) {
        return status;
    }
    if (chunk_id >= manifest.chunks.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    const ChunkPlanV1& chunk = manifest.chunks[chunk_id];
    if (chunk.sq_begin == chunk.sq_end) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (attempts.size() > manifest.max_worker_attempts ||
        (handoff != nullptr && terminal_failure != nullptr) ||
        ((handoff != nullptr || terminal_failure != nullptr) && attempts.empty())) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }

    util::Sha256Digest predecessor = manifest.self_digest;
    for (uint32_t index = 0; index < attempts.size(); ++index) {
        const auto& attempt = attempts[index];
        if (const auto status = validate_sealed_copy(attempt); !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (attempt.manifest_digest != manifest.self_digest || attempt.chunk_id != chunk_id ||
            attempt.sq_begin != chunk.sq_begin || attempt.sq_end != chunk.sq_end ||
            attempt.attempt_ordinal != index || attempt.predecessor_digest != predecessor ||
            attempt.retry_policy_version != manifest.retry_policy_version ||
            !worker_attempt_relative_stem_matches(chunk.relative_artifact_stem,
                                                  attempt.attempt_ordinal,
                                                  attempt.lease.relative_stem)) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (manifest_control_conflicts_with_lease(manifest, attempt.lease)) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (lease_identities_conflict(attempt.lease, attempts[previous].lease)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        predecessor = attempt.self_digest;
    }

    if (handoff != nullptr) {
        if (const auto status = validate_sealed_copy(*handoff); !status) {
            return status;
        }
        if (manifest_control_conflicts_with_bundle(manifest, handoff->lease, handoff->artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
        const auto& attempt = attempts.back();
        if (handoff->manifest_digest != manifest.self_digest ||
            handoff->work_digest != manifest.work_sha256 || handoff->wave_id != manifest.wave_id ||
            handoff->chunk_id != chunk_id || handoff->sq_begin != chunk.sq_begin ||
            handoff->sq_end != chunk.sq_end ||
            handoff->attempt_ordinal != attempt.attempt_ordinal ||
            handoff->attempt_started_digest != attempt.self_digest ||
            handoff->lease != attempt.lease ||
            handoff->artifact.descriptor.format_version != manifest.ooc_format_version) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        TerminalChunkInputV1 projection;
        projection.chunk_id = handoff->chunk_id;
        projection.disposition = ChunkDispositionV1::handoff;
        projection.sq_begin = handoff->sq_begin;
        projection.sq_end = handoff->sq_end;
        projection.next_sq_index = handoff->next_sq_index;
        projection.processed_sq_count = handoff->processed_sq_count;
        projection.completion_reason = handoff->completion_reason;
        projection.raw_relation_count = handoff->relation_count;
        if (const auto status = validate_completion_against_manifest(manifest, projection);
            !status) {
            return status;
        }
    }
    if (terminal_failure != nullptr) {
        if (attempts.size() != manifest.max_worker_attempts) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (const auto status = validate_sealed_copy(*terminal_failure); !status) {
            return status;
        }
        const auto chain_digest =
            distributed_sieve_attempt_chain_digest(manifest.self_digest, attempts);
        if (!chain_digest) {
            return chain_digest.status;
        }
        if (terminal_failure->manifest_digest != manifest.self_digest ||
            terminal_failure->chunk_id != chunk_id ||
            terminal_failure->sq_begin != chunk.sq_begin ||
            terminal_failure->sq_end != chunk.sq_end ||
            terminal_failure->exhausted_attempt_count != attempts.size() ||
            terminal_failure->last_attempt_digest != attempts.back().self_digest ||
            terminal_failure->predecessor_chain_digest != *chain_digest.digest) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
    }
    return {};
}

DistributedSieveProtocolStatus validate_terminal_chunk_projection(
    const WaveManifestV1& manifest, uint32_t chunk_id, std::span<const AttemptStartedV1> attempts,
    const WorkerHandoffV1* handoff, const TerminalChunkInputV1& projection) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(manifest); !status) {
        return status;
    }
    if (chunk_id >= manifest.chunks.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    const auto& chunk = manifest.chunks[chunk_id];
    if (chunk.sq_begin == chunk.sq_end) {
        if (!attempts.empty() || handoff != nullptr || projection.chunk_id != chunk_id ||
            projection.disposition != ChunkDispositionV1::empty ||
            projection.sq_begin != chunk.sq_begin || projection.sq_end != chunk.sq_end) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        return validate_terminal_input(projection);
    }
    if (handoff == nullptr) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status =
            validate_worker_attempt_chain(manifest, chunk_id, attempts, handoff, nullptr);
        !status) {
        return status;
    }
    if (projection.chunk_id != chunk_id || projection.disposition != ChunkDispositionV1::handoff ||
        projection.sq_begin != handoff->sq_begin || projection.sq_end != handoff->sq_end ||
        projection.next_sq_index != handoff->next_sq_index ||
        projection.processed_sq_count != handoff->processed_sq_count ||
        projection.completion_reason != handoff->completion_reason ||
        projection.durable_attempt_count != attempts.size() ||
        projection.last_attempt_digest != attempts.back().self_digest ||
        projection.lease_id != handoff->lease.lease_id ||
        projection.handoff_digest != handoff->self_digest ||
        projection.raw_relation_count != handoff->relation_count ||
        projection.sequence_receipt != handoff->artifact.sequence_receipt ||
        projection.corpus_sha256 != handoff->artifact.corpus_sha256) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_terminal_input(projection); !status) {
        return status;
    }
    return validate_completion_against_manifest(manifest, projection);
}

DistributedSieveProtocolStatus validate_merge_predecessor_chain(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> starts,
    const MergePreparedV1* prepared, const WaveMergeCommitV1* commit) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(manifest); !status) {
        return status;
    }
    if (starts.size() > manifest.max_merge_build_attempts ||
        ((prepared != nullptr || commit != nullptr) && starts.empty()) ||
        (commit != nullptr && prepared == nullptr)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    util::Sha256Digest predecessor = manifest.self_digest;
    for (uint32_t index = 0; index < starts.size(); ++index) {
        const auto& start = starts[index];
        if (const auto status = validate_sealed_copy(start); !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (start.manifest_digest != manifest.self_digest ||
            start.work_digest != manifest.work_sha256 ||
            start.merge_policy_version != manifest.merge_policy_version ||
            start.merge_attempt_ordinal != index || start.predecessor_digest != predecessor ||
            start.ordered_inputs.size() != manifest.chunks.size()) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (manifest_control_conflicts_with_lease(manifest, start.merged_lease)) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (std::size_t chunk_index = 0; chunk_index < start.ordered_inputs.size();
             ++chunk_index) {
            const auto& input = start.ordered_inputs[chunk_index];
            const auto& plan = manifest.chunks[chunk_index];
            if (input.chunk_id != plan.chunk_id || input.sq_begin != plan.sq_begin ||
                input.sq_end != plan.sq_end) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
            if (const auto status = validate_completion_against_manifest(manifest, input);
                !status) {
                return failure(status.error, status.byte_offset, index);
            }
        }
        if (index > 0 &&
            !same_terminal_inputs(start.ordered_inputs, starts.front().ordered_inputs)) {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (lease_identities_conflict(start.merged_lease, starts[previous].merged_lease)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        predecessor = start.self_digest;
    }
    if (prepared != nullptr) {
        if (const auto status = validate_sealed_copy(*prepared); !status) {
            return status;
        }
        const auto& start = starts.back();
        if (prepared->manifest_digest != manifest.self_digest ||
            prepared->work_digest != manifest.work_sha256 ||
            prepared->merge_policy_version != manifest.merge_policy_version ||
            prepared->merge_started_digest != start.self_digest ||
            prepared->merged_lease != start.merged_lease ||
            prepared->merged_artifact.descriptor.format_version != manifest.ooc_format_version ||
            !same_terminal_inputs(prepared->ordered_inputs, start.ordered_inputs)) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (manifest_control_conflicts_with_bundle(manifest, prepared->merged_lease,
                                                   prepared->merged_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
        for (uint32_t index = 0; index < starts.size(); ++index) {
            if (lease_and_artifact_conflict(starts[index].merged_lease,
                                            prepared->merged_artifact)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
    }
    if (commit != nullptr) {
        if (const auto status = validate_sealed_copy(*commit); !status) {
            return status;
        }
        if (commit->manifest_digest != manifest.self_digest ||
            commit->work_digest != manifest.work_sha256 ||
            commit->merge_policy_version != manifest.merge_policy_version ||
            commit->merge_prepared_digest != prepared->self_digest ||
            commit->merged_lease != prepared->merged_lease ||
            commit->merged_artifact != prepared->merged_artifact ||
            commit->merged_artifact.descriptor.format_version != manifest.ooc_format_version ||
            commit->input_relation_count != prepared->input_relation_count ||
            commit->duplicate_relation_count != prepared->duplicate_relation_count ||
            commit->output_relation_count != prepared->output_relation_count ||
            !commit_matches_prepared(*commit, *prepared)) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (manifest_control_conflicts_with_bundle(manifest, commit->merged_lease,
                                                   commit->merged_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
    }
    return {};
}

DistributedSieveProtocolStatus validate_merge_dependency_chain(
    const WaveManifestV1& manifest, std::span<const ChunkTerminalEvidenceViewV1> terminal_evidence,
    std::span<const MergeStartedV1> starts, const MergePreparedV1* prepared,
    const WaveMergeCommitV1* commit) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (starts.empty() || terminal_evidence.size() != manifest.chunks.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (const auto status = validate_merge_predecessor_chain(manifest, starts, prepared, commit);
        !status) {
        return status;
    }
    if (commit != nullptr) {
        for (std::uint32_t index = 0; index < commit->chunks.size(); ++index) {
            const auto& diagnostic = commit->chunks[index].diagnostic;
            if (diagnostic.kind != NormalizedDiagnosticKindV1::none || diagnostic.code != 0) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
    }
    if (starts.front().ordered_inputs.size() != terminal_evidence.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    for (uint32_t index = 0; index < terminal_evidence.size(); ++index) {
        const auto& evidence = terminal_evidence[index];
        if (const auto status = validate_terminal_chunk_projection(
                manifest, index, evidence.attempts, evidence.handoff,
                starts.front().ordered_inputs[index]);
            !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (evidence.handoff == nullptr) {
            continue;
        }
        const auto& handoff = *evidence.handoff;
        for (uint32_t previous = 0; previous < index; ++previous) {
            const auto* previous_handoff = terminal_evidence[previous].handoff;
            if (previous_handoff != nullptr &&
                artifact_bundles_conflict(handoff.lease, handoff.artifact, previous_handoff->lease,
                                          previous_handoff->artifact)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        for (const auto& start : starts) {
            if (lease_identities_conflict(handoff.lease, start.merged_lease) ||
                lease_and_artifact_conflict(start.merged_lease, handoff.artifact)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        if (prepared != nullptr &&
            artifact_bundles_conflict(handoff.lease, handoff.artifact, prepared->merged_lease,
                                      prepared->merged_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }
    return {};
}

DistributedSieveProtocolStatus validate_consumption_predecessor_chain(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::span<const ConsumptionStartedV1> starts, const SuccessorPreparedV1* prepared,
    const WaveConsumptionAckV1* ack) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(manifest); !status) {
        return status;
    }
    if (const auto status = validate_sealed_copy(commit); !status) {
        return status;
    }
    if (commit.manifest_digest != manifest.self_digest ||
        commit.work_digest != manifest.work_sha256 ||
        commit.merged_artifact.descriptor.format_version != manifest.ooc_format_version ||
        starts.size() > manifest.max_consumption_attempts ||
        ((prepared != nullptr || ack != nullptr) && starts.empty()) ||
        (ack != nullptr && prepared == nullptr)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (manifest_control_conflicts_with_bundle(manifest, commit.merged_lease,
                                               commit.merged_artifact)) {
        return failure(DistributedSieveProtocolError::duplicate_entry);
    }

    util::Sha256Digest predecessor = manifest.self_digest;
    for (uint32_t index = 0; index < starts.size(); ++index) {
        const auto& start = starts[index];
        if (const auto status = validate_sealed_copy(start); !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (start.merge_commit_digest != commit.self_digest ||
            start.manifest_digest != manifest.self_digest ||
            start.execution_contract_version != manifest.execution_contract_version ||
            start.consumption_attempt_ordinal != index || start.predecessor_digest != predecessor) {
            return failure(DistributedSieveProtocolError::noncanonical_order,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (manifest_control_conflicts_with_lease(manifest, start.successor_lease)) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (lease_identities_conflict(start.successor_lease,
                                          starts[previous].successor_lease)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        if (lease_identities_conflict(start.successor_lease, commit.merged_lease) ||
            lease_and_artifact_conflict(start.successor_lease, commit.merged_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        predecessor = start.self_digest;
    }
    if (prepared != nullptr) {
        if (const auto status = validate_sealed_copy(*prepared); !status) {
            return status;
        }
        const auto& start = starts.back();
        if (prepared->consumption_started_digest != start.self_digest ||
            prepared->successor_lease != start.successor_lease ||
            prepared->successor_artifact.descriptor.format_version !=
                start.successor_format_version ||
            prepared->input_relation_count != commit.output_relation_count) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (manifest_control_conflicts_with_bundle(manifest, prepared->successor_lease,
                                                   prepared->successor_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
        for (uint32_t index = 0; index < starts.size(); ++index) {
            if (lease_and_artifact_conflict(starts[index].successor_lease,
                                            prepared->successor_artifact)) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        if (artifact_bundles_conflict(commit.merged_lease, commit.merged_artifact,
                                      prepared->successor_lease, prepared->successor_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
    }
    if (ack != nullptr) {
        if (const auto status = validate_sealed_copy(*ack); !status) {
            return status;
        }
        const auto& start = starts.back();
        if (ack->merge_commit_digest != commit.self_digest ||
            ack->consumer_kind != start.consumer_kind ||
            ack->consumption_started_digest != start.self_digest ||
            ack->successor_prepared_digest != prepared->self_digest ||
            ack->successor_artifact != prepared->successor_artifact ||
            ack->successor_semantic_digest != prepared->successor_semantic_digest ||
            ack->successor_cleanup_authority_identity != start.successor_lease.owner_marker) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (manifest_control_conflicts_with_artifact(manifest, ack->successor_artifact) ||
            manifest_control_contains_identity(manifest,
                                               ack->successor_cleanup_authority_identity)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
    }
    return {};
}

DistributedSieveProtocolStatus validate_artifact_cleanup_dependencies(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::span<const ConsumptionStartedV1> consumption_starts,
    const SuccessorPreparedV1* successor_prepared, const WaveConsumptionAckV1* ack,
    const ArtifactCleanupAuthorizedV1& authorization, const WorkerHandoffV1* worker_handoff,
    const MergePreparedV1* merge_prepared) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(manifest); !status) {
        return status;
    }
    if (const auto status = validate_sealed_copy(commit); !status) {
        return status;
    }
    if (const auto status = validate_sealed_copy(authorization); !status) {
        return status;
    }
    if (commit.manifest_digest != manifest.self_digest ||
        commit.work_digest != manifest.work_sha256 ||
        commit.merged_artifact.descriptor.format_version != manifest.ooc_format_version ||
        authorization.manifest_digest != manifest.self_digest) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (manifest_control_conflicts_with_bundle(manifest, commit.merged_lease,
                                               commit.merged_artifact) ||
        manifest_control_conflicts_with_cleanup_authorization(manifest, authorization)) {
        return failure(DistributedSieveProtocolError::duplicate_entry);
    }

    switch (authorization.authorizer) {
    case CleanupAuthorizerKindV1::merge_commit_worker: {
        if (authorization.artifact_kind != CleanupArtifactKindV1::worker ||
            authorization.authorizer_record_digest != commit.self_digest ||
            authorization.manifest_order_ordinal >= commit.chunks.size() ||
            !consumption_starts.empty() || successor_prepared != nullptr || ack != nullptr ||
            worker_handoff == nullptr || merge_prepared != nullptr) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        const auto& summary = commit.chunks[authorization.manifest_order_ordinal].input;
        if (summary.disposition != ChunkDispositionV1::handoff ||
            authorization.lease.lease_id != summary.lease_id ||
            authorization.handoff_digest != summary.handoff_digest ||
            authorization.artifact.descriptor.relation_count != summary.raw_relation_count ||
            authorization.artifact.sequence_receipt != summary.sequence_receipt ||
            authorization.artifact.corpus_sha256 != summary.corpus_sha256) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (const auto status = validate_sealed_copy(*worker_handoff); !status) {
            return status;
        }
        if (manifest_control_conflicts_with_bundle(manifest, worker_handoff->lease,
                                                   worker_handoff->artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
        if (worker_handoff->self_digest != summary.handoff_digest ||
            worker_handoff->manifest_digest != manifest.self_digest ||
            worker_handoff->work_digest != manifest.work_sha256 ||
            worker_handoff->wave_id != manifest.wave_id ||
            worker_handoff->chunk_id != summary.chunk_id ||
            worker_handoff->sq_begin != summary.sq_begin ||
            worker_handoff->sq_end != summary.sq_end ||
            worker_handoff->next_sq_index != summary.next_sq_index ||
            worker_handoff->processed_sq_count != summary.processed_sq_count ||
            worker_handoff->completion_reason != summary.completion_reason ||
            worker_handoff->attempt_ordinal + 1U != summary.durable_attempt_count ||
            worker_handoff->attempt_started_digest != summary.last_attempt_digest ||
            worker_handoff->lease != authorization.lease ||
            worker_handoff->artifact != authorization.artifact ||
            worker_handoff->artifact.descriptor.format_version != manifest.ooc_format_version ||
            worker_handoff->relation_count != summary.raw_relation_count) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    }
    case CleanupAuthorizerKindV1::consumption_ack_merged:
        if (authorization.artifact_kind != CleanupArtifactKindV1::merged ||
            authorization.manifest_order_ordinal != 0 || ack == nullptr ||
            successor_prepared == nullptr || consumption_starts.empty() ||
            merge_prepared == nullptr ||
            authorization.authorizer_record_digest != ack->self_digest ||
            authorization.handoff_digest != commit.merge_prepared_digest ||
            authorization.lease != commit.merged_lease ||
            authorization.artifact != commit.merged_artifact || worker_handoff != nullptr) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        if (const auto status = validate_consumption_predecessor_chain(
                manifest, commit, consumption_starts, successor_prepared, ack);
            !status) {
            return status;
        }
        if (const auto status = validate_sealed_copy(*merge_prepared); !status) {
            return status;
        }
        if (manifest_control_conflicts_with_bundle(manifest, merge_prepared->merged_lease,
                                                   merge_prepared->merged_artifact)) {
            return failure(DistributedSieveProtocolError::duplicate_entry);
        }
        if (merge_prepared->self_digest != commit.merge_prepared_digest ||
            merge_prepared->manifest_digest != manifest.self_digest ||
            merge_prepared->work_digest != manifest.work_sha256 ||
            merge_prepared->merged_lease != authorization.lease ||
            merge_prepared->merged_artifact != authorization.artifact) {
            return failure(DistributedSieveProtocolError::invalid_value);
        }
        break;
    }
    return {};
}

DistributedSieveProtocolStatus validate_artifact_cleanup_completion_dependency(
    const ArtifactCleanupAuthorizedV1& authorization,
    const ArtifactCleanupCompletedV1& completion) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_sealed_copy(authorization); !status) {
        return status;
    }
    if (const auto status = validate_sealed_copy(completion); !status) {
        return status;
    }
    if (completion.authorization_digest != authorization.self_digest) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (completion.cleanup_intent_identity.has_value() &&
        cleanup_authorization_contains_native_identity(authorization,
                                                       *completion.cleanup_intent_identity)) {
        return failure(DistributedSieveProtocolError::duplicate_entry);
    }
    return {};
}

DistributedSieveProtocolStatus validate_wave_completion_dependencies(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    const SuccessorPreparedV1& successor, const WaveConsumptionAckV1& ack,
    std::span<const ConsumptionStartedV1> consumption_starts,
    std::span<const ArtifactCleanupAuthorizedV1> cleanup_authorizations,
    std::span<const ArtifactCleanupCompletedV1> cleanup_completions,
    const WaveCompletedV1& completed) noexcept {
    using namespace distributed_sieve_protocol_detail;
    if (const auto status = validate_consumption_predecessor_chain(
            manifest, commit, consumption_starts, &successor, &ack);
        !status) {
        return status;
    }
    if (const auto status = validate_sealed_copy(completed); !status) {
        return status;
    }
    if (cleanup_authorizations.size() != completed.cleanup_confirmations.size() ||
        cleanup_completions.size() != completed.cleanup_confirmations.size()) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    for (uint32_t index = 0; index < cleanup_completions.size(); ++index) {
        const auto& authorization = cleanup_authorizations[index];
        const auto& cleanup = cleanup_completions[index];
        const auto& confirmation = completed.cleanup_confirmations[index];
        if (const auto status =
                validate_artifact_cleanup_completion_dependency(authorization, cleanup);
            !status) {
            return failure(status.error, status.byte_offset, index);
        }
        if (manifest_control_conflicts_with_cleanup_authorization(manifest, authorization) ||
            (cleanup.cleanup_intent_identity.has_value() &&
             manifest_control_contains_identity(manifest, *cleanup.cleanup_intent_identity))) {
            return failure(DistributedSieveProtocolError::duplicate_entry,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (authorization.self_digest == cleanup_authorizations[previous].self_digest ||
                cleanup.self_digest == cleanup_completions[previous].self_digest ||
                cleanup_authorization_bindings_conflict(authorization,
                                                        cleanup_authorizations[previous])) {
                return failure(DistributedSieveProtocolError::duplicate_entry,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        }
        if (authorization.artifact_kind != confirmation.artifact_kind ||
            authorization.manifest_order_ordinal != confirmation.manifest_order_ordinal ||
            authorization.self_digest != confirmation.authorization_digest ||
            cleanup.authorization_digest != confirmation.authorization_digest ||
            cleanup.self_digest != confirmation.completion_digest ||
            authorization.manifest_digest != manifest.self_digest) {
            return failure(DistributedSieveProtocolError::invalid_value,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
        if (confirmation.artifact_kind == CleanupArtifactKindV1::worker) {
            if (authorization.authorizer != CleanupAuthorizerKindV1::merge_commit_worker ||
                authorization.authorizer_record_digest != commit.self_digest ||
                confirmation.manifest_order_ordinal >= commit.chunks.size()) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
            const auto& input = commit.chunks[confirmation.manifest_order_ordinal].input;
            if (input.disposition != ChunkDispositionV1::handoff ||
                authorization.lease.lease_id != input.lease_id ||
                authorization.handoff_digest != input.handoff_digest ||
                authorization.artifact.descriptor.relation_count != input.raw_relation_count ||
                authorization.artifact.descriptor.format_version != manifest.ooc_format_version ||
                authorization.artifact.sequence_receipt != input.sequence_receipt ||
                authorization.artifact.corpus_sha256 != input.corpus_sha256) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        } else if (confirmation.artifact_kind == CleanupArtifactKindV1::merged) {
            if (authorization.authorizer != CleanupAuthorizerKindV1::consumption_ack_merged ||
                authorization.authorizer_record_digest != ack.self_digest ||
                confirmation.manifest_order_ordinal != 0 ||
                authorization.handoff_digest != commit.merge_prepared_digest ||
                authorization.lease != commit.merged_lease ||
                authorization.artifact != commit.merged_artifact) {
                return failure(DistributedSieveProtocolError::invalid_value,
                               DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
            }
        } else {
            return failure(DistributedSieveProtocolError::unknown_enum,
                           DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, index);
        }
    }
    if (commit.manifest_digest != manifest.self_digest ||
        commit.work_digest != manifest.work_sha256 ||
        successor.consumption_started_digest != ack.consumption_started_digest ||
        successor.input_relation_count != commit.output_relation_count ||
        successor.self_digest != ack.successor_prepared_digest ||
        ack.merge_commit_digest != commit.self_digest ||
        ack.successor_artifact != successor.successor_artifact ||
        ack.successor_semantic_digest != successor.successor_semantic_digest ||
        completed.wave_root_identity != manifest.wave_root_identity ||
        completed.permanent_lock_identity != manifest.permanent_lock_identity ||
        completed.manifest_digest != manifest.self_digest ||
        completed.merge_commit_digest != commit.self_digest ||
        completed.consumption_ack_digest != ack.self_digest ||
        completed.successor_prepared_digest != successor.self_digest ||
        completed.chunks != commit.chunks ||
        completed.successor_artifact != successor.successor_artifact ||
        completed.successor_semantic_digest != successor.successor_semantic_digest) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return {};
}

} // namespace gnfs::sieve
