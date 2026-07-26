#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_authorized_cleanup_intent.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_durable_handoff.hpp>
#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>

#include "support/child_process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using gnfs::core::Relation;
using gnfs::relation::OOCAuthorizedCleanupIntentV2;
using gnfs::relation::OOCAuthorizedCleanupMarkerKindV2;
using gnfs::relation::OOCCleanupFaultPoint;
using gnfs::relation::OOCCleanupOwnershipReceipt;
using gnfs::relation::OOCCleanupPaths;
using gnfs::relation::OOCCleanupPublishFaultPoint;
using gnfs::relation::OOCCleanupRequest;
using gnfs::relation::OOCCleanupStage;
using gnfs::relation::OOCCleanupStatus;
using gnfs::relation::OOCCleanupTestHooks;
using gnfs::relation::OOCCleanupTestOperation;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCExactCleanupExpectation;
using gnfs::relation::OOCPrivateHandoffAdoptionFaultPoint;
using gnfs::relation::OOCPrivateHandoffAdoptionReceipt;
using gnfs::relation::OOCPrivateHandoffAdoptionTestHooks;
using gnfs::relation::OOCPrivateHandoffFaultPoint;
using gnfs::relation::OOCPrivateHandoffPairDescriptorV1;
using gnfs::relation::OOCPrivateHandoffPublishResult;
using gnfs::relation::OOCPrivateHandoffReader;
using gnfs::relation::OOCPrivateHandoffRecordV1;
using gnfs::relation::OOCPrivateHandoffState;
using gnfs::relation::OOCPrivateHandoffTestHooks;
using gnfs::relation::OOCPrivateLeaseFaultPoint;
using gnfs::relation::OOCPrivateLeaseOwnershipReceipt;
using gnfs::relation::OOCPrivateLeaseTestHooks;
using gnfs::relation::OOCRelationReader;
using gnfs::relation::OOCRelationStoreFormat;
using gnfs::relation::OOCRelationWriter;
using gnfs::relation::OOCSnapshotDescriptor;

static_assert(!std::is_default_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_copy_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_copy_assignable_v<OOCCleanupOwnershipReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_move_assignable_v<OOCCleanupOwnershipReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_copy_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_move_assignable_v<OOCPrivateLeaseOwnershipReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_copy_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_copy_assignable_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_move_assignable_v<OOCPrivateHandoffAdoptionReceipt>);
static_assert(!std::is_default_constructible_v<OOCPrivateHandoffReader>);
static_assert(!std::is_copy_constructible_v<OOCPrivateHandoffReader>);
static_assert(std::is_nothrow_move_constructible_v<OOCPrivateHandoffReader>);
static_assert(!std::is_move_assignable_v<OOCPrivateHandoffReader>);

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("gnfs-ooc-cleanup-transaction-" + std::to_string(tick) + "-" +
                     std::to_string(sequence.fetch_add(1)) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temp directory", path_, error);
            }
        }
        throw std::runtime_error("could not reserve a temporary directory");
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        (void)std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::file_status
symlink_status_no_follow(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    if (error) {
        throw std::filesystem::filesystem_error("inspect test path without following links", path,
                                                error);
    }
    return status;
}

[[nodiscard]] bool entry_exists_no_follow(const std::filesystem::path& path) {
    return std::filesystem::exists(symlink_status_no_follow(path));
}

[[nodiscard]] bool entry_is_symlink_no_follow(const std::filesystem::path& path) {
    return std::filesystem::is_symlink(symlink_status_no_follow(path));
}

void write_test_leaf(const std::filesystem::path& path, std::string_view payload) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test leaf");
    }
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write test leaf");
    }
}

[[maybe_unused, nodiscard]] std::vector<std::byte>
read_test_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("could not open test bytes");
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("could not size test bytes");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error("could not read test bytes");
    }
    return bytes;
}

[[maybe_unused]] void write_test_bytes(const std::filesystem::path& path,
                                       std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test byte leaf");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write test bytes");
    }
}

void check_test_bytes_preserved(const std::filesystem::path& path,
                                const std::vector<std::byte>& expected) {
    const bool present = entry_exists_no_follow(path);
    CHECK(present);
    if (present) {
        CHECK(read_test_bytes(path) == expected);
    }
}

[[nodiscard]] bool create_symlink_or_explicit_skip(const std::filesystem::path& target,
                                                   const std::filesystem::path& link,
                                                   [[maybe_unused]] std::string_view label) {
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (!error) {
        return true;
    }
#ifdef _WIN32
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error.value() == ERROR_PRIVILEGE_NOT_HELD) {
        std::cout << "[SKIP] " << label << ": symlink privilege unavailable: " << error.message()
                  << '\n';
        return false;
    }
#endif
    CHECK(!error);
    return false;
}

[[nodiscard]] bool create_hard_link_checked(const std::filesystem::path& target,
                                            const std::filesystem::path& link) {
    std::error_code error;
    std::filesystem::create_hard_link(target, link, error);
    CHECK(!error);
    return !error;
}

void check_entries_equivalent(const std::filesystem::path& first,
                              const std::filesystem::path& second) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(first, second, error);
    CHECK(!error);
    CHECK(equivalent);
}

void write_u64(std::ofstream& output, std::uint64_t value) {
    output.write(reinterpret_cast<const char*>(&value),
                 static_cast<std::streamsize>(sizeof(value)));
    if (!output) {
        throw std::runtime_error("could not write test u64");
    }
}

void pad_to(std::ofstream& output, std::uint64_t size) {
    const auto position = output.tellp();
    if (position == std::streampos(-1) || static_cast<std::uint64_t>(position) > size) {
        throw std::runtime_error("invalid test file extent");
    }
    for (std::uint64_t cursor = static_cast<std::uint64_t>(position); cursor < size; ++cursor) {
        output.put(static_cast<char>(cursor & 0xffU));
    }
    if (!output) {
        throw std::runtime_error("could not pad test file");
    }
}

void write_index(const std::filesystem::path& path, std::uint64_t magic, std::uint64_t store_id,
                 std::uint64_t count, std::uint64_t index_size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test index");
    }
    write_u64(output, magic);
    write_u64(output, OOCRelationStoreFormat::FORMAT_VERSION_V3);
    write_u64(output, store_id);
    write_u64(output, count);
    pad_to(output, index_size);
}

void write_data(const std::filesystem::path& path, std::uint64_t store_id,
                std::uint64_t data_size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not create test data");
    }
    write_u64(output, OOCRelationStoreFormat::MAGIC_V3_DATA);
    write_u64(output, OOCRelationStoreFormat::FORMAT_VERSION_V3);
    write_u64(output, store_id);
    pad_to(output, data_size);
}

struct PairShape final {
    std::uint64_t magic = OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE;
    std::uint64_t count = 0;
    std::uint64_t index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES;
    std::uint64_t data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 16;
};

struct RegisteredOwnedPair final {
    std::uint64_t logical_store_id = 0;
    std::uint64_t actual_store_id = 0;
    std::optional<OOCCleanupOwnershipReceipt> ownership;
};

[[nodiscard]] std::string owned_pair_key(const std::filesystem::path& base) {
    return OOCCleanupTransaction::paths_for(base).base_path.string();
}

[[nodiscard]] std::unordered_map<std::string, RegisteredOwnedPair>& owned_pair_registry() {
    static std::unordered_map<std::string, RegisteredOwnedPair> registry;
    return registry;
}

void register_cleanup_ownership(const std::filesystem::path& base, std::uint64_t logical_store_id,
                                std::uint64_t actual_store_id,
                                OOCCleanupOwnershipReceipt ownership) {
    auto& registry = owned_pair_registry();
    const auto key = owned_pair_key(base);
    registry.erase(key);
    registry.emplace(key, RegisteredOwnedPair{
                              .logical_store_id = logical_store_id,
                              .actual_store_id = actual_store_id,
                              .ownership = std::move(ownership),
                          });
}

[[nodiscard]] Relation make_real_relation(std::int64_t a, std::uint64_t b) {
    Relation relation(a, b);
    relation.rational_factors.push_back(static_cast<std::uint32_t>(100 + a));
    relation.algebraic_factors.push_back(static_cast<std::uint32_t>(200 + a));
    return relation;
}

void write_pair(const std::filesystem::path& base, std::uint64_t store_id,
                const PairShape& shape = {}) {
    OOCRelationWriter writer(base.string());
    const std::uint64_t actual_store_id = writer.store_id();
    writer.abort();
    auto ownership = writer.take_cleanup_ownership_receipt();
    write_index(base.string() + ".relidx", shape.magic, actual_store_id, shape.count,
                shape.index_size);
    write_data(base.string() + ".reldata", actual_store_id, shape.data_size);
    register_cleanup_ownership(base, store_id, actual_store_id, std::move(ownership));
}

void write_pair(const std::filesystem::path& base, std::uint64_t store_id,
                OOCPrivateLeaseOwnershipReceipt& private_lease, const PairShape& shape = {}) {
    OOCRelationWriter writer(base.string(), private_lease);
    const std::uint64_t actual_store_id = writer.store_id();
    writer.abort();
    auto ownership = writer.take_cleanup_ownership_receipt();
    write_index(base.string() + ".relidx", shape.magic, actual_store_id, shape.count,
                shape.index_size);
    write_data(base.string() + ".reldata", actual_store_id, shape.data_size);
    register_cleanup_ownership(base, store_id, actual_store_id, std::move(ownership));
}

[[nodiscard]] OOCCleanupOwnershipReceipt
capture_cleanup_ownership(const std::filesystem::path& base_path, std::uint64_t store_id) {
    auto& registry = owned_pair_registry();
    const auto found = registry.find(owned_pair_key(base_path));
    if (found == registry.end() || found->second.logical_store_id != store_id ||
        !found->second.ownership) {
        throw std::logic_error("test pair has no matching production cleanup ownership");
    }
    OOCCleanupOwnershipReceipt ownership(std::move(*found->second.ownership));
    found->second.ownership.reset();
    return ownership;
}

[[nodiscard]] std::uint64_t actual_store_id_for(const std::filesystem::path& base_path,
                                                std::uint64_t logical_store_id) {
    const auto& registry = owned_pair_registry();
    const auto found = registry.find(owned_pair_key(base_path));
    if (found == registry.end() || found->second.logical_store_id != logical_store_id) {
        throw std::logic_error("test pair has no matching production store identity");
    }
    return found->second.actual_store_id;
}

[[nodiscard]] gnfs::relation::OOCCleanupResult begin_cleanup(const OOCCleanupRequest& request,
                                                             OOCCleanupTestHooks hooks = {}) {
    auto ownership = capture_cleanup_ownership(request.base_path, request.store_id);
    return OOCCleanupTransaction::begin_or_resume(ownership, request.exact, hooks);
}

[[nodiscard]] gnfs::relation::OOCCleanupResult begin_cleanup(const std::filesystem::path& base_path,
                                                             std::uint64_t store_id,
                                                             OOCCleanupTestHooks hooks = {}) {
    auto ownership = capture_cleanup_ownership(base_path, store_id);
    return OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt, hooks);
}

[[nodiscard]] OOCExactCleanupExpectation exact_for(const PairShape& shape) {
    return OOCExactCleanupExpectation{
        .index_magic = shape.magic,
        .persisted_count = shape.count,
        .index_size = shape.index_size,
        .data_size = shape.data_size,
    };
}

[[maybe_unused, nodiscard]] OOCPrivateHandoffPairDescriptorV1
handoff_pair_descriptor(const OOCSnapshotDescriptor& descriptor) {
    return OOCPrivateHandoffPairDescriptorV1{
        .format_version = descriptor.format_version,
        .store_id = descriptor.store_id,
        .generation = descriptor.generation,
        .count = descriptor.count,
        .index_extent = OOCRelationWriter::index_size_for_count(descriptor.count),
        .data_extent = descriptor.data_end,
    };
}

[[nodiscard]] gnfs::util::Sha256Digest authorized_cleanup_test_digest(std::uint8_t seed) {
    gnfs::util::Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        digest.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + index * 29U));
    }
    return digest;
}

[[nodiscard]] gnfs::util::durable_immutable_record::NativeIdentity
authorized_cleanup_test_identity(std::uint64_t seed) {
    return {
        .first = seed,
        .second = seed + 1,
        .third = seed + 2,
    };
}

[[nodiscard]] std::vector<std::byte>
authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2 kind) {
    OOCAuthorizedCleanupIntentV2 marker;
    marker.marker_kind = kind;
    marker.base_path_digest = authorized_cleanup_test_digest(0x01);
    marker.external_authorization_digest = authorized_cleanup_test_digest(0x21);
    marker.generic_handoff_self_digest = authorized_cleanup_test_digest(0x41);
    marker.lease_id = {UINT64_C(0x0102030405060708), UINT64_C(0x1112131415161718)};
    marker.parent_directory_identity =
        authorized_cleanup_test_identity(UINT64_C(0x2122232425262728));
    marker.lock_identity = authorized_cleanup_test_identity(UINT64_C(0x3132333435363738));
    marker.directory_identity = authorized_cleanup_test_identity(UINT64_C(0x4142434445464748));
    marker.owner_marker_identity = authorized_cleanup_test_identity(UINT64_C(0x5152535455565758));
    marker.owned_marker_identity = authorized_cleanup_test_identity(UINT64_C(0x6162636465666768));
    marker.pair = OOCPrivateHandoffPairDescriptorV1{
        .format_version = OOCRelationStoreFormat::FORMAT_VERSION_V3,
        .store_id = UINT64_C(0x7172737475767778),
        .generation = UINT64_C(0x8182838485868788),
        .count = 0,
        .index_extent = OOCRelationStoreFormat::INDEX_HEADER_BYTES +
                        OOCRelationStoreFormat::INDEX_SENTINEL_BYTES,
        .data_extent = OOCRelationStoreFormat::DATA_HEADER_BYTES,
    };
    marker.handoff = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0x9192939495969798)),
        .extent = gnfs::relation::OOC_PRIVATE_HANDOFF_WIRE_FIXED_BYTES_V1,
    };
    marker.index = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0xa1a2a3a4a5a6a7a8)),
        .extent = marker.pair.index_extent,
    };
    marker.data = {
        .identity = authorized_cleanup_test_identity(UINT64_C(0xb1b2b3b4b5b6b7b8)),
        .extent = marker.pair.data_extent,
    };
    const auto sealed = gnfs::relation::seal_ooc_authorized_cleanup_intent(marker);
    if (!sealed) {
        throw std::runtime_error("could not seal authorized cleanup V2 test marker");
    }
    const auto encoded = gnfs::relation::encode_ooc_authorized_cleanup_intent(marker);
    if (!encoded || !encoded.bytes.has_value()) {
        throw std::runtime_error("could not encode authorized cleanup V2 test marker");
    }
    return *encoded.bytes;
}

constexpr std::uint32_t PRIVATE_HANDOFF_PAYLOAD_KIND = 0x474E4653U;
constexpr std::uint32_t PRIVATE_HANDOFF_PAYLOAD_VERSION = 1;
constexpr std::array<std::byte, 5> PRIVATE_HANDOFF_PAYLOAD{
    std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x50},
};

struct PreparedPrivateHandoff final {
    OOCSnapshotDescriptor descriptor;
    OOCCleanupOwnershipReceipt pair_ownership;
    OOCPrivateLeaseOwnershipReceipt lease_ownership;
};

[[nodiscard]] PreparedPrivateHandoff prepare_private_handoff(const std::filesystem::path& base,
                                                             bool write_relation = true) {
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed()) {
        throw std::runtime_error("could not reserve private handoff fixture");
    }

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    if (write_relation) {
        (void)writer.write(make_real_relation(17, 19));
    }
    const auto descriptor = writer.finalize();
    auto pair_ownership = writer.take_cleanup_ownership_receipt();
    return PreparedPrivateHandoff{
        .descriptor = descriptor,
        .pair_ownership = std::move(pair_ownership),
        .lease_ownership = std::move(*reservation.ownership),
    };
}

[[nodiscard]] OOCPrivateHandoffPublishResult
publish_private_handoff(PreparedPrivateHandoff& prepared, OOCPrivateHandoffTestHooks hooks = {}) {
    return OOCCleanupTransaction::publish_private_handoff(
        prepared.pair_ownership, prepared.lease_ownership,
        handoff_pair_descriptor(prepared.descriptor), PRIVATE_HANDOFF_PAYLOAD_KIND,
        PRIVATE_HANDOFF_PAYLOAD_VERSION, PRIVATE_HANDOFF_PAYLOAD, hooks);
}

[[nodiscard]] int run_private_handoff_adoption_child(std::string_view operation,
                                                     const std::filesystem::path& base) {
#if defined(__APPLE__)
    try {
        if (operation == "publish-exit" || operation == "publish-empty-exit") {
            auto prepared = prepare_private_handoff(base, operation == "publish-exit");
            if (!publish_private_handoff(prepared).canonical()) {
                return 91;
            }
            std::_Exit(0);
        }
        if (operation == "publish-pending-exit" || operation == "publish-canonical-exit") {
            auto prepared = prepare_private_handoff(base);
            auto target = operation == "publish-pending-exit"
                              ? OOCPrivateHandoffFaultPoint::PendingDurable
                              : OOCPrivateHandoffFaultPoint::CanonicalDurable;
            const auto exit_at_target = [](OOCPrivateHandoffFaultPoint point,
                                           void* opaque) noexcept {
                if (point == *static_cast<const OOCPrivateHandoffFaultPoint*>(opaque)) {
                    std::_Exit(0);
                }
                return false;
            };
            (void)publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                                        .stop_after = exit_at_target,
                                                        .context = &target,
                                                    });
            return 96;
        }
        if (operation == "adopt-exit") {
            auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
            if (!adopted.adopted()) {
                return 92;
            }
            OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
            if (!reader.valid() || reader.reader().count() != 1) {
                return 93;
            }
            const auto relation = reader.reader().read(0);
            if (relation.a != 17 || relation.b != 19) {
                return 94;
            }
            std::_Exit(0);
        }
    } catch (...) {
        return 95;
    }
#else
    (void)operation;
    (void)base;
#endif
    return 64;
}

