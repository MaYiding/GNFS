#include <gnfs/core/relation.hpp>
#include <gnfs/relation/ooc_cleanup_transaction.hpp>
#include <gnfs/relation/ooc_relation_format.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>

#include "support/child_process.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
using gnfs::relation::OOCCleanupFaultPoint;
using gnfs::relation::OOCCleanupOwnershipReceipt;
using gnfs::relation::OOCCleanupPublishFaultPoint;
using gnfs::relation::OOCCleanupRequest;
using gnfs::relation::OOCCleanupStage;
using gnfs::relation::OOCCleanupStatus;
using gnfs::relation::OOCCleanupTestHooks;
using gnfs::relation::OOCCleanupTestOperation;
using gnfs::relation::OOCCleanupTransaction;
using gnfs::relation::OOCExactCleanupExpectation;
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

    write_pair(base, store_id, *reservation.ownership);
    CHECK(!entry_exists_no_follow(paths.lease_reserved_pending_path));
    CHECK(!entry_exists_no_follow(paths.lease_reserved_path));
    CHECK(entry_exists_no_follow(paths.lease_owned_path));
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
        CHECK(OOCRelationReader(base.string()).count() == 0);
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
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
        CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
        check_cleanup_complete(paths);
        CHECK(!entry_exists_no_follow(paths.private_directory));
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

    std::error_code error;
    CHECK(std::filesystem::remove(foreign, error));
    CHECK(!error);
    CHECK(OOCCleanupTransaction::remove_private_lease(*reservation.ownership).completed());
    check_cleanup_complete(paths);
}

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
