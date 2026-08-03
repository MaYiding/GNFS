#include "distributed_sieve_work_identity_codec_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_work_identity_codec_detail {
namespace {

[[nodiscard]] constexpr DistributedSieveProtocolStatus
failure(DistributedSieveProtocolError error,
        std::uint64_t byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET,
        std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {error, byte_offset, element_index};
}

class CountingSink final {
public:
    void put_u8(std::uint8_t) noexcept {
        add(1);
    }

    void put_u16(std::uint16_t) noexcept {
        add(2);
    }

    void put_u32(std::uint32_t) noexcept {
        add(4);
    }

    void put_u64(std::uint64_t) noexcept {
        add(8);
    }

    void put_i64(std::int64_t) noexcept {
        add(8);
    }

    void put_bool(bool) noexcept {
        add(1);
    }

    void put_string(const std::string& value) noexcept {
        add(UINT64_C(4) + static_cast<std::uint64_t>(value.size()));
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] DistributedSieveProtocolStatus status() const noexcept {
        return status_;
    }

private:
    void add(std::uint64_t amount) noexcept {
        if (!status_) {
            return;
        }
        if (amount > DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES ||
            size_ > DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES - amount) {
            status_ = failure(DistributedSieveProtocolError::output_too_large, size_);
            return;
        }
        size_ += amount;
    }

public:
    [[nodiscard]] bool good() const noexcept {
        return static_cast<bool>(status_);
    }

private:
    std::uint64_t size_ = 0;
    DistributedSieveProtocolStatus status_;
};

class VectorSink final {
public:
    explicit VectorSink(std::size_t expected_size) {
        bytes_.reserve(expected_size);
    }