void set_private_control_leaf_mode(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
        throw std::filesystem::filesystem_error("set private control leaf mode", path, error);
    }
}

void write_private_control_bytes(const std::filesystem::path& path,
                                 std::span<const std::byte> bytes) {
    write_test_bytes(path, bytes);
    set_private_control_leaf_mode(path);
}

void store_u32_le(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(value)) {
        throw std::runtime_error("test u32 mutation is out of range");
    }
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> static_cast<unsigned int>(index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::vector<std::byte>
encode_private_handoff_record(const OOCPrivateHandoffRecordV1& record) {
    const auto encoded = gnfs::relation::encode_ooc_private_handoff_record(record);
    if (!encoded || !encoded.bytes) {
        throw std::runtime_error("could not encode private handoff test record");
    }
    return *encoded.bytes;
}

void flip_last_byte(const std::filesystem::path& path) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw std::runtime_error("could not open marker for corruption");
    }
    stream.seekg(-1, std::ios::end);
    char byte = 0;
    stream.read(&byte, 1);
    if (!stream) {
        throw std::runtime_error("could not read marker byte");
    }
    byte ^= static_cast<char>(0x80);
    stream.seekp(-1, std::ios::end);
    stream.write(&byte, 1);
    stream.flush();
    if (!stream) {
        throw std::runtime_error("could not corrupt marker byte");
    }
}

void overwrite_count(const std::filesystem::path& index_path, std::uint64_t count) {
    std::fstream stream(index_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw std::runtime_error("could not open index for count mutation");
    }
    stream.seekp(static_cast<std::streamoff>(OOCRelationStoreFormat::INDEX_COUNT_OFFSET));
    stream.write(reinterpret_cast<const char*>(&count),
                 static_cast<std::streamsize>(sizeof(count)));
    stream.flush();
    if (!stream) {
        throw std::runtime_error("could not mutate index count");
    }
}

struct StopContext final {
    OOCCleanupFaultPoint target = OOCCleanupFaultPoint::IntentDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at(OOCCleanupFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<StopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks stop_hooks(StopContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = stop_at,
        .stop_after_publish = nullptr,
        .fail_before_operation = nullptr,
        .context = &context,
    };
}

struct PublishStopContext final {
    OOCCleanupPublishFaultPoint target = OOCCleanupPublishFaultPoint::IntentPendingDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at_publish(OOCCleanupPublishFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PublishStopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks publish_stop_hooks(PublishStopContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = nullptr,
        .stop_after_publish = stop_at_publish,
        .fail_before_operation = nullptr,
        .context = &context,
    };
}

struct PrivateHandoffStopContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::PendingDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_at_private_handoff(OOCPrivateHandoffFaultPoint point,
                                           void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffStopContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCPrivateHandoffTestHooks
private_handoff_stop_hooks(PrivateHandoffStopContext& context) noexcept {
    return OOCPrivateHandoffTestHooks{
        .stop_after = stop_at_private_handoff,
        .context = &context,
    };
}

constexpr std::array PRIVATE_HANDOFF_FAULT_POINTS{
    OOCPrivateHandoffFaultPoint::PendingDurable,
    OOCPrivateHandoffFaultPoint::CanonicalPromoted,
    OOCPrivateHandoffFaultPoint::CanonicalDurable,
    OOCPrivateHandoffFaultPoint::ReservedRevokedDurable,
};

struct OperationFailureContext final {
    OOCCleanupTestOperation target = OOCCleanupTestOperation::IndexRename;
    bool failed = false;
};

[[nodiscard]] bool fail_operation_once(OOCCleanupTestOperation operation, void* opaque) noexcept {
    auto& context = *static_cast<OperationFailureContext*>(opaque);
    if (!context.failed && operation == context.target) {
        context.failed = true;
        return true;
    }
    return false;
}

[[nodiscard]] OOCCleanupTestHooks
operation_failure_hooks(OperationFailureContext& context) noexcept {
    return OOCCleanupTestHooks{
        .stop_after = nullptr,
        .stop_after_publish = nullptr,
        .fail_before_operation = fail_operation_once,
        .context = &context,
    };
}

constexpr std::array CLEANUP_FAULT_POINTS{
    OOCCleanupFaultPoint::IntentDurable,        OOCCleanupFaultPoint::FirstRenameDurable,
    OOCCleanupFaultPoint::SecondRenameDurable,  OOCCleanupFaultPoint::DeleteAuthorizedDurable,
    OOCCleanupFaultPoint::FirstUnlinkDurable,   OOCCleanupFaultPoint::SecondUnlinkDurable,
    OOCCleanupFaultPoint::IntentRemovedDurable,
};

[[nodiscard]] OOCCleanupStage expected_stage(OOCCleanupFaultPoint point) {
    switch (point) {
    case OOCCleanupFaultPoint::IntentDurable:
        return OOCCleanupStage::IntentDurable;
    case OOCCleanupFaultPoint::FirstRenameDurable:
        return OOCCleanupStage::IndexQuarantined;
    case OOCCleanupFaultPoint::SecondRenameDurable:
        return OOCCleanupStage::PairQuarantined;
    case OOCCleanupFaultPoint::DeleteAuthorizedDurable:
        return OOCCleanupStage::DeleteAuthorized;
    case OOCCleanupFaultPoint::FirstUnlinkDurable:
        return OOCCleanupStage::DataRemoved;
    case OOCCleanupFaultPoint::SecondUnlinkDurable:
        return OOCCleanupStage::IndexRemoved;
    case OOCCleanupFaultPoint::IntentRemovedDurable:
        return OOCCleanupStage::IntentRemoved;
    }
    throw std::runtime_error("unknown fault point");
}

void check_fault_namespace(const gnfs::relation::OOCCleanupPaths& paths,
                           OOCCleanupFaultPoint point) {
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(exists(paths.intent_path) != (point == OOCCleanupFaultPoint::IntentRemovedDurable));
    CHECK(exists(paths.staged_path) == (point == OOCCleanupFaultPoint::DeleteAuthorizedDurable ||
                                        point == OOCCleanupFaultPoint::FirstUnlinkDurable ||
                                        point == OOCCleanupFaultPoint::SecondUnlinkDurable ||
                                        point == OOCCleanupFaultPoint::IntentRemovedDurable));

    switch (point) {
    case OOCCleanupFaultPoint::IntentDurable:
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(!exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::FirstRenameDurable:
        CHECK(!exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::SecondRenameDurable:
    case OOCCleanupFaultPoint::DeleteAuthorizedDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::FirstUnlinkDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    case OOCCleanupFaultPoint::SecondUnlinkDurable:
    case OOCCleanupFaultPoint::IntentRemovedDurable:
        CHECK(!exists(paths.index_path));
        CHECK(!exists(paths.data_path));
        CHECK(!exists(paths.quarantine_index_path));
        CHECK(!exists(paths.quarantine_data_path));
        break;
    }
}

void check_cleanup_complete(const gnfs::relation::OOCCleanupPaths& paths) {
    CHECK(!exists(paths.index_path));
    CHECK(!exists(paths.data_path));
    CHECK(!exists(paths.intent_path));
    CHECK(!exists(paths.intent_pending_path));
    CHECK(!exists(paths.staged_path));
    CHECK(!exists(paths.staged_pending_path));
    CHECK(!exists(paths.quarantine_index_path));
    CHECK(!exists(paths.quarantine_data_path));
}

void test_fault_point_recovery() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1234'5678'9abc'def0ULL;

    for (std::size_t index = 0; index < CLEANUP_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() / ("fault-" + std::to_string(index));
        write_pair(base, store_id + index);
        StopContext stop{.target = CLEANUP_FAULT_POINTS[index]};
        const auto interrupted = begin_cleanup(base, store_id + index, stop_hooks(stop));
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == expected_stage(CLEANUP_FAULT_POINTS[index]));
        CHECK(stop.stopped);

        const auto paths = OOCCleanupTransaction::paths_for(base);
        check_fault_namespace(paths, CLEANUP_FAULT_POINTS[index]);
        const auto resumed = OOCCleanupTransaction::resume(base);
        CHECK(resumed.completed());
        check_cleanup_complete(paths);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);
    }
}

void test_receipt_authority_and_pending_publication() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x9191'a2a2'b3b3'c4c4ULL;

    {
        const auto base = temp.path() / "intent-pending-retry";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        };
        const auto interrupted = OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt,
                                                                        publish_stop_hooks(stop));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::None);
        CHECK(stop.stopped);
        CHECK(!ownership.spent());
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));

        // A crash or short write can corrupt only the no-authority pending
        // leaf. The same unspent ownership capability repairs it in place.
        flip_last_byte(paths.intent_pending_path);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).completed());
        CHECK(ownership.spent());
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
              OOCCleanupStatus::InvalidRequest);
        check_cleanup_complete(paths);
    }

    {
        const auto base = temp.path() / "staged-pending-retry";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::StagedPendingDurable,
        };
        const auto interrupted = OOCCleanupTransaction::begin_or_resume(ownership, std::nullopt,
                                                                        publish_stop_hooks(stop));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(interrupted.status == OOCCleanupStatus::Interrupted);
        CHECK(interrupted.stage == OOCCleanupStage::PairQuarantined);
        CHECK(stop.stopped);
        CHECK(ownership.spent());
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.staged_pending_path));
        CHECK(!entry_exists_no_follow(paths.staged_path));
        CHECK(entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(entry_exists_no_follow(paths.quarantine_data_path));
        CHECK(OOCCleanupTransaction::resume(base).completed());
        check_cleanup_complete(paths);
    }

    {
        const auto base = temp.path() / "receipt-identity-replacement";
        write_pair(base, store_id + 2);
        auto ownership = capture_cleanup_ownership(base, store_id + 2);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_data = temp.path() / "receipt-owned-data";
        std::filesystem::rename(paths.data_path, saved_data);
        write_data(paths.data_path, actual_store_id_for(base, store_id + 2),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
              OOCCleanupStatus::SourcePairInvalid);
        CHECK(!ownership.spent());
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(saved_data));
        CHECK(entry_exists_no_follow(paths.index_path));
    }

    {
        const auto base = temp.path() / "receipt-empty-terminal";
        write_pair(base, store_id + 3);
        auto ownership = capture_cleanup_ownership(base, store_id + 3);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        std::filesystem::remove(paths.index_path);
        std::filesystem::remove(paths.data_path);
        CHECK(OOCCleanupTransaction::begin_or_resume(ownership).completed());
        CHECK(ownership.spent());
    }

    {
        const auto base = temp.path() / "receipt-one-shot-move";
        OOCRelationWriter writer(base.string());
        const auto descriptor = writer.finalize();
        auto source = writer.take_cleanup_ownership_receipt();
        bool second_transfer_rejected = false;
        try {
            (void)writer.take_cleanup_ownership_receipt();
        } catch (const std::logic_error&) {
            second_transfer_rejected = true;
        }
        CHECK(second_transfer_rejected);

        OOCCleanupOwnershipReceipt destination(std::move(source));
        CHECK(source.spent());
        CHECK(!destination.spent());
        CHECK(OOCCleanupTransaction::begin_or_resume(source).status ==
              OOCCleanupStatus::InvalidRequest);
        const auto moved_cleanup = OOCCleanupTransaction::begin_or_resume(
            destination,
            OOCExactCleanupExpectation{
                .index_magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
                .persisted_count = descriptor.count,
                .index_size = OOCRelationWriter::index_size_for_count(descriptor.count),
                .data_size = descriptor.data_end,
            });
        CHECK(moved_cleanup.completed());
        CHECK(destination.spent());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_namespace_operation_failures_are_retryable() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xa1a1'b2b2'c3c3'd4d0ULL;
    struct FailureCase final {
        OOCCleanupTestOperation operation;
        OOCCleanupStatus expected_status;
        bool retry_reports_no_transaction = false;
    };
    constexpr std::array cases{
        FailureCase{OOCCleanupTestOperation::IndexRename, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IndexRenameParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::DataRename, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::DataRenameParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::DataUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::DataUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::IndexUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IndexUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::IntentUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::IntentUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure},
        FailureCase{OOCCleanupTestOperation::StagedUnlink, OOCCleanupStatus::IoFailure},
        FailureCase{OOCCleanupTestOperation::StagedUnlinkParentSync,
                    OOCCleanupStatus::DurabilityFailure, true},
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto base = temp.path() / ("operation-failure-" + std::to_string(index));
        write_pair(base, store_id + index);
        auto ownership = capture_cleanup_ownership(base, store_id + index);
        OperationFailureContext failure{.target = cases[index].operation};
        const auto result = OOCCleanupTransaction::begin_or_resume(
            ownership, std::nullopt, operation_failure_hooks(failure));
        CHECK(failure.failed);
        CHECK(result.status == cases[index].expected_status);
        CHECK(result.retryable());
        CHECK(ownership.spent());

        const auto resumed = OOCCleanupTransaction::resume(base);
        if (cases[index].retry_reports_no_transaction) {
            CHECK(resumed.status == OOCCleanupStatus::NoTransaction);
        } else {
            CHECK(resumed.completed());
        }
        CHECK(resumed.transaction_terminal());
        CHECK(OOCCleanupTransaction::confirm_pair_namespace_reusable(base).completed());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_reserved_cleanup_suffix_is_rejected() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xb1b1'c2c2'd3d3'e4e4ULL;
    const auto base = temp.path() / "foreign.gnfs-ooc-cleanup-v1";
    write_index(base.string() + ".relidx", OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, store_id, 0,
                OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    write_data(base.string() + ".reldata", store_id,
               OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
    const auto result = OOCCleanupTransaction::resume(base);
    CHECK(result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(entry_exists_no_follow(base.string() + ".relidx"));
    CHECK(entry_exists_no_follow(base.string() + ".reldata"));
}

void test_fresh_writer_rejects_nonempty_cleanup_namespace() {
    TempDirectory temp;
    constexpr std::array<std::string_view, 8> labels{
        "index",  "data",           "intent",           "intent-pending",
        "staged", "staged-pending", "quarantine-index", "quarantine-data",
    };

    for (std::size_t index = 0; index < labels.size(); ++index) {
        const auto base = temp.path() / ("fresh-reuse-" + std::to_string(index));
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const std::array<const std::filesystem::path*, 8> leaves{
            &paths.index_path,
            &paths.data_path,
            &paths.intent_path,
            &paths.intent_pending_path,
            &paths.staged_path,
            &paths.staged_pending_path,
            &paths.quarantine_index_path,
            &paths.quarantine_data_path,
        };
        write_test_leaf(*leaves[index], labels[index]);

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string());
            (void)writer;
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(entry_exists_no_follow(*leaves[index]));
        if (index != 0) {
            CHECK(!entry_exists_no_follow(paths.index_path));
        }
        if (index != 1) {
            CHECK(!entry_exists_no_follow(paths.data_path));
        }
    }
}

void test_windows_sharing_violation_is_retryable() {
#ifdef _WIN32
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xc1c1'd2d2'e3e3'f4f4ULL;
    const auto base = temp.path() / "windows-sharing";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);

    const HANDLE held = ::CreateFileW(paths.quarantine_data_path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    if (held == INVALID_HANDLE_VALUE) {
        return;
    }
    const auto blocked = OOCCleanupTransaction::resume(base);
    CHECK(blocked.status == OOCCleanupStatus::IoFailure);
    CHECK(blocked.retryable());
    CHECK(entry_exists_no_follow(paths.quarantine_data_path));
    CHECK(::CloseHandle(held) != FALSE);
    CHECK(OOCCleanupTransaction::resume(base).completed());
    check_cleanup_complete(paths);
#endif
}

void test_exact_finalized_expectation() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xabc0'1234'5678'9001ULL;
    const PairShape shape{
        .magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
        .count = 2,
        .index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES + 3 * sizeof(std::uint64_t),
        .data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 48,
    };

    {
        const auto base = temp.path() / "finalized-convenience-rejected";
        write_pair(base, store_id, shape);
        const auto result = begin_cleanup(base, store_id);
        CHECK(result.status == OOCCleanupStatus::SourcePairInvalid);
        CHECK(std::filesystem::exists(base.string() + ".relidx"));
        CHECK(std::filesystem::exists(base.string() + ".reldata"));
    }

    {
        const auto base = temp.path() / "finalized-exact-mismatch";
        write_pair(base, store_id + 1, shape);
        auto wrong = exact_for(shape);
        ++wrong.data_size;
        const OOCCleanupRequest request{
            .base_path = base,
            .store_id = store_id + 1,
            .exact = wrong,
        };
        const auto result = begin_cleanup(request);
        CHECK(result.status == OOCCleanupStatus::SourcePairInvalid);
        CHECK(std::filesystem::exists(base.string() + ".relidx"));
        CHECK(std::filesystem::exists(base.string() + ".reldata"));
    }

    {
        const auto base = temp.path() / "finalized-exact";
        write_pair(base, store_id + 2, shape);
        const OOCCleanupRequest request{
            .base_path = base,
            .store_id = store_id + 2,
            .exact = exact_for(shape),
        };
        CHECK(begin_cleanup(request).completed());
        check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
    }
}

void test_real_finalized_store_cleanup() {
    TempDirectory temp;
    const auto base = temp.path() / "real-finalized";
    OOCSnapshotDescriptor descriptor;
    std::optional<OOCCleanupOwnershipReceipt> ownership;
    {
        OOCRelationWriter writer(base.string());
        CHECK(writer.write(make_real_relation(11, 12)) == 0);
        CHECK(writer.write(make_real_relation(13, 14)) == 1);
        descriptor = writer.finalize();
        ownership.emplace(writer.take_cleanup_ownership_receipt());
    }
    register_cleanup_ownership(base, descriptor.store_id, descriptor.store_id,
                               std::move(*ownership));

    CHECK(descriptor.format_version == OOCRelationWriter::FORMAT_VERSION_V3);
    CHECK(descriptor.store_id != 0);
    CHECK(descriptor.count == 2);
    {
        OOCRelationReader reader(base.string(), descriptor);
        CHECK(reader.count() == descriptor.count);
        CHECK(reader.read(0).a == 11);
        CHECK(reader.read(0).b == 12);
        CHECK(reader.read(1).a == 13);
        CHECK(reader.read(1).b == 14);
    }

    const OOCCleanupRequest request{
        .base_path = base,
        .store_id = descriptor.store_id,
        .exact =
            OOCExactCleanupExpectation{
                .index_magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
                .persisted_count = descriptor.count,
                .index_size = OOCRelationWriter::index_size_for_count(descriptor.count),
                .data_size = descriptor.data_end,
            },
    };
    CHECK(begin_cleanup(request).completed());
    check_cleanup_complete(OOCCleanupTransaction::paths_for(base));
}

void test_marker_corruption_is_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x0102'0304'0506'0708ULL;

    {
        const auto base = temp.path() / "intent-corrupt";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        flip_last_byte(paths.intent_path);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
    }

    {
        const auto base = temp.path() / "staged-corrupt";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        flip_last_byte(paths.staged_path);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.quarantine_data_path));
    }
}

void test_authorized_v2_markers_are_not_legacy_cleanup_authority() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = UINT64_C(0x0a0b0c0d0e0f1011);

    {
        const auto base = temp.path() / "v2-in-legacy-intent";
        write_pair(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto v2_intent =
            authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::intent);
        write_test_bytes(paths.intent_path, v2_intent);

        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(paths.intent_path, v2_intent);
        CHECK(!entry_exists_no_follow(paths.staged_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }

    {
        const auto base = temp.path() / "v2-in-legacy-staged";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto legacy_intent_bytes = read_test_bytes(paths.intent_path);
        const auto v2_staged =
            authorized_cleanup_v2_marker_bytes(OOCAuthorizedCleanupMarkerKindV2::staged);
        write_test_bytes(paths.staged_path, v2_staged);

        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(paths.intent_path, legacy_intent_bytes);
        check_test_bytes_preserved(paths.staged_path, v2_staged);
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    }
}

void test_absence_before_staged_has_no_delete_authority() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1111'2222'3333'4444ULL;
    const auto base = temp.path() / "unauthorized-absence";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_data = temp.path() / "saved-data";
    std::filesystem::rename(paths.data_path, saved_data);

    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(exists(paths.intent_path));
    CHECK(!exists(paths.staged_path));
    CHECK(exists(paths.quarantine_index_path));
    CHECK(exists(saved_data));
}

void test_reverse_pre_staged_state_is_rejected() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1212'2323'3434'4545ULL;
    const auto base = temp.path() / "reverse-pre-staged";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);

    const auto paths = OOCCleanupTransaction::paths_for(base);
    std::filesystem::rename(paths.data_path, paths.quarantine_data_path);
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.quarantine_data_path));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
}

void test_source_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1313'2424'3535'4646ULL;

    {
        const auto base = temp.path() / "source-symlink";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_index = temp.path() / "source-symlink-owned-index";
        std::filesystem::rename(paths.index_path, saved_index);
        if (create_symlink_or_explicit_skip(saved_index, paths.index_path, "source symlink")) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::SourcePairInvalid);
            CHECK(entry_is_symlink_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(saved_index));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "source-hardlink";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "source-hardlink-alias";
        if (create_hard_link_checked(paths.index_path, alias)) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::SourcePairInvalid);
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.index_path, alias);
        }
    }
}

void test_quarantine_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1414'2525'3636'4747ULL;

    {
        const auto base = temp.path() / "quarantine-symlink";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_index = temp.path() / "quarantine-symlink-owned-index";
        std::filesystem::rename(paths.quarantine_index_path, saved_index);
        if (create_symlink_or_explicit_skip(saved_index, paths.quarantine_index_path,
                                            "quarantine symlink")) {
            CHECK(OOCCleanupTransaction::resume(base).status ==
                  OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_is_symlink_no_follow(paths.quarantine_index_path));
            CHECK(entry_exists_no_follow(saved_index));
            CHECK(entry_exists_no_follow(paths.data_path));
            CHECK(entry_exists_no_follow(paths.intent_path));
        }
    }

    {
        const auto base = temp.path() / "quarantine-hardlink";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "quarantine-hardlink-alias";
        if (create_hard_link_checked(paths.quarantine_index_path, alias)) {
            CHECK(OOCCleanupTransaction::resume(base).status ==
                  OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_exists_no_follow(paths.quarantine_index_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.data_path));
            CHECK(entry_exists_no_follow(paths.intent_path));
            check_entries_equivalent(paths.quarantine_index_path, alias);
        }
    }
}

void test_intent_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1515'2626'3737'4848ULL;

    {
        const auto base = temp.path() / "intent-symlink";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved_intent = temp.path() / "intent-symlink-owned-marker";
        std::filesystem::rename(paths.intent_path, saved_intent);
        if (create_symlink_or_explicit_skip(saved_intent, paths.intent_path, "intent symlink")) {
            CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_is_symlink_no_follow(paths.intent_path));
            CHECK(entry_exists_no_follow(saved_intent));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "intent-hardlink";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto alias = temp.path() / "intent-hardlink-alias";
        if (create_hard_link_checked(paths.intent_path, alias)) {
            CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_exists_no_follow(paths.intent_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.intent_path, alias);
        }
    }
}

void test_lock_link_attacks_are_fail_closed() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x1616'2727'3838'4949ULL;

    {
        const auto base = temp.path() / "lock-symlink";
        write_pair(base, store_id);
        auto ownership = capture_cleanup_ownership(base, store_id);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_test_leaf(paths.lock_path, "owned cleanup lock");
        const auto saved_lock = temp.path() / "lock-symlink-owned-lock";
        std::filesystem::rename(paths.lock_path, saved_lock);
        const auto target = temp.path() / "lock-symlink-target";
        write_test_leaf(target, "foreign lock target");
        if (create_symlink_or_explicit_skip(target, paths.lock_path, "lock symlink")) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::NamespaceConflict);
            CHECK(entry_is_symlink_no_follow(paths.lock_path));
            CHECK(entry_exists_no_follow(saved_lock));
            CHECK(entry_exists_no_follow(target));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }

    {
        const auto base = temp.path() / "lock-hardlink";
        write_pair(base, store_id + 1);
        auto ownership = capture_cleanup_ownership(base, store_id + 1);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_test_leaf(paths.lock_path, "owned cleanup lock");
        const auto alias = temp.path() / "lock-hardlink-alias";
        if (create_hard_link_checked(paths.lock_path, alias)) {
            CHECK(OOCCleanupTransaction::begin_or_resume(ownership).status ==
                  OOCCleanupStatus::NamespaceConflict);
            CHECK(entry_exists_no_follow(paths.lock_path));
            CHECK(entry_exists_no_follow(alias));
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
            check_entries_equivalent(paths.lock_path, alias);
        }
    }
}

void test_foreign_replacements_are_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x2222'3333'4444'5555ULL;

    {
        const auto base = temp.path() / "foreign-original-index";
        write_pair(base, store_id);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        write_index(paths.index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, store_id + 100,
                    0, OOCRelationStoreFormat::INDEX_HEADER_BYTES);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.quarantine_index_path));
        CHECK(exists(paths.data_path));
    }

    {
        const auto base = temp.path() / "foreign-data-same-header";
        write_pair(base, store_id + 1);
        StopContext stop{.target = OOCCleanupFaultPoint::FirstRenameDurable};
        CHECK(begin_cleanup(base, store_id + 1, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved = temp.path() / "saved-owned-data";
        std::filesystem::rename(paths.data_path, saved);
        write_data(paths.data_path, actual_store_id_for(base, store_id + 1),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.data_path));
        CHECK(exists(saved));
        CHECK(exists(paths.quarantine_index_path));
    }

    {
        const auto base = temp.path() / "foreign-quarantine-data";
        write_pair(base, store_id + 2);
        StopContext stop{.target = OOCCleanupFaultPoint::DeleteAuthorizedDurable};
        CHECK(begin_cleanup(base, store_id + 2, stop_hooks(stop)).status ==
              OOCCleanupStatus::Interrupted);
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto saved = temp.path() / "saved-quarantine-data";
        std::filesystem::rename(paths.quarantine_data_path, saved);
        write_data(paths.quarantine_data_path, actual_store_id_for(base, store_id + 2),
                   OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
        CHECK(OOCCleanupTransaction::resume(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(exists(paths.quarantine_data_path));
        CHECK(exists(saved));
        CHECK(exists(paths.quarantine_index_path));
    }
}

void test_index_count_drift_is_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x3333'4444'5555'6666ULL;
    const auto base = temp.path() / "count-drift";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    overwrite_count(paths.index_path, 1);
    CHECK(OOCCleanupTransaction::resume(base).status ==
          OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(exists(paths.intent_path));
}

void test_staged_only_tail_has_no_delete_authority() {
    TempDirectory temp;
    constexpr std::uint64_t old_store_id = 0x4444'5555'6666'7777ULL;
    constexpr std::uint64_t new_store_id = 0x5555'6666'7777'8888ULL;
    const auto base = temp.path() / "staged-only";
    write_pair(base, old_store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::SecondUnlinkDurable};
    CHECK(begin_cleanup(base, old_store_id, stop_hooks(stop)).status ==
          OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    CHECK(exists(paths.intent_path));
    CHECK(exists(paths.staged_path));
    std::filesystem::remove(paths.intent_path);

    // A cooperating fresh writer must reject a staged-only namespace. Model an
    // external/noncooperating replacement directly to verify that staged alone
    // still cannot authorize deletion of the live pair.
    write_index(paths.index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE, new_store_id, 0,
                OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    write_data(paths.data_path, new_store_id, OOCRelationStoreFormat::DATA_HEADER_BYTES + 16);
    CHECK(OOCCleanupTransaction::resume(base).completed());
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(!exists(paths.staged_path));
}

void test_quarantine_collision_is_preserved() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x6666'7777'8888'9999ULL;
    const auto base = temp.path() / "quarantine-collision";
    write_pair(base, store_id);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    write_index(paths.quarantine_index_path, OOCRelationStoreFormat::MAGIC_V3_INCOMPLETE,
                store_id + 1, 0, OOCRelationStoreFormat::INDEX_HEADER_BYTES);
    CHECK(begin_cleanup(base, store_id).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(exists(paths.index_path));
    CHECK(exists(paths.data_path));
    CHECK(exists(paths.quarantine_index_path));
}

class HeldBaseLock final {
public:
    explicit HeldBaseLock(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("could not hold Win32 cleanup lock");
        }
#else
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor_ < 0 || ::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            if (descriptor_ >= 0) {
                (void)::close(descriptor_);
            }
            throw std::runtime_error("could not hold POSIX cleanup lock");
        }
#endif
    }

    HeldBaseLock(const HeldBaseLock&) = delete;
    HeldBaseLock& operator=(const HeldBaseLock&) = delete;

    ~HeldBaseLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

constexpr int LOCK_CONTENDER_BUSY_EXIT = 91;
constexpr int LOCK_HOLDER_CONFIRMED_EXIT = 92;

int run_lock_contender_child(const std::filesystem::path& base, std::uint64_t store_id) {
    (void)store_id;
    const auto result = OOCCleanupTransaction::resume(base);
    return result.status == OOCCleanupStatus::Busy ? LOCK_CONTENDER_BUSY_EXIT : 66;
}

int run_lock_holder_child(const std::string& executable, const std::filesystem::path& base,
                          std::uint64_t store_id) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    HeldBaseLock held(paths.lock_path);
    const auto contender = gnfs::test::run_child_process(
        executable, {"--contend-cleanup-lock", base.string(), std::to_string(store_id)});
    if (!contender.exited || contender.signaled ||
        contender.exit_code != LOCK_CONTENDER_BUSY_EXIT) {
        return 67;
    }
    return LOCK_HOLDER_CONFIRMED_EXIT;
}

void test_cross_process_lock_reports_busy(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x7777'8888'9999'aaaaULL;
    const auto base = temp.path() / "busy";
    write_pair(base, store_id);
    StopContext stop{.target = OOCCleanupFaultPoint::IntentDurable};
    CHECK(begin_cleanup(base, store_id, stop_hooks(stop)).status == OOCCleanupStatus::Interrupted);
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto holder = gnfs::test::run_child_process(
        executable, {"--hold-cleanup-lock", base.string(), std::to_string(store_id)});
    CHECK(holder.exited);
    CHECK(!holder.signaled);
    CHECK(holder.exit_code == LOCK_HOLDER_CONFIRMED_EXIT);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(OOCCleanupTransaction::resume(base).completed());
    check_cleanup_complete(paths);
}

void test_private_lease_uses_one_persistent_external_lock(const std::string& executable) {
    TempDirectory temp;
    const auto lease = temp.path() / "private.gnfs-sink-lease";
    const auto base = lease / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto frozen_temp = std::filesystem::weakly_canonical(temp.path());
    CHECK(paths.private_directory == frozen_temp / "private.gnfs-sink-lease");
    CHECK(paths.lock_path.parent_path() == frozen_temp);
    CHECK(paths.lock_path.parent_path() != paths.private_directory);

    auto first = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(first.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    constexpr std::uint64_t unused_store_id = 0x8899'aabb'ccdd'eeffULL;
    const auto contender = gnfs::test::run_child_process(
        executable, {"--contend-cleanup-lock", base.string(), std::to_string(unused_store_id)});
    CHECK(contender.exited);
    CHECK(!contender.signaled);
    CHECK(contender.exit_code == LOCK_CONTENDER_BUSY_EXIT);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(*first.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    auto second = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(second.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*second.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

void test_private_lease_receipt_rejects_replacement_directory() {
    TempDirectory temp;
    const auto lease = temp.path() / "aba.gnfs-sink-lease";
    const auto base = lease / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto replacement = temp.path() / "replacement-lease";
    const auto saved_owned = temp.path() / "saved-owned-lease";

    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    CHECK(std::filesystem::create_directory(replacement));

    std::error_code error;
    std::filesystem::rename(paths.private_directory, saved_owned, error);
    CHECK(!error);
    error.clear();
    std::filesystem::rename(replacement, paths.private_directory, error);
    CHECK(!error);

    const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
    CHECK(rejected.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));

    error.clear();
    CHECK(std::filesystem::remove(paths.private_directory, error));
    CHECK(!error);
    error.clear();
    std::filesystem::rename(saved_owned, paths.private_directory, error);
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lock_path));
}

constexpr std::array PRIVATE_LEASE_RESERVE_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::ReservedPendingDurable,
    OOCPrivateLeaseFaultPoint::ReservedDurable,
    OOCPrivateLeaseFaultPoint::StagingDirectoryDurable,
    OOCPrivateLeaseFaultPoint::OwnerPendingDurable,
    OOCPrivateLeaseFaultPoint::OwnerDurable,
    OOCPrivateLeaseFaultPoint::OwnedPendingDurable,
    OOCPrivateLeaseFaultPoint::OwnedDurable,
    OOCPrivateLeaseFaultPoint::FinalRenameDurable,
};

constexpr std::array PRIVATE_LEASE_REMOVE_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnerRemovedDurable,
    OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnedRemovedDurable,
};

constexpr std::array PRIVATE_WRITER_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::FreshIndexReserved,
    OOCPrivateLeaseFaultPoint::FreshDataReserved,
    OOCPrivateLeaseFaultPoint::FreshHeadersValidated,
    OOCPrivateLeaseFaultPoint::FreshPairOwnershipCaptured,
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
};

constexpr std::array PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS{
    OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable,
    OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable,
    OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnerRemovedDurable,
    OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable,
    OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    OOCPrivateLeaseFaultPoint::OwnedRemovedDurable,
};

constexpr int PRIVATE_LEASE_CRASH_EXIT_BASE = 140;

struct PrivateLeaseCrashContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::ReservedPendingDurable;
};

[[nodiscard]] bool crash_private_lease_at(OOCPrivateLeaseFaultPoint point, void* opaque) noexcept {
    const auto& context = *static_cast<const PrivateLeaseCrashContext*>(opaque);
    if (point == context.target) {
        std::_Exit(PRIVATE_LEASE_CRASH_EXIT_BASE + static_cast<int>(point));
    }
    return false;
}

[[nodiscard]] OOCPrivateLeaseTestHooks
private_lease_crash_hooks(PrivateLeaseCrashContext& context) noexcept {
    return OOCPrivateLeaseTestHooks{
        .stop_after = crash_private_lease_at,
        .context = &context,
    };
}

int run_private_lease_crash_child(std::string_view operation, std::size_t point_index,
                                  const std::filesystem::path& base) {
    if (operation == "reserve") {
        if (point_index >= PRIVATE_LEASE_RESERVE_FAULT_POINTS.size()) {
            return 64;
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_LEASE_RESERVE_FAULT_POINTS[point_index],
        };
        const auto result =
            OOCCleanupTransaction::reserve_private_lease(base, private_lease_crash_hooks(context));
        (void)result;
        return 65;
    }
    if (operation == "remove") {
        if (point_index >= PRIVATE_LEASE_REMOVE_FAULT_POINTS.size()) {
            return 64;
        }
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        if (!reservation.completed()) {
            return 66;
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_LEASE_REMOVE_FAULT_POINTS[point_index],
        };
        const auto result = OOCCleanupTransaction::remove_private_lease(
            *reservation.ownership, private_lease_crash_hooks(context));
        (void)result;
        return 67;
    }
    if (operation == "preactive-recover") {
        if (point_index >= PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS.size()) {
            return 64;
        }
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            if (!reservation.completed()) {
                return 68;
            }
            const auto paths = OOCCleanupTransaction::paths_for(base);
            write_test_leaf(paths.index_path, "partial preactive index");
            write_test_leaf(paths.data_path, "partial preactive data");
        }
        PrivateLeaseCrashContext context{
            .target = PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS[point_index],
        };
        const auto result =
            OOCCleanupTransaction::recover_private_lease(base, private_lease_crash_hooks(context));
        (void)result;
        return 69;
    }
    return 64;
}

int run_private_writer_crash_child(std::size_t point_index, const std::filesystem::path& base) {
    if (point_index >= PRIVATE_WRITER_FAULT_POINTS.size()) {
        return 64;
    }
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed()) {
        return 70;
    }
    PrivateLeaseCrashContext context{
        .target = PRIVATE_WRITER_FAULT_POINTS[point_index],
    };
    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             private_lease_crash_hooks(context));
    (void)writer;
    return 71;
}