    void put_u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void put_u16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            put_u8(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void put_i64(std::int64_t value) {
        put_u64(std::bit_cast<std::uint64_t>(value));
    }

    void put_bool(bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_string(const std::string& value) {
        put_u32(static_cast<std::uint32_t>(value.size()));
        const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::vector<std::byte> take_bytes() noexcept {
        return std::move(bytes_);
    }

    [[nodiscard]] constexpr bool good() const noexcept {
        return true;
    }

private:
    std::vector<std::byte> bytes_;
};

class CanonicalDecodeArchive final {
public:
    static constexpr bool is_decoding = true;

    explicit CanonicalDecodeArchive(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool schema(std::uint32_t expected) noexcept {
        const std::uint64_t field_offset = offset();
        std::uint32_t actual = 0;
        if (!u32(actual)) {
            return false;
        }
        if (actual != expected) {
            return reject_at(DistributedSieveProtocolError::unsupported_schema_version,
                             field_offset);
        }
        return true;
    }

    [[nodiscard]] bool tag(std::uint8_t expected) noexcept {
        const std::uint64_t field_offset = offset();
        std::uint8_t actual = 0;
        if (!u8(actual)) {
            return false;
        }
        if (actual != expected) {
            return reject_at(DistributedSieveProtocolError::invalid_value, field_offset);
        }
        return true;
    }

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept {
        if (!take(1)) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 16; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint16_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }

    [[nodiscard]] bool i64(std::int64_t& value) noexcept {
        std::uint64_t bits = 0;
        if (!u64(bits)) {
            return false;
        }
        value = std::bit_cast<std::int64_t>(bits);
        return true;
    }

    [[nodiscard]] bool boolean(bool& value) noexcept {
        const std::uint64_t field_offset = offset();
        std::uint8_t raw = 0;
        if (!u8(raw)) {
            return false;
        }
        if (raw > 1U) {
            return reject_at(DistributedSieveProtocolError::invalid_boolean, field_offset);
        }
        value = raw != 0U;
        return true;
    }

    [[nodiscard]] bool string(std::string& value, std::uint32_t max_size) {
        const std::uint64_t length_offset = offset();
        std::uint32_t size = 0;
        if (!u32(size)) {
            return false;
        }
        if (size > max_size) {
            return reject_at(DistributedSieveProtocolError::collection_too_large, length_offset);
        }
        if (!take(size)) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
        value.assign(begin, begin + size);
        offset_ += size;
        return true;
    }

    template <typename Vector>
    [[nodiscard]] bool prepare_sequence(Vector& values, std::uint32_t count,
                                        std::uint32_t max_count, std::uint64_t minimum_entry_bytes,
                                        std::uint64_t count_offset) {
        if (count > max_count) {
            return reject_at(DistributedSieveProtocolError::collection_too_large, count_offset);
        }
        if (minimum_entry_bytes != 0 &&
            static_cast<std::uint64_t>(count) >
                std::numeric_limits<std::uint64_t>::max() / minimum_entry_bytes) {
            return reject_at(DistributedSieveProtocolError::integer_out_of_range, count_offset);
        }
        const std::uint64_t minimum_bytes = static_cast<std::uint64_t>(count) * minimum_entry_bytes;
        if (minimum_bytes > static_cast<std::uint64_t>(remaining())) {
            return reject_at(DistributedSieveProtocolError::truncated, bytes_.size());
        }
        if (static_cast<std::uint64_t>(count) > static_cast<std::uint64_t>(values.max_size())) {
            return reject_at(DistributedSieveProtocolError::resource_exhausted, count_offset);
        }
        values.resize(count);
        return true;
    }

    [[nodiscard]] bool finish() noexcept {
        if (!status_) {
            return false;
        }
        if (offset_ != bytes_.size()) {
            return reject_at(DistributedSieveProtocolError::trailing_bytes, offset());
        }
        return true;
    }

    [[nodiscard]] bool reject(DistributedSieveProtocolError error) noexcept {
        return reject_at(error, offset());
    }

    [[nodiscard]] bool
    reject_at(DistributedSieveProtocolError error, std::uint64_t byte_offset,
              std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
        if (status_) {
            status_ = failure(error, byte_offset, element_index);
        }
        return false;
    }

    [[nodiscard]] std::uint64_t offset() const noexcept {
        return static_cast<std::uint64_t>(offset_);
    }

    [[nodiscard]] DistributedSieveProtocolStatus status() const noexcept {
        return status_;
    }

private:
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    [[nodiscard]] bool take(std::size_t amount) noexcept {
        if (!status_) {
            return false;
        }
        if (amount > remaining()) {
            return reject_at(DistributedSieveProtocolError::truncated, bytes_.size());
        }
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    DistributedSieveProtocolStatus status_;
};

} // namespace

DistributedSieveWorkIdentityEncodeResultV1
encode_distributed_sieve_work_identity_v1(const DistributedSieveWorkIdentityV1& identity) noexcept {
    if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
        return {std::nullopt, status};
    }

    try {
        CountingSink counter;
        if (!emit_distributed_sieve_work_identity_v1(counter, identity)) {
            return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value)};
        }
        if (const auto status = counter.status(); !status) {
            return {std::nullopt, status};
        }
        if (counter.size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::output_too_large, counter.size())};
        }

        VectorSink writer(static_cast<std::size_t>(counter.size()));
        if (!emit_distributed_sieve_work_identity_v1(writer, identity)) {
            return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value)};
        }
        auto bytes = writer.take_bytes();
        if (bytes.size() != counter.size()) {
            return {std::nullopt,
                    failure(DistributedSieveProtocolError::invalid_value, bytes.size())};
        }
        return {std::move(bytes), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveWorkIdentityDecodeResultV1
decode_distributed_sieve_work_identity_v1(std::span<const std::byte> bytes) noexcept {
    if (static_cast<std::uint64_t>(bytes.size()) >
        DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES) {
        return {std::nullopt,
                failure(DistributedSieveProtocolError::input_too_large, bytes.size())};
    }

    try {
        DistributedSieveWorkIdentityV1 identity;
        CanonicalDecodeArchive archive(bytes);
        if (!archive_detail::archive_work_identity_fields(archive, identity) || !archive.finish()) {
            return {std::nullopt, archive.status()};
        }
        if (const auto status = validate_distributed_sieve_work_identity(identity); !status) {
            return {std::nullopt, status};
        }
        return {std::move(identity), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

} // namespace gnfs::sieve::distributed_sieve_work_identity_codec_detail