constexpr int PRIVATE_LEASE_ABANDONED_EXIT = 93;

int run_private_lease_abandon_child(std::string_view scenario, const std::filesystem::path& base,
                                    std::uint64_t store_id) {
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    if (!reservation.completed() || store_id == 0) {
        return 66;
    }
    write_pair(base, store_id, *reservation.ownership);
    if (scenario == "live-pair") {
        return PRIVATE_LEASE_ABANDONED_EXIT;
    }
    if (scenario == "pending-only") {
        PublishStopContext stop{
            .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        };
        const auto interrupted = begin_cleanup(base, store_id, publish_stop_hooks(stop));
        return interrupted.status == OOCCleanupStatus::Interrupted ? PRIVATE_LEASE_ABANDONED_EXIT
                                                                   : 67;
    }
    if (scenario == "canonical-intent") {
        StopContext stop{
            .target = OOCCleanupFaultPoint::IntentDurable,
        };
        const auto interrupted = begin_cleanup(base, store_id, stop_hooks(stop));
        return interrupted.status == OOCCleanupStatus::Interrupted ? PRIVATE_LEASE_ABANDONED_EXIT
                                                                   : 68;
    }
    return 64;
}

[[nodiscard]] std::vector<std::filesystem::path>
private_lease_staging_entries(const gnfs::relation::OOCCleanupPaths& paths) {
    std::vector<std::filesystem::path> entries;
    const auto prefix =
        paths.private_directory.filename().generic_string() + ".gnfs-private-lease-v1.stage-";
    std::error_code error;
    std::filesystem::directory_iterator cursor(paths.private_directory.parent_path(), error);
    if (error) {
        throw std::filesystem::filesystem_error("inspect private lease staging entries",
                                                paths.private_directory.parent_path(), error);
    }
    for (const auto& entry : cursor) {
        if (entry.path().filename().generic_string().starts_with(prefix)) {
            entries.push_back(entry.path());
        }
    }
    return entries;
}

void check_empty_private_lease_recovery(const std::filesystem::path& base) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    if (!recovered.transaction_terminal()) {
        std::cerr << "private lease recovery did not converge for " << base
                  << ": status=" << static_cast<int>(recovered.status)
                  << " error=" << recovered.native_error.message() << '\n';
    }
    CHECK(recovered.transaction_terminal());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(private_lease_staging_entries(paths).empty());
    CHECK(entry_exists_no_follow(paths.lock_path));

    const auto repeated = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(repeated.status == OOCCleanupStatus::NoTransaction);

    auto fresh = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(fresh.completed());
    CHECK(entry_exists_no_follow(paths.private_directory));

    // A completed recovery must not retain stale authority over a fresh
    // generation created at the same path.
    const auto stale_retry = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(stale_retry.status == OOCCleanupStatus::Busy);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(OOCCleanupTransaction::remove_private_lease(*fresh.ownership).completed());
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

void test_private_lease_process_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_LEASE_RESERVE_FAULT_POINTS.size(); ++index) {
        const auto lease = temp.path() / ("reserve-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-lease", "reserve", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == PRIVATE_LEASE_CRASH_EXIT_BASE +
                                     static_cast<int>(PRIVATE_LEASE_RESERVE_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }

    for (std::size_t index = 0; index < PRIVATE_LEASE_REMOVE_FAULT_POINTS.size(); ++index) {
        const auto lease = temp.path() / ("remove-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-lease", "remove", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == PRIVATE_LEASE_CRASH_EXIT_BASE +
                                     static_cast<int>(PRIVATE_LEASE_REMOVE_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }
}

void test_private_lease_preactive_rollback_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS.size(); ++index) {
        const auto lease =
            temp.path() / ("preactive-recovery-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child =
            gnfs::test::run_child_process(executable, {"--crash-private-lease", "preactive-recover",
                                                       std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code ==
              PRIVATE_LEASE_CRASH_EXIT_BASE +
                  static_cast<int>(PRIVATE_PREACTIVE_RECOVERY_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }
}

void test_private_writer_preactivation_crash_recovery(const std::string& executable) {
    TempDirectory temp;

    // Every writer boundary before RESERVED is consumed remains rollback
    // state. Recovery owns the exact lease generation and removes any zero,
    // partial, or fully header-written pair left by process termination.
    for (std::size_t index = 0; index + 1 < PRIVATE_WRITER_FAULT_POINTS.size(); ++index) {
        const auto lease =
            temp.path() / ("writer-precommit-" + std::to_string(index) + ".gnfs-sink-lease");
        const auto base = lease / "corpus";
        const auto child = gnfs::test::run_child_process(
            executable, {"--crash-private-writer", std::to_string(index), base.string()});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code ==
              PRIVATE_LEASE_CRASH_EXIT_BASE + static_cast<int>(PRIVATE_WRITER_FAULT_POINTS[index]));
        check_empty_private_lease_recovery(base);
    }

    // RESERVED removal is the activation commit point. A crash after that
    // durable boundary must preserve the live pair and may not recreate
    // cleanup authority from OWNED alone.
    const std::size_t commit_index = PRIVATE_WRITER_FAULT_POINTS.size() - 1;
    const auto committed_base = temp.path() / "writer-committed.gnfs-sink-lease" / "corpus";
    const auto committed_paths = OOCCleanupTransaction::paths_for(committed_base);
    const auto child = gnfs::test::run_child_process(
        executable,
        {"--crash-private-writer", std::to_string(commit_index), committed_base.string()});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_CRASH_EXIT_BASE +
                                 static_cast<int>(PRIVATE_WRITER_FAULT_POINTS[commit_index]));
    CHECK(!entry_exists_no_follow(committed_paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(committed_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(committed_paths.private_directory));
    CHECK(entry_exists_no_follow(committed_paths.index_path));
    CHECK(entry_exists_no_follow(committed_paths.data_path));
    CHECK(private_lease_staging_entries(committed_paths).empty());
    CHECK(OOCCleanupTransaction::recover_private_lease(committed_base).status ==
          OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(committed_paths.index_path));
    CHECK(entry_exists_no_follow(committed_paths.data_path));
}

void test_private_lease_preactive_link_attacks_are_preserved() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "preactive-hardlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign = temp.path() / "preactive-hardlink-target";
        write_test_leaf(foreign, "foreign hardlink target");
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
            CHECK(create_hard_link_checked(foreign, paths.index_path));
        }

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(entry_exists_no_follow(foreign));
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(paths.index_path));
        check_entries_equivalent(foreign, paths.index_path);
        CHECK(private_lease_staging_entries(paths).empty());
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }

    {
        const auto base = temp.path() / "preactive-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign = temp.path() / "preactive-symlink-target";
        write_test_leaf(foreign, "foreign symlink target");
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
            if (!create_symlink_or_explicit_skip(foreign, paths.data_path,
                                                 "preactive pair symlink")) {
                return;
            }
        }

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(entry_exists_no_follow(foreign));
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_is_symlink_no_follow(paths.data_path));
        CHECK(private_lease_staging_entries(paths).empty());
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }
}

void test_private_lease_recovery_preserves_live_pair_without_intent(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0x9a9a'abab'bcbc'cdc0ULL;
    const auto base = temp.path() / "live-pair.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "live-pair",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}

void test_private_lease_recovery_preserves_pending_only_pair(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xa0a0'b1b1'c2c2'd3d3ULL;
    const auto base = temp.path() / "pending-only.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "pending-only",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
}

void test_private_lease_writer_activation_closes_reservation() {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xb0b0'c1c1'd2d2'e3e3ULL;
    const auto base = temp.path() / "writer-activation.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));

    write_pair(base, store_id, *reservation.ownership);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!reservation.ownership->spent());

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(begin_cleanup(base, store_id).completed());
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    CHECK(reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
}

void test_private_lease_recovery_finishes_canonical_pair_intent(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t store_id = 0xc0c0'd1d1'e2e2'f3f3ULL;
    const auto base = temp.path() / "canonical-intent.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto child =
        gnfs::test::run_child_process(executable, {"--abandon-private-lease", "canonical-intent",
                                                   base.string(), std::to_string(store_id)});
    CHECK(child.exited);
    CHECK(!child.signaled);
    CHECK(child.exit_code == PRIVATE_LEASE_ABANDONED_EXIT);
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));

    CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NoTransaction);
}

void test_deferred_private_writer_handoff_and_pending_recovery() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "deferred-handoff.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            const auto descriptor = writer.finalize_and_publish_cleanup_handoff();
            CHECK(descriptor.count == 0);
        }
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCRelationReader(base.string()).count() == 0);
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    }

    {
        const auto base = temp.path() / "deferred-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        bool interrupted = false;
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            PublishStopContext stop{
                .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
            };
            try {
                (void)writer.finalize_and_publish_cleanup_handoff(publish_stop_hooks(stop));
            } catch (const std::system_error&) {
                interrupted = true;
            }
        }
        CHECK(interrupted);
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(entry_exists_no_follow(paths.intent_pending_path));
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    }
}

void test_deferred_handoff_foreign_leaf_blocks_pair_mutation() {
    TempDirectory temp;
    const auto base = temp.path() / "handoff-foreign.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        (void)writer.finalize_and_publish_cleanup_handoff();
    }

    const auto foreign = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign, "foreign");
    const auto rejected = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
    CHECK(rejected.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(entry_exists_no_follow(foreign));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));

    std::error_code error;
    CHECK(std::filesystem::remove(foreign, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
}

void test_private_handoff_writer_rejects_metadata_before_finalize() {
    TempDirectory temp;
    const auto base = temp.path() / "private-handoff-invalid-metadata.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    std::optional<OOCSnapshotDescriptor> descriptor;
    {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
        (void)writer.write(make_real_relation(31, 37));

        bool zero_kind_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(0, PRIVATE_HANDOFF_PAYLOAD_VERSION,
                                                              PRIVATE_HANDOFF_PAYLOAD);
        } catch (const std::invalid_argument&) {
            zero_kind_rejected = true;
        }
        CHECK(zero_kind_rejected);
        (void)writer.write(make_real_relation(41, 43));

        bool zero_version_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(PRIVATE_HANDOFF_PAYLOAD_KIND, 0,
                                                              PRIVATE_HANDOFF_PAYLOAD);
        } catch (const std::invalid_argument&) {
            zero_version_rejected = true;
        }
        CHECK(zero_version_rejected);

        const std::vector<std::byte> oversized_payload(
            gnfs::relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U, std::byte{0x7f});
        bool oversized_rejected = false;
        try {
            (void)writer.finalize_and_publish_private_handoff(
                PRIVATE_HANDOFF_PAYLOAD_KIND, PRIVATE_HANDOFF_PAYLOAD_VERSION, oversized_payload);
        } catch (const std::invalid_argument&) {
            oversized_rejected = true;
        }
        CHECK(oversized_rejected);
        (void)writer.write(make_real_relation(47, 53));

        descriptor = writer.finalize_and_publish_cleanup_handoff();
    }
    CHECK(descriptor.has_value());
    if (descriptor) {
        CHECK(descriptor->count == 3);
    }
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(entry_exists_no_follow(paths.intent_path));
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

void test_private_handoff_transaction_rejects_oversize_before_mutation() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-handoff-oversize-transaction.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const std::vector<std::byte> oversized_payload(
        gnfs::relation::OOC_PRIVATE_HANDOFF_MAX_OPAQUE_PAYLOAD_BYTES + 1U, std::byte{0x6a});

    const auto rejected = OOCCleanupTransaction::publish_private_handoff(
        prepared.pair_ownership, prepared.lease_ownership,
        handoff_pair_descriptor(prepared.descriptor), PRIVATE_HANDOFF_PAYLOAD_KIND,
        PRIVATE_HANDOFF_PAYLOAD_VERSION, oversized_payload);
    CHECK(rejected.result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(!rejected.canonical());
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
    CHECK(prepared.lease_ownership.spent());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

#if !defined(__APPLE__)
void test_private_handoff_unsupported_publish_is_non_mutating() {
    TempDirectory temp;
    const auto base =
        temp.path() / "unsupported-private-handoff-publish.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

    const auto rejected = publish_private_handoff(prepared);
    CHECK(rejected.result.status == OOCCleanupStatus::PlatformUnsupported);
    CHECK(rejected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!rejected.canonical());
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
    CHECK(prepared.lease_ownership.spent());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

struct UnsupportedAdoptionHookContext final {
    std::size_t observations = 0;
};

[[nodiscard]] bool observe_unsupported_adoption(OOCPrivateHandoffAdoptionFaultPoint,
                                                void* opaque) noexcept {
    auto& context = *static_cast<UnsupportedAdoptionHookContext*>(opaque);
    ++context.observations;
    return false;
}

void test_private_handoff_unsupported_adoption_is_non_observing() {
    TempDirectory temp;
    UnsupportedAdoptionHookContext context;
    const auto hooks = OOCPrivateHandoffAdoptionTestHooks{
        .stop_after = observe_unsupported_adoption,
        .context = &context,
    };

    {
        const auto base = temp.path() / "unsupported-adoption-missing.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto unsupported = OOCCleanupTransaction::adopt_private_handoff(base, hooks);
        CHECK(unsupported.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!unsupported.adopted());
        CHECK(!unsupported.adoption.has_value());
        CHECK(context.observations == 0);
        CHECK(!entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }

    {
        const auto base = temp.path() / "unsupported-adoption-existing.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        CHECK(std::filesystem::create_directory(paths.private_directory));
        const auto sentinel = paths.private_directory / "unobserved-sentinel";
        write_test_leaf(sentinel, "unsupported adoption must not inspect this directory");
        const auto sentinel_bytes = read_test_bytes(sentinel);
        const auto unsupported = OOCCleanupTransaction::adopt_private_handoff(base, hooks);
        CHECK(unsupported.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!unsupported.adopted());
        CHECK(!unsupported.adoption.has_value());
        CHECK(context.observations == 0);
        CHECK(read_test_bytes(sentinel) == sentinel_bytes);
        CHECK(!entry_exists_no_follow(paths.lock_path));
    }

    const auto invalid = OOCCleanupTransaction::adopt_private_handoff({}, hooks);
    CHECK(invalid.result.status == OOCCleanupStatus::InvalidRequest);
    CHECK(!invalid.adopted());
    CHECK(!invalid.adoption.has_value());
    CHECK(context.observations == 0);
}
#endif

[[nodiscard]] std::filesystem::path
private_lease_stage_path_for_token(const gnfs::relation::OOCCleanupPaths& paths,
                                   std::string_view token) {
    auto leaf = paths.private_directory.filename().native();
    leaf.append(std::filesystem::path(".gnfs-private-lease-v1.stage-").native());
    leaf.append(std::filesystem::path(std::string(token)).native());
    return paths.private_directory.parent_path() / std::filesystem::path(std::move(leaf));
}

void check_missing_lock_orphan_stage_conflict(
    const std::filesystem::path& base, const std::filesystem::path& stage_path,
    const std::filesystem::path& sentinel_path,
    const std::optional<std::array<std::uint64_t, 3>>& expected_identity) {
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto sentinel_bytes = read_test_bytes(sentinel_path);
    CHECK(!entry_exists_no_follow(paths.lock_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);
    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());

    CHECK(!entry_exists_no_follow(paths.lock_path));
    CHECK(entry_exists_no_follow(stage_path));
    CHECK(read_test_bytes(sentinel_path) == sentinel_bytes);
    if (expected_identity) {
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(stage_path) ==
              expected_identity);
    }
}

void test_private_handoff_missing_lock_orphan_stage_is_preserved() {
    TempDirectory temp;
    constexpr std::string_view EXACT_TOKEN = "0123456789abcdef0123456789abcdef";

    {
        const auto base = temp.path() / "orphan-stage.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto stage_path = private_lease_stage_path_for_token(paths, EXACT_TOKEN);
        const auto sentinel_path = stage_path / "sentinel";
        std::error_code error;
        CHECK(std::filesystem::create_directory(stage_path, error));
        CHECK(!error);
        write_test_leaf(sentinel_path, "orphan stage sentinel");
        const auto identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(stage_path);
        CHECK(identity.has_value());

        check_missing_lock_orphan_stage_conflict(base, stage_path, sentinel_path, identity);
    }

    {
        const auto base = temp.path() / "orphan-stage-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto stage_path = private_lease_stage_path_for_token(paths, EXACT_TOKEN);
        const auto target = temp.path() / "orphan-stage-symlink-target";
        const auto sentinel_path = target / "sentinel";
        std::error_code error;
        CHECK(std::filesystem::create_directory(target, error));
        CHECK(!error);
        write_test_leaf(sentinel_path, "orphan symlink stage sentinel");
        if (create_symlink_or_explicit_skip(target, stage_path,
                                            "missing-lock private lease stage symlink")) {
            check_missing_lock_orphan_stage_conflict(base, stage_path, sentinel_path, std::nullopt);
            CHECK(entry_is_symlink_no_follow(stage_path));
            check_entries_equivalent(stage_path, target);
        }
    }
}

void test_private_handoff_invalid_orphan_stage_names_are_ignored() {
    TempDirectory temp;
    const auto base = temp.path() / "invalid-orphan-stages.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    constexpr std::array<std::string_view, 5> INVALID_TOKENS{
        "0123456789abcdef0123456789abcde",        "0123456789abcdef0123456789abcdef0",
        "0123456789abcdef0123456789abcdeF",       "0123456789abcdef0123456789abcdeg",
        "0123456789abcdef0123456789abcdef.extra",
    };

    std::array<std::filesystem::path, INVALID_TOKENS.size()> stage_paths;
    std::array<std::optional<std::array<std::uint64_t, 3>>, INVALID_TOKENS.size()> identities;
    std::array<std::vector<std::byte>, INVALID_TOKENS.size()> sentinel_bytes;
    for (std::size_t index = 0; index < INVALID_TOKENS.size(); ++index) {
        stage_paths[index] = private_lease_stage_path_for_token(paths, INVALID_TOKENS[index]);
        std::error_code error;
        CHECK(std::filesystem::create_directory(stage_paths[index], error));
        CHECK(!error);
        write_test_leaf(stage_paths[index] / "sentinel", std::to_string(index));
        sentinel_bytes[index] = read_test_bytes(stage_paths[index] / "sentinel");
        identities[index] = gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
            stage_paths[index]);
        CHECK(identities[index].has_value());
    }

    CHECK(!entry_exists_no_follow(paths.lock_path));
    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NoTransaction);
    CHECK(inspected.state == OOCPrivateHandoffState::None);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NoTransaction);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    if (reservation.completed()) {
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        CHECK(reservation.ownership->spent());
    }

    for (std::size_t index = 0; index < INVALID_TOKENS.size(); ++index) {
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  stage_paths[index]) == identities[index]);
        CHECK(read_test_bytes(stage_paths[index] / "sentinel") == sentinel_bytes[index]);
    }
}

[[nodiscard]] bool is_preserving_namespace_status(OOCCleanupStatus status) {
    return status == OOCCleanupStatus::NamespaceConflict ||
           status == OOCCleanupStatus::ForeignReplacementPreserved;
}

struct PrivateLeaseStopOnceContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::OwnedDurable;
    bool stopped = false;
};

[[nodiscard]] bool stop_private_lease_once(OOCPrivateLeaseFaultPoint point, void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseStopOnceContext*>(opaque);
    if (!context.stopped && point == context.target) {
        context.stopped = true;
        return true;
    }
    return false;
}

struct PrivateLeasePostSyncReplacementContext final {
    std::filesystem::path reserved_path;
    bool invoked = false;
    bool injected = false;
};

[[nodiscard]] bool inject_reserved_replacement_after_commit(OOCPrivateLeaseFaultPoint point,
                                                            void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeasePostSyncReplacementContext*>(opaque);
    if (context.invoked || point != OOCPrivateLeaseFaultPoint::ReservedRemovedDurable) {
        return false;
    }
    context.invoked = true;
    try {
        std::ofstream stream(context.reserved_path, std::ios::binary | std::ios::out);
        constexpr std::string_view payload = "foreign RESERVED replacement";
        stream.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        stream.flush();
        stream.close();
        context.injected = stream.good();
    } catch (...) {
        context.injected = false;
    }
    return false;
}

void test_private_lease_unknown_child_preserves_matching_pending() {
    TempDirectory temp;

    {
        const auto base =
            temp.path() / "final-unknown-before-pending-cleanup.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "foreign-control-leaf";
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_test_leaf(foreign_path, "foreign final child");

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto reserved_pending_bytes = read_test_bytes(paths.lease_reserved_pending_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owned_pending_bytes = read_test_bytes(paths.lease_owned_pending_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        const auto directory_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                paths.private_directory);

        const auto removed = OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership);
        CHECK(is_preserving_namespace_status(removed.status));
        CHECK(!prepared.lease_ownership.spent());
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  paths.private_directory) == directory_identity);

        std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
        stale_lease.emplace(std::move(prepared.lease_ownership));
        stale_lease.reset();
        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(is_preserving_namespace_status(recovered.status));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(paths.index_path, index_bytes);
        check_test_bytes_preserved(paths.data_path, data_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(!prepared.pair_ownership.spent());
    }

    {
        const auto base =
            temp.path() / "staging-unknown-before-pending-cleanup.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        PrivateLeaseStopOnceContext stop{
            .target = OOCPrivateLeaseFaultPoint::OwnedDurable,
        };
        const auto reservation = OOCCleanupTransaction::reserve_private_lease(
            base, OOCPrivateLeaseTestHooks{
                      .stop_after = stop_private_lease_once,
                      .context = &stop,
                  });
        CHECK(reservation.result.status == OOCCleanupStatus::Interrupted);
        CHECK(!reservation.ownership.has_value());
        CHECK(stop.stopped);
        const auto staging = private_lease_staging_entries(paths);
        CHECK(staging.size() == 1);
        if (staging.size() != 1) {
            return;
        }
        const auto owner_path = staging.front() / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = staging.front() / "foreign-control-leaf";
        write_private_control_bytes(paths.lease_reserved_pending_path,
                                    read_test_bytes(paths.lease_reserved_path));
        write_private_control_bytes(paths.lease_owned_pending_path,
                                    read_test_bytes(paths.lease_owned_path));
        write_test_leaf(foreign_path, "foreign staging child");

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto reserved_pending_bytes = read_test_bytes(paths.lease_reserved_pending_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owned_pending_bytes = read_test_bytes(paths.lease_owned_pending_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        const auto staging_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(staging.front());

        const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
        CHECK(is_preserving_namespace_status(recovered.status));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_reserved_pending_path, reserved_pending_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(paths.lease_owned_pending_path, owned_pending_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  staging.front()) == staging_identity);
    }
}

struct FreshWriterBoundaryContext final {
    OOCPrivateLeaseFaultPoint observe = OOCPrivateLeaseFaultPoint::FreshIndexReserved;
    std::filesystem::path foreign_path;
    bool observed = false;
    bool injected = false;
    bool injection_failed = false;
};

[[nodiscard]] bool observe_or_inject_fresh_writer_boundary(OOCPrivateLeaseFaultPoint point,
                                                           void* opaque) noexcept {
    auto& context = *static_cast<FreshWriterBoundaryContext*>(opaque);
    if (point != context.observe) {
        return false;
    }
    context.observed = true;
    if (!context.foreign_path.empty()) {
        try {
            write_test_leaf(context.foreign_path, "injected activation foreign child");
            context.injected = true;
        } catch (...) {
            context.injection_failed = true;
        }
    }
    return point == OOCPrivateLeaseFaultPoint::FreshIndexReserved;
}

void test_private_lease_unknown_scan_precedes_writer_mutation() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "unknown-before-fresh-writer.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "foreign-control-leaf";
        write_test_leaf(foreign_path, "foreign before writer");
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        const auto foreign_bytes = read_test_bytes(foreign_path);
        FreshWriterBoundaryContext boundary;

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCPrivateLeaseTestHooks{
                                         .stop_after = observe_or_inject_fresh_writer_boundary,
                                         .context = &boundary,
                                     });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!boundary.observed);
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        check_test_bytes_preserved(foreign_path, foreign_bytes);
        CHECK(!reservation.ownership->spent());
    }

    {
        const auto base = temp.path() / "unknown-before-reserved-revoke.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto foreign_path = paths.private_directory / "activation-foreign-control-leaf";
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        FreshWriterBoundaryContext boundary{
            .observe = OOCPrivateLeaseFaultPoint::FreshPairOwnershipCaptured,
            .foreign_path = foreign_path,
        };

        bool rejected = false;
        try {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCPrivateLeaseTestHooks{
                                         .stop_after = observe_or_inject_fresh_writer_boundary,
                                         .context = &boundary,
                                     });
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(boundary.observed);
        CHECK(boundary.injected);
        CHECK(!boundary.injection_failed);
        CHECK(rejected);
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        if (boundary.injected) {
            constexpr std::string_view EXPECTED = "injected activation foreign child";
            const auto actual = read_test_bytes(foreign_path);
            CHECK(actual.size() == EXPECTED.size());
            CHECK(std::equal(actual.begin(), actual.end(), EXPECTED.begin(), EXPECTED.end(),
                             [](std::byte byte, char character) {
                                 return std::to_integer<unsigned char>(byte) ==
                                        static_cast<unsigned char>(character);
                             }));
        }
        CHECK(!reservation.ownership->spent());
    }
}

void test_unscoped_writer_rejects_existing_preactive_private_lease() {
    TempDirectory temp;

    for (const bool add_unknown_child : {false, true}) {
        const std::string lease_name = add_unknown_child ? "unscoped-writer-preactive-unknown"
                                                         : "unscoped-writer-preactive-clean";
        const auto base = temp.path() / (lease_name + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());

        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto unknown_path = paths.private_directory / "foreign-control-leaf";
        if (add_unknown_child) {
            write_test_leaf(unknown_path, "unscoped writer foreign child");
        }

        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
        const auto owner_bytes = read_test_bytes(owner_path);
        std::optional<std::vector<std::byte>> unknown_bytes;
        if (add_unknown_child) {
            unknown_bytes = read_test_bytes(unknown_path);
        }
        const auto directory_identity =
            gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                paths.private_directory);
        CHECK(directory_identity.has_value());
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));

        // Leave the durable preactive namespace in place but release the live
        // lock, so rejection must come from the namespace gate rather than Busy.
        reservation.ownership.reset();
        CHECK(!reservation.ownership.has_value());
        CHECK(entry_exists_no_follow(paths.lock_path));

        bool rejected = false;
        std::optional<OOCRelationWriter> writer;
        try {
            writer.emplace(base.string());
        } catch (const std::system_error&) {
            rejected = true;
        }
        CHECK(rejected);
        CHECK(!writer.has_value());

        // Keep an erroneously constructed writer alive through these checks so
        // its destructor cannot hide an O_EXCL pair mutation.
        CHECK(!entry_exists_no_follow(paths.index_path));
        CHECK(!entry_exists_no_follow(paths.data_path));
        check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
        check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
        check_test_bytes_preserved(owner_path, owner_bytes);
        if (unknown_bytes.has_value()) {
            check_test_bytes_preserved(unknown_path, *unknown_bytes);
        }
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
        CHECK(gnfs::relation::ooc_cleanup_detail::inspect_directory_identity_locked(
                  paths.private_directory) == directory_identity);

        writer.reset();
    }
}

void test_private_lease_unknown_scan_precedes_legacy_intent_publication() {
    TempDirectory temp;
    const auto base = temp.path() / "unknown-before-legacy-intent.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(59, 61));
    const auto descriptor = writer.finalize();
    CHECK(descriptor.count == 1);
    const auto foreign_path = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign_path, "foreign before legacy intent");

    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto foreign_bytes = read_test_bytes(foreign_path);
    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff();
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(foreign_path, foreign_bytes);
    CHECK(!reservation.ownership->spent());
}

void test_private_lease_activation_interrupt_after_commit_preserves_pair() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-activation-commit-interrupt.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    PrivateLeaseStopOnceContext interruption{
        .target = OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = stop_private_lease_once,
                                     .context = &interruption,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(interruption.stopped);
    CHECK(rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::RecoveryRequired);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

void test_private_lease_activation_post_sync_failure_preserves_pair() {
    TempDirectory temp;
    const auto base =
        temp.path() / "private-activation-post-sync-failure.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    PrivateLeasePostSyncReplacementContext replacement{
        .reserved_path = paths.lease_reserved_path,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = inject_reserved_replacement_after_commit,
                                     .context = &replacement,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(replacement.invoked);
    CHECK(replacement.injected);
    CHECK(rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentCorrupt);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

#if !defined(_WIN32)
[[nodiscard]] bool replace_test_lock_leaf(const std::filesystem::path& lock_path,
                                          const std::filesystem::path& saved_lock_path) noexcept {
    if (::rename(lock_path.c_str(), saved_lock_path.c_str()) != 0) {
        return false;
    }
    const int descriptor = ::open(lock_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }

    constexpr std::string_view payload = "foreign replacement lock";
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = ::write(descriptor, payload.data() + written, payload.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    int sync_result = -1;
    if (written == payload.size()) {
        do {
            sync_result = ::fsync(descriptor);
        } while (sync_result != 0 && errno == EINTR);
    }
    const bool synced = sync_result == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}

[[nodiscard]] bool replace_test_leaf_with_bytes(const std::filesystem::path& path,
                                                const std::filesystem::path& saved_path,
                                                std::span<const std::byte> bytes) noexcept {
    if (::rename(path.c_str(), saved_path.c_str()) != 0) {
        return false;
    }
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto result = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    int sync_result = -1;
    if (written == bytes.size()) {
        do {
            sync_result = ::fsync(descriptor);
        } while (sync_result != 0 && errno == EINTR);
    }
    const bool synced = sync_result == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}

[[nodiscard]] bool create_test_leaf_noexcept(const std::filesystem::path& path,
                                             std::string_view payload) noexcept {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = ::write(descriptor, payload.data() + written, payload.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    const bool complete = written == payload.size();
    const bool closed = ::close(descriptor) == 0;
    return complete && closed;
}

#if defined(__APPLE__)
struct ProtectedPrivateHandoffBytes final {
    std::vector<std::byte> canonical;
    std::vector<std::byte> index;
    std::vector<std::byte> data;
    std::vector<std::byte> owner;
    std::vector<std::byte> owned;
};

[[nodiscard]] ProtectedPrivateHandoffBytes
capture_protected_private_handoff_bytes(const OOCCleanupPaths& paths) {
    return {
        .canonical = read_test_bytes(paths.private_handoff_path),
        .index = read_test_bytes(paths.index_path),
        .data = read_test_bytes(paths.data_path),
        .owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory)),
        .owned = read_test_bytes(paths.lease_owned_path),
    };
}

void check_protected_private_handoff_bytes(const OOCCleanupPaths& paths,
                                           const ProtectedPrivateHandoffBytes& expected) {
    check_test_bytes_preserved(paths.private_handoff_path, expected.canonical);
    check_test_bytes_preserved(paths.index_path, expected.index);
    check_test_bytes_preserved(paths.data_path, expected.data);
    check_test_bytes_preserved(
        gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
        expected.owner);
    check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_owned_pending_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

[[nodiscard]] std::size_t count_open_test_descriptors() {
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator cursor("/dev/fd", error), end; !error && cursor != end;
         cursor.increment(error)) {
        ++count;
    }
    if (error) {
        throw std::filesystem::filesystem_error("enumerate /dev/fd", "/dev/fd", error);
    }
    return count;
}

void check_adopted_private_handoff_reader(OOCPrivateHandoffReader& adopted,
                                          std::size_t expected_count) {
    CHECK(adopted.valid());
    CHECK(adopted.reader().count() == expected_count);
    CHECK(adopted.record().pair.count == expected_count);
    CHECK(adopted.record().index.identity == adopted.index_snapshot().identity);
    CHECK(adopted.record().index.extent == adopted.index_snapshot().size);
    CHECK(adopted.record().data.identity == adopted.data_snapshot().identity);
    CHECK(adopted.record().data.extent == adopted.data_snapshot().size);
    if (expected_count == 1) {
        const auto relation = adopted.reader().read(0);
        CHECK(relation.a == 17);
        CHECK(relation.b == 19);
        CHECK(relation.rational_factors == std::vector<std::uint32_t>{117});
        CHECK(relation.algebraic_factors == std::vector<std::uint32_t>{217});
    }
}

void test_private_handoff_cross_process_adoption(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "cross-process-adoption.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    CHECK(entry_exists_no_follow(paths.lock_path));
    CHECK(entry_exists_no_follow(paths.private_directory));
    const auto expected = capture_protected_private_handoff_bytes(paths);
    check_protected_private_handoff_bytes(paths, expected);

    {
        auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(adopted.adopted());
        CHECK(adopted.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(adopted.state == OOCPrivateHandoffState::Canonical);
        CHECK(adopted.adoption.has_value());
        OOCPrivateHandoffAdoptionReceipt receipt(std::move(*adopted.adoption));
        CHECK(adopted.adoption->spent());
        CHECK(!receipt.spent());
        OOCPrivateHandoffReader reader(std::move(receipt));
        CHECK(receipt.spent());
        check_adopted_private_handoff_reader(reader, 1);

        const auto busy = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(busy.result.status == OOCCleanupStatus::Busy);
        CHECK(!busy.adopted());
        CHECK(!busy.adoption.has_value());
        check_protected_private_handoff_bytes(paths, expected);

        OOCPrivateHandoffReader moved(std::move(reader));
        CHECK(!reader.valid());
        check_adopted_private_handoff_reader(moved, 1);
    }

    {
        auto reopened = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(reopened.adopted());
        OOCPrivateHandoffReader reader(std::move(*reopened.adoption));
        check_adopted_private_handoff_reader(reader, 1);
    }
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::HandoffPresent);
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_adopter_owner_death(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "adopter-owner-death.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    const auto expected = capture_protected_private_handoff_bytes(paths);

    const auto adopter = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "adopt-exit", base.string()});
    CHECK(adopter.exited);
    CHECK(!adopter.signaled);
    CHECK(adopter.exit_code == 0);
    check_protected_private_handoff_bytes(paths, expected);

    {
        auto recovered = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(recovered.adopted());
        OOCPrivateHandoffReader reader(std::move(*recovered.adoption));
        check_adopted_private_handoff_reader(reader, 1);
    }
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_zero_row_adoption(const std::string& executable) {
    TempDirectory temp;
    const auto base = temp.path() / "zero-row-adoption.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto publisher = gnfs::test::run_child_process(
        executable, {"--private-handoff-adoption-child", "publish-empty-exit", base.string()});
    CHECK(publisher.exited);
    CHECK(!publisher.signaled);
    CHECK(publisher.exit_code == 0);
    const auto expected = capture_protected_private_handoff_bytes(paths);

    {
        auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(adopted.adopted());
        OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
        check_adopted_private_handoff_reader(reader, 0);
    }
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::HandoffPresent);
    check_protected_private_handoff_bytes(paths, expected);
}

void test_private_handoff_adoption_publication_prefixes(const std::string& executable) {
    TempDirectory temp;

    {
        const auto base = temp.path() / "pending-prefix-adoption.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher =
            gnfs::test::run_child_process(executable, {"--private-handoff-adoption-child",
                                                       "publish-pending-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
        const auto pending = read_test_bytes(paths.private_handoff_pending_path);
        const auto index = read_test_bytes(paths.index_path);
        const auto data = read_test_bytes(paths.data_path);
        const auto reserved = read_test_bytes(paths.lease_reserved_path);
        const auto owned = read_test_bytes(paths.lease_owned_path);
        const auto owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory));
        const auto descriptors_before = count_open_test_descriptors();

        const auto observed = OOCCleanupTransaction::adopt_private_handoff(base);
        CHECK(observed.result.status == OOCCleanupStatus::RecoveryRequired);
        CHECK(observed.state == OOCPrivateHandoffState::PendingOnly);
        CHECK(!observed.adopted());
        CHECK(!observed.adoption.has_value());
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_test_bytes_preserved(paths.private_handoff_pending_path, pending);
        check_test_bytes_preserved(paths.index_path, index);
        check_test_bytes_preserved(paths.data_path, data);
        check_test_bytes_preserved(paths.lease_reserved_path, reserved);
        check_test_bytes_preserved(paths.lease_owned_path, owned);
        check_test_bytes_preserved(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
            owner);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
    }

    {
        const auto base = temp.path() / "canonical-prefix-adoption.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher =
            gnfs::test::run_child_process(executable, {"--private-handoff-adoption-child",
                                                       "publish-canonical-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        const auto canonical = read_test_bytes(paths.private_handoff_path);
        const auto index = read_test_bytes(paths.index_path);
        const auto data = read_test_bytes(paths.data_path);
        const auto reserved = read_test_bytes(paths.lease_reserved_path);
        const auto owned = read_test_bytes(paths.lease_owned_path);
        const auto owner = read_test_bytes(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory));
        const bool pending_present = entry_exists_no_follow(paths.private_handoff_pending_path);
        const auto pending = pending_present
                                 ? std::optional<std::vector<std::byte>>(
                                       read_test_bytes(paths.private_handoff_pending_path))
                                 : std::nullopt;

        {
            auto adopted = OOCCleanupTransaction::adopt_private_handoff(base);
            CHECK(adopted.adopted());
            OOCPrivateHandoffReader reader(std::move(*adopted.adoption));
            check_adopted_private_handoff_reader(reader, 1);
        }
        check_test_bytes_preserved(paths.private_handoff_path, canonical);
        check_test_bytes_preserved(paths.index_path, index);
        check_test_bytes_preserved(paths.data_path, data);
        check_test_bytes_preserved(paths.lease_reserved_path, reserved);
        check_test_bytes_preserved(paths.lease_owned_path, owned);
        check_test_bytes_preserved(
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
            owner);
        CHECK(entry_exists_no_follow(paths.private_handoff_pending_path) == pending_present);
        if (pending) {
            check_test_bytes_preserved(paths.private_handoff_pending_path, *pending);
        }
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
    }
}

constexpr std::array PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS{
    OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
    OOCPrivateHandoffAdoptionFaultPoint::IndexInitialValidationComplete,
    OOCPrivateHandoffAdoptionFaultPoint::IndexOpened,
    OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete,
    OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
    OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
    OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
};

struct PrivateHandoffAdoptionStopContext final {
    OOCPrivateHandoffAdoptionFaultPoint target =
        OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified;
    bool stopped = false;
};

[[nodiscard]] bool stop_private_handoff_adoption(OOCPrivateHandoffAdoptionFaultPoint point,
                                                 void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffAdoptionStopContext*>(opaque);
    if (point != context.target) {
        return false;
    }
    context.stopped = true;
    return true;
}

void test_private_handoff_adoption_interruptions(const std::string& executable) {
    TempDirectory temp;
    for (std::size_t index = 0; index < PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() /
                          ("adoption-interruption-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher = gnfs::test::run_child_process(
            executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        const auto expected = capture_protected_private_handoff_bytes(paths);
        const auto descriptors_before = count_open_test_descriptors();

        PrivateHandoffAdoptionStopContext context{
            .target = PRIVATE_HANDOFF_ADOPTION_FAULT_POINTS[index],
        };
        {
            const auto interrupted = OOCCleanupTransaction::adopt_private_handoff(
                base, OOCPrivateHandoffAdoptionTestHooks{
                          .stop_after = stop_private_handoff_adoption,
                          .context = &context,
                      });
            CHECK(context.stopped);
            CHECK(interrupted.result.status == OOCCleanupStatus::Interrupted);
            CHECK(!interrupted.adopted());
            CHECK(!interrupted.adoption.has_value());
        }
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_protected_private_handoff_bytes(paths, expected);

        {
            auto retried = OOCCleanupTransaction::adopt_private_handoff(base);
            CHECK(retried.adopted());
            OOCPrivateHandoffReader reader(std::move(*retried.adoption));
            check_adopted_private_handoff_reader(reader, 1);
        }
        check_protected_private_handoff_bytes(paths, expected);
    }
}

enum class PrivateHandoffAdoptionMutation : std::uint8_t {
    ReplaceCanonical,
    RemoveCanonical,
    RemoveIndex,
    ReplaceIndex,
    ReplaceData,
    ReplaceOwner,
    ReplaceOwned,
    AddUnknown,
    AddLegacyIntent,
    ReplaceDirectory,
    ReplaceLock,
};

struct PrivateHandoffAdoptionMutationContext final {
    OOCPrivateHandoffAdoptionFaultPoint target =
        OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified;
    PrivateHandoffAdoptionMutation mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical;
    std::filesystem::path path;
    std::filesystem::path saved_path;
    std::filesystem::path replacement_path;
    std::vector<std::byte> bytes;
    bool invoked = false;
    bool succeeded = false;
};

[[nodiscard]] bool mutate_private_handoff_adoption(OOCPrivateHandoffAdoptionFaultPoint point,
                                                   void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffAdoptionMutationContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    switch (context.mutation) {
    case PrivateHandoffAdoptionMutation::ReplaceCanonical:
    case PrivateHandoffAdoptionMutation::ReplaceIndex:
    case PrivateHandoffAdoptionMutation::ReplaceData:
    case PrivateHandoffAdoptionMutation::ReplaceOwner:
    case PrivateHandoffAdoptionMutation::ReplaceOwned:
        context.succeeded =
            replace_test_leaf_with_bytes(context.path, context.saved_path, context.bytes);
        break;
    case PrivateHandoffAdoptionMutation::RemoveCanonical:
    case PrivateHandoffAdoptionMutation::RemoveIndex:
        context.succeeded = ::rename(context.path.c_str(), context.saved_path.c_str()) == 0;
        break;
    case PrivateHandoffAdoptionMutation::AddUnknown:
    case PrivateHandoffAdoptionMutation::AddLegacyIntent:
        context.succeeded = create_test_leaf_noexcept(context.path, "adoption attack leaf");
        break;
    case PrivateHandoffAdoptionMutation::ReplaceDirectory:
        context.succeeded = ::rename(context.path.c_str(), context.saved_path.c_str()) == 0 &&
                            ::rename(context.replacement_path.c_str(), context.path.c_str()) == 0;
        break;
    case PrivateHandoffAdoptionMutation::ReplaceLock:
        context.succeeded = replace_test_lock_leaf(context.path, context.saved_path);
        break;
    }
    return false;
}

struct PrivateHandoffAdoptionAttackCase final {
    std::string_view label;
    OOCPrivateHandoffAdoptionFaultPoint point;
    PrivateHandoffAdoptionMutation mutation;
    OOCCleanupStatus expected_status;
};

constexpr std::array PRIVATE_HANDOFF_ADOPTION_ATTACKS{
    PrivateHandoffAdoptionAttackCase{
        .label = "canonical-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "canonical-missing-before-final",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::RemoveCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "index-missing-before-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::RemoveIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "index-replacement-after-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::IndexOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "data-replacement-during-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataInitialValidationComplete,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceData,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "data-replacement-after-open",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceData,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "owner-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwner,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "owned-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwned,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "unknown-leaf",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::AddUnknown,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "legacy-intent",
        .point = OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified,
        .mutation = PrivateHandoffAdoptionMutation::AddLegacyIntent,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "directory-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::DataOpened,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceDirectory,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "lock-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeFinalRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceLock,
        .expected_status = OOCCleanupStatus::NamespaceConflict,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-canonical-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceCanonical,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-index-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceIndex,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-owned-replacement",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::ReplaceOwned,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
    PrivateHandoffAdoptionAttackCase{
        .label = "late-unknown-leaf",
        .point = OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation,
        .mutation = PrivateHandoffAdoptionMutation::AddUnknown,
        .expected_status = OOCCleanupStatus::ForeignReplacementPreserved,
    },
};

void configure_private_handoff_adoption_attack(const PrivateHandoffAdoptionAttackCase& attack,
                                               const OOCCleanupPaths& paths,
                                               const std::filesystem::path& scratch,
                                               PrivateHandoffAdoptionMutationContext& context) {
    context.target = attack.point;
    context.mutation = attack.mutation;
    context.saved_path = scratch / (std::string(attack.label) + ".saved");
    switch (attack.mutation) {
    case PrivateHandoffAdoptionMutation::ReplaceCanonical:
    case PrivateHandoffAdoptionMutation::RemoveCanonical:
        context.path = paths.private_handoff_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::RemoveIndex:
    case PrivateHandoffAdoptionMutation::ReplaceIndex:
        context.path = paths.index_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceData:
        context.path = paths.data_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceOwner:
        context.path =
            gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory);
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceOwned:
        context.path = paths.lease_owned_path;
        context.bytes = read_test_bytes(context.path);
        break;
    case PrivateHandoffAdoptionMutation::AddUnknown:
        context.path = paths.private_directory / "foreign-adoption-leaf";
        break;
    case PrivateHandoffAdoptionMutation::AddLegacyIntent:
        context.path = paths.intent_path;
        break;
    case PrivateHandoffAdoptionMutation::ReplaceDirectory:
        context.path = paths.private_directory;
        context.replacement_path = scratch / (std::string(attack.label) + ".replacement");
        CHECK(::mkdir(context.replacement_path.c_str(), 0700) == 0);
        break;
    case PrivateHandoffAdoptionMutation::ReplaceLock:
        context.path = paths.lock_path;
        break;
    }
}

void check_private_handoff_adoption_attack_preserved(
    const PrivateHandoffAdoptionAttackCase& attack, const OOCCleanupPaths& paths,
    const ProtectedPrivateHandoffBytes& expected,
    const PrivateHandoffAdoptionMutationContext& context) {
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
    if (attack.mutation != PrivateHandoffAdoptionMutation::AddLegacyIntent) {
        CHECK(!entry_exists_no_follow(paths.intent_path));
    }

    if (attack.mutation == PrivateHandoffAdoptionMutation::ReplaceDirectory) {
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(context.saved_path));
        check_test_bytes_preserved(context.saved_path / paths.private_handoff_path.filename(),
                                   expected.canonical);
        check_test_bytes_preserved(context.saved_path / paths.index_path.filename(),
                                   expected.index);
        check_test_bytes_preserved(context.saved_path / paths.data_path.filename(), expected.data);
        check_test_bytes_preserved(context.saved_path /
                                       gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(
                                           paths.private_directory)
                                           .filename(),
                                   expected.owner);
        check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
        return;
    }

    if (attack.mutation == PrivateHandoffAdoptionMutation::RemoveCanonical) {
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        check_test_bytes_preserved(context.saved_path, expected.canonical);
    } else {
        check_test_bytes_preserved(paths.private_handoff_path, expected.canonical);
    }
    check_test_bytes_preserved(paths.data_path, expected.data);
    check_test_bytes_preserved(
        gnfs::relation::ooc_cleanup_detail::private_lease_owner_path(paths.private_directory),
        expected.owner);
    check_test_bytes_preserved(paths.lease_owned_path, expected.owned);
    if (attack.mutation == PrivateHandoffAdoptionMutation::RemoveIndex) {
        CHECK(!entry_exists_no_follow(paths.index_path));
        check_test_bytes_preserved(context.saved_path, expected.index);
    } else {
        check_test_bytes_preserved(paths.index_path, expected.index);
    }
    if (attack.mutation == PrivateHandoffAdoptionMutation::AddUnknown ||
        attack.mutation == PrivateHandoffAdoptionMutation::AddLegacyIntent) {
        CHECK(entry_exists_no_follow(context.path));
    }
    if (attack.mutation == PrivateHandoffAdoptionMutation::ReplaceLock) {
        CHECK(entry_exists_no_follow(paths.lock_path));
        CHECK(entry_exists_no_follow(context.saved_path));
    }
}

void test_private_handoff_adoption_rejects_namespace_drift(const std::string& executable) {
    TempDirectory temp;
    for (std::size_t index = 0; index < PRIVATE_HANDOFF_ADOPTION_ATTACKS.size(); ++index) {
        const auto& attack = PRIVATE_HANDOFF_ADOPTION_ATTACKS[index];
        const auto base = temp.path() / (std::string(attack.label) + ".gnfs-sink-lease") / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto publisher = gnfs::test::run_child_process(
            executable, {"--private-handoff-adoption-child", "publish-exit", base.string()});
        CHECK(publisher.exited);
        CHECK(!publisher.signaled);
        CHECK(publisher.exit_code == 0);
        const auto expected = capture_protected_private_handoff_bytes(paths);

        PrivateHandoffAdoptionMutationContext context;
        configure_private_handoff_adoption_attack(attack, paths, temp.path(), context);
        const auto descriptors_before = count_open_test_descriptors();
        {
            const auto rejected = OOCCleanupTransaction::adopt_private_handoff(
                base, OOCPrivateHandoffAdoptionTestHooks{
                          .stop_after = mutate_private_handoff_adoption,
                          .context = &context,
                      });
            CHECK(context.invoked);
            CHECK(context.succeeded);
            CHECK(rejected.result.status == attack.expected_status);
            CHECK(!rejected.adopted());
            CHECK(!rejected.adoption.has_value());
        }
        CHECK(count_open_test_descriptors() == descriptors_before);
        check_private_handoff_adoption_attack_preserved(attack, paths, expected, context);
    }
}

struct PrivateHandoffLockReplacementContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::CanonicalPromoted;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_private_handoff_lock(OOCPrivateHandoffFaultPoint point,
                                                void* opaque) noexcept {
    auto& context = *static_cast<PrivateHandoffLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}
#endif

struct CleanupPublishLockReplacementContext final {
    OOCCleanupPublishFaultPoint target = OOCCleanupPublishFaultPoint::IntentPendingDurable;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_cleanup_publish_lock(OOCCleanupPublishFaultPoint point,
                                                void* opaque) noexcept {
    auto& context = *static_cast<CleanupPublishLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}

struct PrivateLeaseLockReplacementContext final {
    OOCPrivateLeaseFaultPoint target = OOCPrivateLeaseFaultPoint::FinalRenameDurable;
    std::filesystem::path lock_path;
    std::filesystem::path saved_lock_path;
    bool invoked = false;
    bool replaced = false;
};

[[nodiscard]] bool replace_private_lease_lock(OOCPrivateLeaseFaultPoint point,
                                              void* opaque) noexcept {
    auto& context = *static_cast<PrivateLeaseLockReplacementContext*>(opaque);
    if (context.invoked || point != context.target) {
        return false;
    }
    context.invoked = true;
    context.replaced = replace_test_lock_leaf(context.lock_path, context.saved_lock_path);
    return false;
}

#if defined(__APPLE__)
void create_abandoned_private_handoff_pending(const std::filesystem::path& base,
                                              bool remove_reserved) {
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            auto prepared = prepare_private_handoff(base);
            PrivateHandoffStopContext stop{
                .target = OOCPrivateHandoffFaultPoint::PendingDurable,
            };
            const auto interrupted =
                publish_private_handoff(prepared, private_handoff_stop_hooks(stop));
            if (!stop.stopped || interrupted.result.status != OOCCleanupStatus::Interrupted ||
                interrupted.state != OOCPrivateHandoffState::PendingOnly) {
                ::_exit(82);
            }
            if (remove_reserved) {
                const auto paths = OOCCleanupTransaction::paths_for(base);
                std::error_code error;
                if (!std::filesystem::remove(paths.lease_reserved_path, error) || error) {
                    ::_exit(83);
                }
            }
            ::_exit(0);
        } catch (...) {
            ::_exit(84);
        }
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

void test_private_handoff_publish_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-handoff-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-handoff-lock";
    auto prepared = prepare_private_handoff(base);

    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    PrivateHandoffLockReplacementContext replacement{
        .target = OOCPrivateHandoffFaultPoint::CanonicalPromoted,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    const auto rejected =
        publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                              .stop_after = replace_private_handoff_lock,
                                              .context = &replacement,
                                          });
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(rejected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(rejected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));

    const auto handoff_bytes = read_test_bytes(paths.private_handoff_path);
    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);

    const auto retried = publish_private_handoff(prepared);
    CHECK(retried.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(retried.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    check_test_bytes_preserved(paths.private_handoff_path, handoff_bytes);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    check_test_bytes_preserved(paths.private_handoff_path, handoff_bytes);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
}
#endif

void test_private_cleanup_intent_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-intent-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-intent-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    OOCRelationWriter writer(base.string(), *reservation.ownership,
                             OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
    (void)writer.write(make_real_relation(31, 37));
    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    CleanupPublishLockReplacementContext replacement{
        .target = OOCCleanupPublishFaultPoint::IntentPendingDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    bool rejected = false;
    try {
        (void)writer.finalize_and_publish_cleanup_handoff(OOCCleanupTestHooks{
            .stop_after = nullptr,
            .stop_after_publish = replace_cleanup_publish_lock,
            .fail_before_operation = nullptr,
            .context = &replacement,
        });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(writer.has_cleanup_ownership_receipt());
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));

    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto pending_bytes = read_test_bytes(paths.intent_pending_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership->spent());
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.intent_pending_path, pending_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
}

void test_private_lease_reservation_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-reserve-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-reserve-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    PrivateLeaseLockReplacementContext replacement{
        .target = OOCPrivateLeaseFaultPoint::FinalRenameDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    const auto reservation = OOCCleanupTransaction::reserve_private_lease(
        base, OOCPrivateLeaseTestHooks{
                  .stop_after = replace_private_lease_lock,
                  .context = &replacement,
              });
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));

    const auto original_lock_bytes = read_test_bytes(saved_lock_path);
    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentConflict);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.lease_reserved_path, reserved_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    CHECK(!entry_exists_no_follow(paths.index_path));
    CHECK(!entry_exists_no_follow(paths.data_path));
}

void test_private_lease_activation_rejects_replaced_held_lock() {
    TempDirectory temp;
    const auto base = temp.path() / "private-activate-lock-replacement.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_lock_path = temp.path() / "saved-private-activate-lock";
    const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());
    const auto original_lock_bytes = read_test_bytes(paths.lock_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);
    const auto owner_bytes = read_test_bytes(owner_path);
    PrivateLeaseLockReplacementContext replacement{
        .target = OOCPrivateLeaseFaultPoint::ReservedRemovedDurable,
        .lock_path = paths.lock_path,
        .saved_lock_path = saved_lock_path,
    };

    bool rejected = false;
    try {
        OOCRelationWriter writer(base.string(), *reservation.ownership,
                                 OOCPrivateLeaseTestHooks{
                                     .stop_after = replace_private_lease_lock,
                                     .context = &replacement,
                                 });
    } catch (const std::system_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(replacement.invoked);
    CHECK(replacement.replaced);
    CHECK(!reservation.ownership->spent());
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(entry_exists_no_follow(owner_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));

    const auto replacement_lock_bytes = read_test_bytes(paths.lock_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    bool retry_rejected = false;
    try {
        OOCRelationWriter retry(base.string(), *reservation.ownership);
    } catch (const std::exception&) {
        retry_rejected = true;
    }
    CHECK(retry_rejected);
    CHECK(!reservation.ownership->spent());
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::IntentConflict);
    check_test_bytes_preserved(paths.lock_path, replacement_lock_bytes);
    check_test_bytes_preserved(saved_lock_path, original_lock_bytes);
    check_test_bytes_preserved(paths.lease_owned_path, owned_bytes);
    check_test_bytes_preserved(owner_path, owner_bytes);
    check_test_bytes_preserved(paths.index_path, index_bytes);
    check_test_bytes_preserved(paths.data_path, data_bytes);
}

#endif

#if defined(__APPLE__)
void test_private_handoff_writer_round_trip() {
    TempDirectory temp;

    for (const std::size_t relation_count : {std::size_t{0}, std::size_t{2}}) {
        const auto base =
            temp.path() /
            ("writer-private-handoff-" + std::to_string(relation_count) + ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());

        std::optional<OOCSnapshotDescriptor> descriptor;
        {
            OOCRelationWriter writer(base.string(), *reservation.ownership,
                                     OOCRelationWriter::PrivateLeaseMode::DeferCleanupHandoff);
            for (std::size_t index = 0; index < relation_count; ++index) {
                (void)writer.write(
                    make_real_relation(23 + static_cast<std::int64_t>(index), 29 + index));
            }
            descriptor = writer.finalize_and_publish_private_handoff(
                PRIVATE_HANDOFF_PAYLOAD_KIND, PRIVATE_HANDOFF_PAYLOAD_VERSION,
                PRIVATE_HANDOFF_PAYLOAD);
        }

        CHECK(descriptor.has_value());
        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(inspected.record.has_value());
        if (descriptor && inspected.record) {
            CHECK(inspected.record->pair == handoff_pair_descriptor(*descriptor));
            CHECK(inspected.record->payload_kind == PRIVATE_HANDOFF_PAYLOAD_KIND);
            CHECK(inspected.record->payload_version == PRIVATE_HANDOFF_PAYLOAD_VERSION);
            CHECK(inspected.record->opaque_payload.size() == PRIVATE_HANDOFF_PAYLOAD.size());
            CHECK(std::equal(inspected.record->opaque_payload.begin(),
                             inspected.record->opaque_payload.end(),
                             PRIVATE_HANDOFF_PAYLOAD.begin()));
        }
        CHECK(OOCRelationReader(base.string()).count() == relation_count);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));

        const auto retained = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
        CHECK(retained.status == OOCCleanupStatus::HandoffPresent);
        CHECK(!reservation.ownership->spent());
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
    }
}

void test_private_handoff_pending_only_never_authorizes_cleanup() {
    TempDirectory temp;
    const auto base = temp.path() / "pending-private-handoff.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    create_abandoned_private_handoff_pending(base, false);

    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(inspected.state == OOCPrivateHandoffState::PendingOnly);
    CHECK(!inspected.canonical());

    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::RecoveryRequired);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(entry_exists_no_follow(paths.index_path));
    CHECK(entry_exists_no_follow(paths.data_path));
    CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));

    const auto rolled_back = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(rolled_back.completed());
    check_cleanup_complete(paths);
    CHECK(!entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.private_directory));
}

void test_private_handoff_pending_without_reserved_is_preserved() {
    TempDirectory temp;
    const auto base = temp.path() / "pending-without-reserved.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    create_abandoned_private_handoff_pending(base, true);
    const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::RecoveryRequired);
    CHECK(inspected.state == OOCPrivateHandoffState::PendingOnly);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::RecoveryRequired);
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::RecoveryRequired);

    CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
}

void test_private_handoff_missing_lock_conflicts_before_mutation() {
    TempDirectory temp;
    const auto base = temp.path() / "missing-lock.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    std::error_code error;
    CHECK(std::filesystem::create_directories(paths.private_directory, error));
    CHECK(!error);
    write_private_control_bytes(paths.private_handoff_pending_path, PRIVATE_HANDOFF_PAYLOAD);
    const auto original = read_test_bytes(paths.private_handoff_pending_path);
    CHECK(!entry_exists_no_follow(paths.lock_path));

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);
    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(!reservation.ownership.has_value());

    CHECK(!entry_exists_no_follow(paths.lock_path));
    CHECK(read_test_bytes(paths.private_handoff_pending_path) == original);
}

void test_private_handoff_canonical_pending_convergence_and_taint() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "canonical-duplicate-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        write_private_control_bytes(paths.private_handoff_pending_path, canonical_bytes);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "canonical-corrupt-pending.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        constexpr std::array corrupt{
            std::byte{0xde},
            std::byte{0xad},
            std::byte{0xbe},
            std::byte{0xef},
        };
        write_private_control_bytes(paths.private_handoff_pending_path, corrupt);
        const auto pending_bytes = read_test_bytes(paths.private_handoff_pending_path);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(read_test_bytes(paths.private_handoff_pending_path) == pending_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "canonical-foreign-pending.gnfs-sink-lease" / "corpus";
        const auto foreign_base = temp.path() / "foreign-pending-source.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        const auto foreign_paths = OOCCleanupTransaction::paths_for(foreign_base);
        auto prepared = prepare_private_handoff(base);
        auto foreign_prepared = prepare_private_handoff(foreign_base);
        CHECK(publish_private_handoff(prepared).canonical());
        CHECK(publish_private_handoff(foreign_prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        const auto foreign_bytes = read_test_bytes(foreign_paths.private_handoff_path);
        write_private_control_bytes(paths.private_handoff_pending_path, foreign_bytes);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(read_test_bytes(paths.private_handoff_pending_path) == foreign_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }
}

enum class PrivateHandoffCanonicalMutation : std::uint8_t {
    Truncated,
    DigestCorrupt,
    WireVersionMismatch,
    IdentityMismatch,
};

void mutate_private_handoff_canonical(const std::filesystem::path& path,
                                      PrivateHandoffCanonicalMutation mutation) {
    auto bytes = read_test_bytes(path);
    switch (mutation) {
    case PrivateHandoffCanonicalMutation::Truncated:
        if (bytes.empty()) {
            throw std::runtime_error("cannot truncate empty private handoff");
        }
        bytes.pop_back();
        break;
    case PrivateHandoffCanonicalMutation::DigestCorrupt:
        if (bytes.empty()) {
            throw std::runtime_error("cannot corrupt empty private handoff");
        }
        bytes.back() ^= std::byte{0x80};
        break;
    case PrivateHandoffCanonicalMutation::WireVersionMismatch:
        store_u32_le(bytes, 8, gnfs::relation::OOC_PRIVATE_HANDOFF_WIRE_VERSION_V1 + 1U);
        break;
    case PrivateHandoffCanonicalMutation::IdentityMismatch: {
        const auto decoded = gnfs::relation::decode_ooc_private_handoff_record(bytes);
        if (!decoded || !decoded.value) {
            throw std::runtime_error("could not decode private handoff mutation fixture");
        }
        auto record = *decoded.value;
        record.index.identity.first ^= UINT64_C(0x8000000000000000);
        if (!gnfs::relation::seal_ooc_private_handoff_record(record)) {
            throw std::runtime_error("could not seal private handoff mutation fixture");
        }
        bytes = encode_private_handoff_record(record);
        break;
    }
    }
    write_private_control_bytes(path, bytes);
}

void test_private_handoff_canonical_corruption_is_preserved() {
    TempDirectory temp;
    constexpr std::array mutations{
        PrivateHandoffCanonicalMutation::Truncated,
        PrivateHandoffCanonicalMutation::DigestCorrupt,
        PrivateHandoffCanonicalMutation::WireVersionMismatch,
        PrivateHandoffCanonicalMutation::IdentityMismatch,
    };

    for (std::size_t index = 0; index < mutations.size(); ++index) {
        const auto base = temp.path() /
                          ("canonical-mutation-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        mutate_private_handoff_canonical(paths.private_handoff_path, mutations[index]);
        const auto mutated = read_test_bytes(paths.private_handoff_path);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(!inspected.canonical());
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(!prepared.lease_ownership.spent());

        CHECK(read_test_bytes(paths.private_handoff_path) == mutated);
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    }
}

void test_private_handoff_macos_path_policy_is_fail_closed() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "handoff-mode.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        std::error_code error;
        std::filesystem::permissions(paths.private_handoff_path,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read,
                                     std::filesystem::perm_options::replace, error);
        CHECK(!error);

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
        CHECK(entry_exists_no_follow(paths.index_path));
        CHECK(entry_exists_no_follow(paths.data_path));
    }

    {
        const auto base = temp.path() / "handoff-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(publish_private_handoff(prepared).canonical());
        const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
        const auto foreign = temp.path() / "handoff-symlink-target";
        write_private_control_bytes(foreign, canonical_bytes);
        std::error_code error;
        CHECK(std::filesystem::remove(paths.private_handoff_path, error));
        CHECK(!error);
        if (create_symlink_or_explicit_skip(foreign, paths.private_handoff_path,
                                            "private handoff canonical symlink")) {
            const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
            CHECK(inspected.result.status == OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
            CHECK(entry_is_symlink_no_follow(paths.private_handoff_path));
            CHECK(read_test_bytes(foreign) == canonical_bytes);
            CHECK(entry_exists_no_follow(paths.index_path));
            CHECK(entry_exists_no_follow(paths.data_path));
        }
    }
}

void test_private_handoff_mixed_with_legacy_authority_is_preserved() {
    TempDirectory temp;
    const auto base = temp.path() / "mixed-generic-and-legacy.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto published = publish_private_handoff(prepared);
        ::_exit(published.canonical() ? 0 : 81);
    }

    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());
    CHECK(entry_exists_no_follow(paths.private_handoff_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));

    const auto legacy = gnfs::relation::ooc_cleanup_detail::capture_source_pair(
        paths, prepared.descriptor.store_id);
    const auto intent_bytes = gnfs::relation::ooc_cleanup_detail::serialize_marker(
        legacy, gnfs::relation::ooc_cleanup_detail::INTENT_MAGIC);
    const auto staged_bytes = gnfs::relation::ooc_cleanup_detail::serialize_marker(
        legacy, gnfs::relation::ooc_cleanup_detail::STAGED_MAGIC);
    write_private_control_bytes(paths.intent_path, intent_bytes);
    write_private_control_bytes(paths.staged_path, staged_bytes);

    const auto generic_bytes = read_test_bytes(paths.private_handoff_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

    CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!prepared.lease_ownership.spent());
    std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
    stale_lease.emplace(std::move(prepared.lease_ownership));
    stale_lease.reset();

    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.result.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
    CHECK(!inspected.canonical());
    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::NamespaceConflict);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NamespaceConflict);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
          OOCCleanupStatus::NamespaceConflict);

    CHECK(read_test_bytes(paths.private_handoff_path) == generic_bytes);
    CHECK(read_test_bytes(paths.intent_path) == intent_bytes);
    CHECK(read_test_bytes(paths.staged_path) == staged_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
    CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.staged_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

void test_private_handoff_canonical_blocks_stale_pair_receipt() {
    TempDirectory temp;
    const auto base = temp.path() / "canonical-stale-pair.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    auto prepared = prepare_private_handoff(base);

    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto published = publish_private_handoff(prepared);
        ::_exit(published.canonical() ? 0 : 85);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(!prepared.lease_ownership.spent());

    std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
    stale_lease.emplace(std::move(prepared.lease_ownership));
    stale_lease.reset();

    const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
    const auto index_bytes = read_test_bytes(paths.index_path);
    const auto data_bytes = read_test_bytes(paths.data_path);
    const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
    CHECK(inspected.canonical());
    CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
    CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
          OOCCleanupStatus::HandoffPresent);
    CHECK(!prepared.pair_ownership.spent());
    CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::HandoffPresent);
    const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.result.status == OOCCleanupStatus::HandoffPresent);
    CHECK(!reservation.ownership.has_value());

    CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
    CHECK(read_test_bytes(paths.index_path) == index_bytes);
    CHECK(read_test_bytes(paths.data_path) == data_bytes);
    CHECK(!entry_exists_no_follow(paths.intent_path));
    CHECK(!entry_exists_no_follow(paths.intent_pending_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
    CHECK(!entry_exists_no_follow(paths.quarantine_data_path));
}

constexpr int PRIVATE_HANDOFF_CRASH_EXIT_BASE = 180;

struct PrivateHandoffCrashContext final {
    OOCPrivateHandoffFaultPoint target = OOCPrivateHandoffFaultPoint::PendingDurable;
};

[[nodiscard]] bool crash_at_private_handoff(OOCPrivateHandoffFaultPoint point,
                                            void* opaque) noexcept {
    const auto& context = *static_cast<PrivateHandoffCrashContext*>(opaque);
    if (point == context.target) {
        ::_exit(PRIVATE_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(point));
    }
    return false;
}

void test_private_handoff_process_crash_and_cow_retry() {
    TempDirectory temp;

    for (std::size_t index = 0; index < PRIVATE_HANDOFF_FAULT_POINTS.size(); ++index) {
        const auto point = PRIVATE_HANDOFF_FAULT_POINTS[index];
        const auto base = temp.path() /
                          ("private-handoff-crash-" + std::to_string(index) + ".gnfs-sink-lease") /
                          "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);

        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            PrivateHandoffCrashContext context{.target = point};
            (void)publish_private_handoff(prepared, OOCPrivateHandoffTestHooks{
                                                        .stop_after = crash_at_private_handoff,
                                                        .context = &context,
                                                    });
            ::_exit(79);
        }

        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == PRIVATE_HANDOFF_CRASH_EXIT_BASE + static_cast<int>(point));
        CHECK(!prepared.pair_ownership.spent());
        CHECK(!prepared.lease_ownership.spent());
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));

        if (point == OOCPrivateHandoffFaultPoint::PendingDurable) {
            CHECK(!entry_exists_no_follow(paths.private_handoff_path));
            CHECK(entry_exists_no_follow(paths.private_handoff_pending_path));
            CHECK(entry_exists_no_follow(paths.lease_reserved_path));
        } else {
            CHECK(entry_exists_no_follow(paths.private_handoff_path));
            CHECK(entry_exists_no_follow(paths.lease_reserved_path) ==
                  (point != OOCPrivateHandoffFaultPoint::ReservedRevokedDurable));

            const auto canonical_bytes = read_test_bytes(paths.private_handoff_path);
            bool stale_writer_rejected = false;
            try {
                OOCRelationWriter stale_writer(base.string(), prepared.lease_ownership);
            } catch (const std::system_error&) {
                stale_writer_rejected = true;
            }
            CHECK(stale_writer_rejected);
            CHECK(read_test_bytes(paths.private_handoff_path) == canonical_bytes);
            CHECK(read_test_bytes(paths.index_path) == index_bytes);
            CHECK(read_test_bytes(paths.data_path) == data_bytes);

            CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
                  OOCCleanupStatus::HandoffPresent);
            CHECK(!prepared.lease_ownership.spent());
            CHECK(read_test_bytes(paths.index_path) == index_bytes);
            CHECK(read_test_bytes(paths.data_path) == data_bytes);
            CHECK(entry_exists_no_follow(paths.private_handoff_path));
            CHECK(!entry_exists_no_follow(paths.intent_path));
            CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        }

        const auto retried = publish_private_handoff(prepared);
        CHECK(retried.canonical());
        CHECK(retried.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(prepared.pair_ownership.spent());
        CHECK(!prepared.lease_ownership.spent());
        CHECK(entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.canonical());
        CHECK(inspected.result.status == OOCCleanupStatus::HandoffPresent);
        CHECK(inspected.state == OOCPrivateHandoffState::Canonical);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::HandoffPresent);
    }
}
#endif

#if defined(__linux__)
void test_private_handoff_linux_platform_policy() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "linux-legacy-no-generic.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        CHECK(!entry_exists_no_follow(paths.private_handoff_path));
        CHECK(!entry_exists_no_follow(paths.private_handoff_pending_path));
        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).completed());
        CHECK(prepared.lease_ownership.spent());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }

    for (const bool canonical : {false, true}) {
        const auto base =
            temp.path() /
            (std::string(canonical ? "linux-generic-canonical" : "linux-generic-pending") +
             ".gnfs-sink-lease") /
            "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        auto prepared = prepare_private_handoff(base);
        const auto generic_path =
            canonical ? paths.private_handoff_path : paths.private_handoff_pending_path;
        write_private_control_bytes(generic_path, PRIVATE_HANDOFF_PAYLOAD);

        const auto generic_bytes = read_test_bytes(generic_path);
        const auto index_bytes = read_test_bytes(paths.index_path);
        const auto data_bytes = read_test_bytes(paths.data_path);
        const auto reserved_bytes = read_test_bytes(paths.lease_reserved_path);
        const auto owned_bytes = read_test_bytes(paths.lease_owned_path);

        CHECK(OOCCleanupTransaction::remove_private_lease(prepared.lease_ownership).status ==
              OOCCleanupStatus::PlatformUnsupported);
        CHECK(!prepared.lease_ownership.spent());
        std::optional<OOCPrivateLeaseOwnershipReceipt> stale_lease;
        stale_lease.emplace(std::move(prepared.lease_ownership));
        stale_lease.reset();

        const auto inspected = OOCCleanupTransaction::inspect_private_handoff(base);
        CHECK(inspected.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(inspected.state == OOCPrivateHandoffState::TaintedPreserved);
        CHECK(OOCCleanupTransaction::begin_or_resume(prepared.pair_ownership).status ==
              OOCCleanupStatus::PlatformUnsupported);
        CHECK(!prepared.pair_ownership.spent());
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::PlatformUnsupported);
        const auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.result.status == OOCCleanupStatus::PlatformUnsupported);
        CHECK(!reservation.ownership.has_value());

        CHECK(read_test_bytes(generic_path) == generic_bytes);
        CHECK(read_test_bytes(paths.index_path) == index_bytes);
        CHECK(read_test_bytes(paths.data_path) == data_bytes);
        CHECK(read_test_bytes(paths.lease_reserved_path) == reserved_bytes);
        CHECK(read_test_bytes(paths.lease_owned_path) == owned_bytes);
        CHECK(!entry_exists_no_follow(paths.intent_path));
        CHECK(!entry_exists_no_follow(paths.intent_pending_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_index_path));
        CHECK(!entry_exists_no_follow(paths.quarantine_data_path));

        std::error_code error;
        CHECK(std::filesystem::remove(generic_path, error));
        CHECK(!error);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
    }
}
#endif

#ifndef _WIN32
void test_fork_copy_cannot_remove_or_unlock_parent_lease() {
    TempDirectory temp;
    const auto base = temp.path() / "fork-lock.gnfs-sink-lease" / "corpus";
    auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
    CHECK(reservation.completed());

    int ready_pipe[2]{-1, -1};
    int release_pipe[2]{-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    CHECK(::pipe(release_pipe) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        (void)::close(release_pipe[1]);
        const auto forbidden = OOCCleanupTransaction::remove_private_lease(*reservation.ownership);
        const char result = forbidden.status == OOCCleanupStatus::InvalidRequest ? '1' : '0';
        (void)::write(ready_pipe[1], &result, 1);
        char release = 0;
        (void)::read(release_pipe[0], &release, 1);
        ::_exit(result == '1' && release == 'x' ? 0 : 72);
    }

    (void)::close(ready_pipe[1]);
    (void)::close(release_pipe[0]);
    char child_result = 0;
    CHECK(::read(ready_pipe[0], &child_result, 1) == 1);
    CHECK(child_result == '1');

    // Dropping the parent's COW receipt closes only its descriptor. The child
    // still holds the inherited open-file description, so path recovery must
    // remain Busy until that child exits.
    reservation.ownership.reset();
    CHECK(OOCCleanupTransaction::recover_private_lease(base).status == OOCCleanupStatus::Busy);

    const char release = 'x';
    CHECK(::write(release_pipe[1], &release, 1) == 1);
    (void)::close(ready_pipe[0]);
    (void)::close(release_pipe[1]);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(OOCCleanupTransaction::recover_private_lease(base).completed());
}
#endif

void test_private_lease_recovery_preserves_unknown_owner_leaf() {
    TempDirectory temp;
    const auto base = temp.path() / "unknown-owner-leaf.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    {
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
    }
    const auto foreign = paths.private_directory / "foreign-control-leaf";
    write_test_leaf(foreign, "foreign");

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(foreign));
    CHECK(entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
}

void test_private_lease_marker_attacks_fail_closed() {
    TempDirectory temp;

    {
        const auto base = temp.path() / "owned-corrupt.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        flip_last_byte(paths.lease_owned_path);
        CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
              OOCCleanupStatus::IntentCorrupt);
        CHECK(entry_exists_no_follow(paths.private_directory));
        CHECK(entry_exists_no_follow(paths.lease_owned_path));
    }

    {
        const auto base = temp.path() / "owner-hardlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        const auto owner_path = paths.private_directory / ".gnfs-private-lease-v1.owner";
        const auto alias = temp.path() / "owner-hardlink-alias";
        if (create_hard_link_checked(owner_path, alias)) {
            const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
            CHECK(recovered.status == OOCCleanupStatus::ForeignReplacementPreserved);
            CHECK(entry_exists_no_follow(paths.private_directory));
            CHECK(entry_exists_no_follow(owner_path));
            CHECK(entry_exists_no_follow(alias));
            check_entries_equivalent(owner_path, alias);
        }
    }

    {
        const auto base = temp.path() / "owned-symlink.gnfs-sink-lease" / "corpus";
        const auto paths = OOCCleanupTransaction::paths_for(base);
        {
            auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
            CHECK(reservation.completed());
        }
        const auto saved = temp.path() / "saved-owned-marker";
        std::error_code error;
        std::filesystem::rename(paths.lease_owned_path, saved, error);
        CHECK(!error);
        if (create_symlink_or_explicit_skip(saved, paths.lease_owned_path,
                                            "private lease owned marker symlink")) {
            CHECK(OOCCleanupTransaction::recover_private_lease(base).status ==
                  OOCCleanupStatus::IntentCorrupt);
            CHECK(entry_exists_no_follow(paths.private_directory));
            CHECK(entry_is_symlink_no_follow(paths.lease_owned_path));
            CHECK(entry_exists_no_follow(saved));
        }
    }
}

void test_private_lease_recovery_rejects_directory_aba() {
    TempDirectory temp;
    const auto base = temp.path() / "directory-aba.gnfs-sink-lease" / "corpus";
    const auto paths = OOCCleanupTransaction::paths_for(base);
    const auto saved_owned = temp.path() / "saved-owned-directory";
    const auto replacement = temp.path() / "replacement-directory";
    const auto sentinel = replacement / "sentinel";

    {
        auto reservation = OOCCleanupTransaction::reserve_private_lease(base);
        CHECK(reservation.completed());
    }
    CHECK(std::filesystem::create_directory(replacement));
    write_test_leaf(sentinel, "foreign replacement");

    std::error_code error;
    std::filesystem::rename(paths.private_directory, saved_owned, error);
    CHECK(!error);
    error.clear();
    std::filesystem::rename(replacement, paths.private_directory, error);
    CHECK(!error);

    const auto recovered = OOCCleanupTransaction::recover_private_lease(base);
    CHECK(recovered.status == OOCCleanupStatus::NamespaceConflict);
    CHECK(entry_exists_no_follow(paths.private_directory));
    CHECK(entry_exists_no_follow(paths.private_directory / "sentinel"));
    CHECK(entry_exists_no_follow(saved_owned));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
}

void test_private_lease_recovery_rejects_marker_replacement() {
    TempDirectory temp;
    const auto left_root = temp.path() / "left";
    const auto right_root = temp.path() / "right";
    CHECK(std::filesystem::create_directory(left_root));
    CHECK(std::filesystem::create_directory(right_root));
    const auto left_base = left_root / "shared.gnfs-sink-lease" / "corpus";
    const auto right_base = right_root / "shared.gnfs-sink-lease" / "corpus";
    const auto left_paths = OOCCleanupTransaction::paths_for(left_base);
    const auto right_paths = OOCCleanupTransaction::paths_for(right_base);

    {
        auto left = OOCCleanupTransaction::reserve_private_lease(left_base);
        auto right = OOCCleanupTransaction::reserve_private_lease(right_base);
        CHECK(left.completed());
        CHECK(right.completed());
    }
    CHECK(entry_exists_no_follow(left_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(right_paths.lease_owned_path));

    const auto saved_marker = temp.path() / "saved-left-owned-marker";
    std::error_code error;
    std::filesystem::rename(left_paths.lease_owned_path, saved_marker, error);
    CHECK(!error);
    error.clear();
    std::filesystem::copy_file(right_paths.lease_owned_path, left_paths.lease_owned_path,
                               std::filesystem::copy_options::none, error);
    CHECK(!error);

    const auto recovered = OOCCleanupTransaction::recover_private_lease(left_base);
    CHECK(recovered.status == OOCCleanupStatus::IntentConflict);
    CHECK(entry_exists_no_follow(left_paths.private_directory));
    CHECK(entry_exists_no_follow(left_paths.lease_owned_path));
    CHECK(entry_exists_no_follow(saved_marker));
    CHECK(entry_exists_no_follow(right_paths.private_directory));
}

struct CrashContext final {
    OOCCleanupFaultPoint target = OOCCleanupFaultPoint::IntentDurable;
};

[[nodiscard]] bool crash_at(OOCCleanupFaultPoint point, void* opaque) noexcept {
    const auto& context = *static_cast<const CrashContext*>(opaque);
    if (point == context.target) {
        std::_Exit(100 + static_cast<int>(point));
    }
    return false;
}

[[nodiscard]] PairShape finalized_crash_shape() {
    return PairShape{
        .magic = OOCRelationStoreFormat::MAGIC_V3_FINAL,
        .count = 2,
        .index_size = OOCRelationStoreFormat::INDEX_HEADER_BYTES + 3 * sizeof(std::uint64_t),
        .data_size = OOCRelationStoreFormat::DATA_HEADER_BYTES + 48,
    };
}

int run_crash_child(std::size_t point_index, const std::filesystem::path& base,
                    std::uint64_t store_id) {
    if (point_index >= CLEANUP_FAULT_POINTS.size() || store_id == 0) {
        return 64;
    }
    const PairShape shape = finalized_crash_shape();
    write_pair(base, store_id, shape);
    const OOCCleanupRequest request{
        .base_path = base,
        .store_id = store_id,
        .exact = exact_for(shape),
    };
    CrashContext context{.target = CLEANUP_FAULT_POINTS[point_index]};
    const auto result = begin_cleanup(request, OOCCleanupTestHooks{
                                                   .stop_after = crash_at,
                                                   .stop_after_publish = nullptr,
                                                   .fail_before_operation = nullptr,
                                                   .context = &context,
                                               });
    (void)result;
    return 65;
}

void test_process_crash_recovery(const std::string& executable) {
    TempDirectory temp;
    constexpr std::uint64_t initial_store_id = 0x8888'9999'aaaa'bbb0ULL;

    for (std::size_t index = 0; index < CLEANUP_FAULT_POINTS.size(); ++index) {
        const auto base = temp.path() / ("process-crash-" + std::to_string(index));
        const std::uint64_t store_id = initial_store_id + index;
        const auto child =
            gnfs::test::run_child_process(executable, {"--crash-cleanup", std::to_string(index),
                                                       base.string(), std::to_string(store_id)});
        CHECK(child.exited);
        CHECK(!child.signaled);
        CHECK(child.exit_code == 100 + static_cast<int>(CLEANUP_FAULT_POINTS[index]));

        const auto paths = OOCCleanupTransaction::paths_for(base);
        check_fault_namespace(paths, CLEANUP_FAULT_POINTS[index]);
        CHECK(OOCCleanupTransaction::resume(base).completed());
        check_cleanup_complete(paths);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);

        const std::uint64_t replacement_store_id = store_id + 0x1000;
        write_pair(base, replacement_store_id);
        CHECK(OOCCleanupTransaction::resume(base).status == OOCCleanupStatus::NoTransaction);
        CHECK(exists(paths.index_path));
        CHECK(exists(paths.data_path));
        CHECK(begin_cleanup(base, replacement_store_id).completed());
    }
}

void run_core_suite(const std::string& executable) {
    test_fault_point_recovery();
    test_receipt_authority_and_pending_publication();
    test_namespace_operation_failures_are_retryable();
    test_reserved_cleanup_suffix_is_rejected();
    test_fresh_writer_rejects_nonempty_cleanup_namespace();
    test_windows_sharing_violation_is_retryable();
    test_exact_finalized_expectation();
    test_real_finalized_store_cleanup();
    test_marker_corruption_is_fail_closed();
    test_authorized_v2_markers_are_not_legacy_cleanup_authority();
    test_absence_before_staged_has_no_delete_authority();
    test_reverse_pre_staged_state_is_rejected();
    test_source_link_attacks_are_fail_closed();
    test_quarantine_link_attacks_are_fail_closed();
    test_intent_link_attacks_are_fail_closed();
    test_lock_link_attacks_are_fail_closed();
    test_foreign_replacements_are_preserved();
    test_index_count_drift_is_preserved();
    test_staged_only_tail_has_no_delete_authority();
    test_quarantine_collision_is_preserved();
    test_cross_process_lock_reports_busy(executable);
    test_private_lease_uses_one_persistent_external_lock(executable);
    test_private_lease_receipt_rejects_replacement_directory();
#if !defined(__APPLE__)
    test_private_handoff_unsupported_adoption_is_non_observing();
#endif
}

void run_private_lease_crash_suite(const std::string& executable) {
    test_private_lease_process_crash_recovery(executable);
    test_private_lease_preactive_rollback_crash_recovery(executable);
    test_private_writer_preactivation_crash_recovery(executable);
    test_private_lease_preactive_link_attacks_are_preserved();
    test_private_lease_recovery_preserves_live_pair_without_intent(executable);
    test_private_lease_recovery_preserves_pending_only_pair(executable);
    test_private_lease_writer_activation_closes_reservation();
    test_private_lease_recovery_finishes_canonical_pair_intent(executable);
    test_deferred_private_writer_handoff_and_pending_recovery();
    test_deferred_handoff_foreign_leaf_blocks_pair_mutation();
    test_private_handoff_writer_rejects_metadata_before_finalize();
    test_private_handoff_transaction_rejects_oversize_before_mutation();
    test_private_handoff_missing_lock_orphan_stage_is_preserved();
    test_private_handoff_invalid_orphan_stage_names_are_ignored();
    test_private_lease_unknown_child_preserves_matching_pending();
    test_private_lease_unknown_scan_precedes_writer_mutation();
    test_unscoped_writer_rejects_existing_preactive_private_lease();
    test_private_lease_unknown_scan_precedes_legacy_intent_publication();
    test_private_lease_activation_interrupt_after_commit_preserves_pair();
    test_private_lease_activation_post_sync_failure_preserves_pair();
#if !defined(__APPLE__)
    test_private_handoff_unsupported_publish_is_non_mutating();
#endif
#if !defined(_WIN32)
    test_private_cleanup_intent_rejects_replaced_held_lock();
    test_private_lease_reservation_rejects_replaced_held_lock();
    test_private_lease_activation_rejects_replaced_held_lock();
#endif
#if defined(__APPLE__)
    test_private_handoff_cross_process_adoption(executable);
    test_private_handoff_adopter_owner_death(executable);
    test_private_handoff_zero_row_adoption(executable);
    test_private_handoff_adoption_publication_prefixes(executable);
    test_private_handoff_adoption_interruptions(executable);
    test_private_handoff_adoption_rejects_namespace_drift(executable);
    test_private_handoff_publish_rejects_replaced_held_lock();
    test_private_handoff_writer_round_trip();
    test_private_handoff_pending_only_never_authorizes_cleanup();
    test_private_handoff_pending_without_reserved_is_preserved();
    test_private_handoff_missing_lock_conflicts_before_mutation();
    test_private_handoff_canonical_pending_convergence_and_taint();
    test_private_handoff_canonical_corruption_is_preserved();
    test_private_handoff_macos_path_policy_is_fail_closed();
    test_private_handoff_mixed_with_legacy_authority_is_preserved();
    test_private_handoff_canonical_blocks_stale_pair_receipt();
    test_private_handoff_process_crash_and_cow_retry();
#endif
#if defined(__linux__)
    test_private_handoff_linux_platform_policy();
#endif
#ifndef _WIN32
    test_fork_copy_cannot_remove_or_unlock_parent_lease();
#endif
    test_private_lease_recovery_preserves_unknown_owner_leaf();
    test_private_lease_marker_attacks_fail_closed();
    test_private_lease_recovery_rejects_directory_aba();
    test_private_lease_recovery_rejects_marker_replacement();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 4 && std::string_view(argv[1]) == "--private-handoff-adoption-child") {
        return run_private_handoff_adoption_child(std::string_view(argv[2]),
                                                  std::filesystem::path(argv[3]));
    }
    if (argc == 4 && std::string_view(argv[1]) == "--contend-cleanup-lock") {
        try {
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[3]));
            return run_lock_contender_child(std::filesystem::path(argv[2]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--hold-cleanup-lock") {
        try {
            const auto executable =
                std::filesystem::absolute(std::filesystem::path(argv[0])).string();
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[3]));
            return run_lock_holder_child(executable, std::filesystem::path(argv[2]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--crash-cleanup") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[2]));
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[4]));
            return run_crash_child(point_index, std::filesystem::path(argv[3]), store_id);
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--crash-private-lease") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[3]));
            return run_private_lease_crash_child(std::string_view(argv[2]), point_index,
                                                 std::filesystem::path(argv[4]));
        } catch (...) {
            return 64;
        }
    }
    if (argc == 4 && std::string_view(argv[1]) == "--crash-private-writer") {
        try {
            const auto point_index = static_cast<std::size_t>(std::stoull(argv[2]));
            return run_private_writer_crash_child(point_index, std::filesystem::path(argv[3]));
        } catch (...) {
            return 64;
        }
    }
    if (argc == 5 && std::string_view(argv[1]) == "--abandon-private-lease") {
        try {
            const auto store_id = static_cast<std::uint64_t>(std::stoull(argv[4]));
            return run_private_lease_abandon_child(std::string_view(argv[2]),
                                                   std::filesystem::path(argv[3]), store_id);
        } catch (...) {
            return 64;
        }
    }

    bool run_core = argc == 1;
    bool run_crash = argc == 1;
    bool run_private_lease_crash = argc == 1;
    if (argc == 3 && std::string_view(argv[1]) == "--suite") {
        const std::string_view suite(argv[2]);
        run_core = suite == "core";
        run_crash = suite == "crash";
        run_private_lease_crash = suite == "lease-crash";
        if (!run_core && !run_crash && !run_private_lease_crash) {
            std::cerr << "unknown suite: " << suite << '\n';
            return 64;
        }
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--suite core|crash|lease-crash]\n";
        return 64;
    }

    try {
        const auto executable = std::filesystem::absolute(std::filesystem::path(argv[0])).string();
        if (run_core) {
            run_core_suite(executable);
        }
        if (run_crash) {
            test_process_crash_recovery(executable);
        }
        if (run_private_lease_crash) {
            run_private_lease_crash_suite(executable);
        }
    } catch (const std::exception& error) {
        ++checks_failed;
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
    }

    std::cout << "OOC cleanup transaction checks passed: " << checks_passed << '\n';
    if (checks_failed != 0) {
        std::cerr << "OOC cleanup transaction checks failed: " << checks_failed << '\n';
        return 1;
    }
    return 0;
}
