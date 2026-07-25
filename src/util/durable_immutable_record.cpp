#include "gnfs/util/durable_immutable_record.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
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
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#include <sys/acl.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace gnfs::util::durable_immutable_record {

namespace detail {

class RecordPublishResultFactory final {
public:
    [[nodiscard]] static RecordPublishResult durable(RecordPublishDisposition disposition,
                                                     RecordSnapshot canonical_snapshot) noexcept {
        if (disposition == RecordPublishDisposition::none) {
            return contract_violation();
        }
        return RecordPublishResult(RecordPublishStatus::durable, disposition, canonical_snapshot,
                                   {});
    }

    [[nodiscard]] static RecordPublishResult
    interrupted(RecordPublishDisposition disposition,
                std::optional<RecordSnapshot> canonical_snapshot) noexcept {
        if (disposition == RecordPublishDisposition::none) {
            return contract_violation();
        }
        return RecordPublishResult(RecordPublishStatus::interrupted, disposition,
                                   std::move(canonical_snapshot),
                                   std::make_error_code(std::errc::operation_canceled));
    }

    [[nodiscard]] static RecordPublishResult failure(RecordPublishStatus status,
                                                     RecordPublishDisposition disposition,
                                                     std::error_code native_error) noexcept {
        if (status == RecordPublishStatus::durable || status == RecordPublishStatus::interrupted) {
            return contract_violation();
        }
        if (!native_error) {
            native_error = std::make_error_code(std::errc::protocol_error);
        }
        return RecordPublishResult(status, disposition, std::nullopt, native_error);
    }

private:
    [[nodiscard]] static RecordPublishResult contract_violation() noexcept {
        return RecordPublishResult(RecordPublishStatus::ops_contract_violation,
                                   RecordPublishDisposition::none, std::nullopt,
                                   std::make_error_code(std::errc::protocol_error));
    }
};

} // namespace detail

namespace {

[[nodiscard]] std::error_code invalid_argument_error() noexcept {
    return std::make_error_code(std::errc::invalid_argument);
}

[[nodiscard]] std::error_code protocol_error() noexcept {
    return std::make_error_code(std::errc::protocol_error);
}

[[maybe_unused, nodiscard]] std::error_code unsupported_error() noexcept {
    return std::make_error_code(std::errc::operation_not_supported);
}

[[nodiscard]] std::error_code error_or_protocol(const std::error_code& error) noexcept {
    return error ? error : protocol_error();
}

[[nodiscard]] bool unsupported_native_error(const std::error_code& error) noexcept {
    return error == std::errc::operation_not_supported ||
           error == std::errc::function_not_supported || error == std::errc::not_supported;
}

[[nodiscard]] bool contains_nul(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return std::find(native.begin(), native.end(), std::filesystem::path::value_type{}) !=
           native.end();
}

[[nodiscard]] bool invalid_relative_leaf(const std::filesystem::path& leaf) {
    if (leaf.empty() || leaf == "." || leaf == ".." || leaf.is_absolute() ||
        leaf.has_parent_path() || leaf.filename() != leaf || contains_nul(leaf)) {
        return true;
    }
    for (const auto value : leaf.native()) {
        using UnsignedValue = std::make_unsigned_t<std::filesystem::path::value_type>;
        const auto code = static_cast<std::uint64_t>(static_cast<UnsignedValue>(value));
        if (code == 0 || code > 0x7FU) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool ascii_casefold_equal(const std::filesystem::path& left,
                                        const std::filesystem::path& right) noexcept {
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    if (left_native.size() != right_native.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left_native.size(); ++index) {
        auto left_value = left_native[index];
        auto right_value = right_native[index];
        const auto fold = [](std::filesystem::path::value_type value) noexcept {
            const auto upper_a = static_cast<std::filesystem::path::value_type>('A');
            const auto upper_z = static_cast<std::filesystem::path::value_type>('Z');
            if (value >= upper_a && value <= upper_z) {
                return static_cast<std::filesystem::path::value_type>(
                    value + static_cast<std::filesystem::path::value_type>('a' - 'A'));
            }
            return value;
        };
        left_value = fold(left_value);
        right_value = fold(right_value);
        if (left_value != right_value) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<BoundedReadResult>
validate_bounded_read_request(NativeHandle parent_handle, const std::filesystem::path& leaf,
                              std::uint64_t min_bytes, std::uint64_t max_bytes) noexcept {
    try {
        if (parent_handle == INVALID_NATIVE_HANDLE || invalid_relative_leaf(leaf) ||
            min_bytes > max_bytes) {
            return BoundedReadResult::rejected(invalid_argument_error());
        }
#ifndef _WIN32
        if (parent_handle < 0 || static_cast<std::uintmax_t>(parent_handle) >
                                     static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
            return BoundedReadResult::rejected(invalid_argument_error());
        }
#endif
        if (max_bytes > MAX_BOUNDED_READ_BYTES) {
            return BoundedReadResult::rejected(std::make_error_code(std::errc::value_too_large));
        }
    } catch (const std::bad_alloc&) {
        return BoundedReadResult::failed(std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::filesystem::filesystem_error& error) {
        return BoundedReadResult::rejected(error.code());
    } catch (...) {
        return BoundedReadResult::failed(std::make_error_code(std::errc::io_error));
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<RecordPublishResult>
validate_request(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                 const std::filesystem::path& canonical_leaf,
                 std::span<const std::byte> expected_bytes) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (expected_bytes.size() > std::numeric_limits<std::uint64_t>::max()) {
            return detail::RecordPublishResultFactory::failure(
                RecordPublishStatus::input_too_large, RecordPublishDisposition::none,
                std::make_error_code(std::errc::value_too_large));
        }
    }
#ifndef _WIN32
    if (static_cast<std::uintmax_t>(expected_bytes.size()) >
        static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
        return detail::RecordPublishResultFactory::failure(
            RecordPublishStatus::input_too_large, RecordPublishDisposition::none,
            std::make_error_code(std::errc::value_too_large));
    }
#endif

    try {
        if (parent_handle == INVALID_NATIVE_HANDLE || invalid_relative_leaf(pending_leaf) ||
            invalid_relative_leaf(canonical_leaf) ||
            ascii_casefold_equal(pending_leaf, canonical_leaf)) {
            return detail::RecordPublishResultFactory::failure(RecordPublishStatus::invalid_request,
                                                               RecordPublishDisposition::none,
                                                               invalid_argument_error());
        }
    } catch (const std::bad_alloc&) {
        return detail::RecordPublishResultFactory::failure(
            RecordPublishStatus::unexpected_failure, RecordPublishDisposition::none,
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::filesystem::filesystem_error& error) {
        return detail::RecordPublishResultFactory::failure(
            RecordPublishStatus::invalid_request, RecordPublishDisposition::none, error.code());
    } catch (...) {
        return detail::RecordPublishResultFactory::failure(
            RecordPublishStatus::unexpected_failure, RecordPublishDisposition::none,
            std::make_error_code(std::errc::io_error));
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_bounded_read_result(const BoundedReadResult& result,
                                             std::uint64_t min_bytes,
                                             std::uint64_t max_bytes) noexcept {
    switch (result.state()) {
    case BoundedReadState::missing:
        return !result.bytes().has_value() && !result.snapshot().has_value() &&
               !result.native_error();
    case BoundedReadState::exact:
        if (!result.bytes().has_value() || !result.snapshot().has_value() ||
            result.native_error()) {
            return false;
        }
        if (result.bytes()->size() != result.snapshot()->size) {
            return false;
        }
        return result.snapshot()->size >= min_bytes && result.snapshot()->size <= max_bytes;
    case BoundedReadState::rejected:
    case BoundedReadState::interrupted:
    case BoundedReadState::unsupported:
    case BoundedReadState::failed:
        return !result.bytes().has_value() && !result.snapshot().has_value() &&
               static_cast<bool>(result.native_error());
    }
    return false;
}

[[nodiscard]] RecordPublishResult
make_failure(RecordPublishStatus status, const std::error_code& error,
             RecordPublishDisposition disposition = RecordPublishDisposition::none) noexcept {
    return detail::RecordPublishResultFactory::failure(status, disposition,
                                                       error_or_protocol(error));
}

[[nodiscard]] RecordPublishResult
make_interrupted(RecordPublishDisposition disposition,
                 std::optional<RecordSnapshot> canonical_snapshot = std::nullopt) noexcept {
    return detail::RecordPublishResultFactory::interrupted(disposition,
                                                           std::move(canonical_snapshot));
}

[[nodiscard]] bool should_interrupt(const RecordTestHooks& hooks, RecordFaultPoint point) noexcept {
    return hooks.stop_after != nullptr && hooks.stop_after(point, hooks.context);
}

[[nodiscard]] bool valid_inspect_result(const InspectResult& result) noexcept {
    switch (result.state()) {
    case InspectState::missing:
        return !result.snapshot().has_value() && !result.native_error();
    case InspectState::exact:
        return result.snapshot().has_value() && !result.native_error();
    case InspectState::rejected:
    case InspectState::interrupted:
    case InspectState::unsupported:
    case InspectState::failed:
        return !result.snapshot().has_value() && static_cast<bool>(result.native_error());
    }
    return false;
}

[[nodiscard]] bool valid_mutation_result(const MutationResult& result) noexcept {
    if (result.state() == MutationState::succeeded) {
        return !result.native_error();
    }
    return static_cast<bool>(result.native_error());
}

[[nodiscard]] InspectResult inspect_retry(RecordOps& ops, NativeHandle parent_handle,
                                          const std::filesystem::path& leaf,
                                          std::span<const std::byte> expected_bytes) noexcept {
    for (;;) {
        InspectResult result = ops.inspect_exact_at(parent_handle, leaf, expected_bytes);
        if (result.state() != InspectState::interrupted) {
            return result;
        }
    }
}

[[nodiscard]] durable_immutable_file::OperationResult
sync_parent_retry(RecordOps& ops, NativeHandle parent_handle) noexcept {
    for (;;) {
        auto result = ops.sync_parent_directory(parent_handle);
        if (result.state() != durable_immutable_file::OperationState::interrupted) {
            return result;
        }
    }
}

[[nodiscard]] RecordPublishResult
inspect_failure(const InspectResult& inspection, RecordPublishStatus rejected_status,
                RecordPublishStatus failed_status,
                RecordPublishDisposition disposition = RecordPublishDisposition::none) noexcept {
    if (!valid_inspect_result(inspection)) {
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    }
    switch (inspection.state()) {
    case InspectState::missing:
    case InspectState::rejected:
        return make_failure(rejected_status, inspection.native_error(), disposition);
    case InspectState::unsupported:
        return make_failure(RecordPublishStatus::platform_unsupported, inspection.native_error(),
                            disposition);
    case InspectState::failed:
        if (unsupported_native_error(inspection.native_error())) {
            return make_failure(RecordPublishStatus::platform_unsupported,
                                inspection.native_error(), disposition);
        }
        return make_failure(failed_status, inspection.native_error(), disposition);
    case InspectState::interrupted:
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    case InspectState::exact:
        break;
    }
    return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(), disposition);
}

[[nodiscard]] RecordPublishResult confirm_canonical_and_cleanup_pending(
    NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
    const std::filesystem::path& canonical_leaf, std::span<const std::byte> expected_bytes,
    RecordOps& ops, const RecordTestHooks& hooks, RecordPublishDisposition disposition,
    const RecordSnapshot& expected_canonical,
    const std::optional<RecordSnapshot>& pending_before_confirmation) noexcept {
    const auto confirmed = ops.confirm_durable_at(parent_handle, canonical_leaf);
    if (!confirmed.is_durable()) {
        if (unsupported_native_error(confirmed.native_error())) {
            return make_failure(RecordPublishStatus::platform_unsupported, confirmed.native_error(),
                                disposition);
        }
        if (confirmed.status() ==
            durable_immutable_file::PublishStatus::file_ops_contract_violation) {
            return make_failure(RecordPublishStatus::ops_contract_violation,
                                confirmed.native_error(), disposition);
        }
        return make_failure(RecordPublishStatus::canonical_confirm_failed, confirmed.native_error(),
                            disposition);
    }

    const auto canonical_after = inspect_retry(ops, parent_handle, canonical_leaf, expected_bytes);
    if (canonical_after.state() != InspectState::exact) {
        return inspect_failure(canonical_after, RecordPublishStatus::canonical_conflict,
                               RecordPublishStatus::canonical_confirm_failed, disposition);
    }
    if (!valid_inspect_result(canonical_after) ||
        *canonical_after.snapshot() != expected_canonical) {
        return make_failure(RecordPublishStatus::canonical_conflict, protocol_error(), disposition);
    }

    if (should_interrupt(hooks, RecordFaultPoint::CanonicalDurable)) {
        return make_interrupted(disposition, expected_canonical);
    }

    const auto pending_after_confirmation =
        inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
    if (!valid_inspect_result(pending_after_confirmation)) {
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    }

    if (!pending_before_confirmation) {
        if (pending_after_confirmation.state() == InspectState::missing) {
            return detail::RecordPublishResultFactory::durable(disposition, expected_canonical);
        }
        if (pending_after_confirmation.state() == InspectState::exact) {
            return make_failure(RecordPublishStatus::pending_conflict, protocol_error(),
                                disposition);
        }
        return inspect_failure(pending_after_confirmation, RecordPublishStatus::pending_conflict,
                               RecordPublishStatus::pending_cleanup_failed, disposition);
    }

    if (pending_after_confirmation.state() != InspectState::exact) {
        return inspect_failure(pending_after_confirmation, RecordPublishStatus::pending_conflict,
                               RecordPublishStatus::pending_cleanup_failed, disposition);
    }
    if (*pending_after_confirmation.snapshot() != *pending_before_confirmation) {
        return make_failure(RecordPublishStatus::pending_conflict, protocol_error(), disposition);
    }
    if (pending_after_confirmation.snapshot()->identity == expected_canonical.identity) {
        return make_failure(RecordPublishStatus::pending_conflict, protocol_error(), disposition);
    }

    const MutationResult removed =
        ops.remove_exact_at(parent_handle, pending_leaf, *pending_before_confirmation);
    if (!valid_mutation_result(removed)) {
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    }
    switch (removed.state()) {
    case MutationState::succeeded:
        break;
    case MutationState::interrupted:
        return make_interrupted(disposition, expected_canonical);
    case MutationState::unsupported:
        return make_failure(RecordPublishStatus::platform_unsupported, removed.native_error(),
                            disposition);
    case MutationState::identity_mismatch:
    case MutationState::source_missing:
    case MutationState::destination_exists:
        return make_failure(RecordPublishStatus::pending_conflict, removed.native_error(),
                            disposition);
    case MutationState::failed:
        if (unsupported_native_error(removed.native_error())) {
            return make_failure(RecordPublishStatus::platform_unsupported, removed.native_error(),
                                disposition);
        }
        return make_failure(RecordPublishStatus::pending_cleanup_failed, removed.native_error(),
                            disposition);
    default:
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    }

    const auto parent_synced = sync_parent_retry(ops, parent_handle);
    if (parent_synced.state() != durable_immutable_file::OperationState::succeeded) {
        if (parent_synced.state() != durable_immutable_file::OperationState::failed) {
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                disposition);
        }
        if (unsupported_native_error(parent_synced.native_error())) {
            return make_failure(RecordPublishStatus::platform_unsupported,
                                parent_synced.native_error(), disposition);
        }
        return make_failure(RecordPublishStatus::pending_cleanup_failed,
                            parent_synced.native_error(), disposition);
    }

    const auto pending_absent = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
    if (!valid_inspect_result(pending_absent)) {
        return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                            disposition);
    }
    if (pending_absent.state() != InspectState::missing) {
        if (pending_absent.state() == InspectState::exact) {
            return make_failure(RecordPublishStatus::pending_conflict, protocol_error(),
                                disposition);
        }
        return inspect_failure(pending_absent, RecordPublishStatus::pending_conflict,
                               RecordPublishStatus::pending_cleanup_failed, disposition);
    }

    const auto canonical_final = inspect_retry(ops, parent_handle, canonical_leaf, expected_bytes);
    if (canonical_final.state() != InspectState::exact) {
        return inspect_failure(canonical_final, RecordPublishStatus::canonical_conflict,
                               RecordPublishStatus::canonical_confirm_failed, disposition);
    }
    if (!valid_inspect_result(canonical_final) ||
        *canonical_final.snapshot() != expected_canonical) {
        return make_failure(RecordPublishStatus::canonical_conflict, protocol_error(), disposition);
    }

    return detail::RecordPublishResultFactory::durable(disposition, expected_canonical);
}

#ifndef _WIN32

[[nodiscard]] std::error_code posix_error(int value) noexcept {
    return {value, std::generic_category()};
}

[[nodiscard]] std::optional<int> parent_descriptor(NativeHandle handle) noexcept {
    if (handle < 0 || static_cast<std::uintmax_t>(handle) >
                          static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(handle);
}

[[nodiscard]] bool metadata_has_file_policy(const struct stat& metadata) noexcept {
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 || metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_uid) != static_cast<std::uint64_t>(::geteuid()) ||
        (metadata.st_mode & static_cast<mode_t>(07777)) != static_cast<mode_t>(0600)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool metadata_has_bounded_policy(const struct stat& metadata, std::uint64_t min_bytes,
                                               std::uint64_t max_bytes) noexcept {
    if (!metadata_has_file_policy(metadata)) {
        return false;
    }
    const auto size = static_cast<std::uint64_t>(metadata.st_size);
    return size >= min_bytes && size <= max_bytes;
}

[[nodiscard]] bool metadata_has_expected_policy(const struct stat& metadata,
                                                std::uint64_t expected_size) noexcept {
    return metadata_has_bounded_policy(metadata, expected_size, expected_size);
}

[[nodiscard]] NativeIdentity native_identity(const struct stat& metadata) noexcept {
    return NativeIdentity{
        .first = static_cast<std::uint64_t>(metadata.st_dev),
        .second = static_cast<std::uint64_t>(metadata.st_ino),
        .third = 0,
    };
}

[[nodiscard]] bool same_named_object(const struct stat& held, const struct stat& named) noexcept {
    return held.st_dev == named.st_dev && held.st_ino == named.st_ino;
}

[[nodiscard]] bool named_snapshot_matches(const struct stat& metadata,
                                          const RecordSnapshot& expected) noexcept {
    return metadata_has_expected_policy(metadata, expected.size) &&
           native_identity(metadata) == expected.identity;
}

#if defined(__APPLE__)
enum class ExtendedAclState : std::uint8_t {
    absent,
    present,
    unsupported,
    failed,
};

[[nodiscard]] ExtendedAclState inspect_extended_acl(int descriptor,
                                                    std::error_code& error) noexcept {
    acl_t extended_acl = nullptr;
    do {
        errno = 0;
        extended_acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    } while (extended_acl == nullptr && errno == EINTR);
    if (extended_acl != nullptr) {
        (void)::acl_free(extended_acl);
        return ExtendedAclState::present;
    }
    if (errno == ENOENT) {
        return ExtendedAclState::absent;
    }
    if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS) {
        error = posix_error(errno);
        return ExtendedAclState::unsupported;
    }
    error = posix_error(errno);
    return ExtendedAclState::failed;
}
#endif

enum class ParentPolicyState : std::uint8_t {
    accepted,
    rejected,
    unsupported,
    failed,
};

struct ParentPolicyResult final {
    ParentPolicyState state = ParentPolicyState::failed;
    std::error_code error{};
};

[[nodiscard]] ParentPolicyResult inspect_parent_policy(int descriptor) noexcept {
    struct stat parent_metadata{};
    int stat_result = -1;
    do {
        stat_result = ::fstat(descriptor, &parent_metadata);
    } while (stat_result != 0 && errno == EINTR);
    if (stat_result != 0) {
        return {ParentPolicyState::failed, posix_error(errno)};
    }
    if (!S_ISDIR(parent_metadata.st_mode) ||
        static_cast<std::uint64_t>(parent_metadata.st_uid) !=
            static_cast<std::uint64_t>(::geteuid()) ||
        (parent_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return {ParentPolicyState::rejected, protocol_error()};
    }
#if defined(__APPLE__)
    std::error_code acl_error;
    const auto acl_state = inspect_extended_acl(descriptor, acl_error);
    if (acl_state == ExtendedAclState::present) {
        return {ParentPolicyState::rejected, protocol_error()};
    }
    if (acl_state == ExtendedAclState::unsupported) {
        return {ParentPolicyState::unsupported, acl_error};
    }
    if (acl_state == ExtendedAclState::failed) {
        return {ParentPolicyState::failed, acl_error};
    }
#endif
    return {ParentPolicyState::accepted, {}};
}

struct StableRelativeReadResult final {
    BoundedReadState state = BoundedReadState::failed;
    std::optional<RecordSnapshot> snapshot{};
    std::error_code error{};
};

[[nodiscard]] StableRelativeReadResult stable_read_missing() noexcept {
    return {.state = BoundedReadState::missing, .snapshot = std::nullopt, .error = {}};
}

[[nodiscard]] StableRelativeReadResult stable_read_exact(RecordSnapshot snapshot) noexcept {
    return {.state = BoundedReadState::exact, .snapshot = snapshot, .error = {}};
}

[[nodiscard]] StableRelativeReadResult stable_read_error(BoundedReadState state,
                                                         std::error_code error) noexcept {
    return {.state = state, .snapshot = std::nullopt, .error = error_or_protocol(error)};
}

/// One security policy and one streaming read path back both the bounded
/// materializer and the existing exact-byte inspector. `prepare` runs after
/// the first stable size observation; `consume` sees ordered chunks from the
/// same held descriptor. A false callback result is a byte-level rejection.
template <typename Prepare, typename Consume>
[[nodiscard]] StableRelativeReadResult
read_stable_relative_file(NativeHandle parent_handle, const std::filesystem::path& leaf,
                          std::uint64_t min_bytes, std::uint64_t max_bytes, Prepare&& prepare,
                          Consume&& consume) noexcept {
    const auto descriptor = parent_descriptor(parent_handle);
    if (!descriptor) {
        return stable_read_error(BoundedReadState::rejected, posix_error(EBADF));
    }

    const auto map_parent_policy = [](const ParentPolicyResult& policy) noexcept {
        switch (policy.state) {
        case ParentPolicyState::accepted:
            return StableRelativeReadResult{
                .state = BoundedReadState::exact, .snapshot = std::nullopt, .error = {}};
        case ParentPolicyState::rejected:
            return stable_read_error(BoundedReadState::rejected, policy.error);
        case ParentPolicyState::unsupported:
            return stable_read_error(BoundedReadState::unsupported, policy.error);
        case ParentPolicyState::failed:
            return stable_read_error(BoundedReadState::failed, policy.error);
        }
        return stable_read_error(BoundedReadState::failed, protocol_error());
    };

    const auto parent_before = map_parent_policy(inspect_parent_policy(*descriptor));
    if (parent_before.state != BoundedReadState::exact) {
        return parent_before;
    }

    int file_descriptor = -1;
    do {
        file_descriptor =
            ::openat(*descriptor, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } while (file_descriptor < 0 && errno == EINTR);
    if (file_descriptor < 0) {
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return stable_read_missing();
        }
        if (saved_errno == ELOOP) {
            return stable_read_error(BoundedReadState::rejected, posix_error(saved_errno));
        }
        return stable_read_error(BoundedReadState::failed, posix_error(saved_errno));
    }

    const auto close_and_return =
        [&](StableRelativeReadResult intended) noexcept -> StableRelativeReadResult {
        // Do not retry close(2): after EINTR the descriptor state is
        // platform-dependent and a retry can close a reused descriptor.
        if (::close(file_descriptor) == 0) {
            return intended;
        }
        return stable_read_error(BoundedReadState::failed, posix_error(errno));
    };

    struct stat held_before{};
    struct stat named_before{};
    int stat_result = -1;
    do {
        stat_result = ::fstat(file_descriptor, &held_before);
    } while (stat_result != 0 && errno == EINTR);
    if (stat_result != 0) {
        return close_and_return(stable_read_error(BoundedReadState::failed, posix_error(errno)));
    }
    do {
        stat_result = ::fstatat(*descriptor, leaf.c_str(), &named_before, AT_SYMLINK_NOFOLLOW);
    } while (stat_result != 0 && errno == EINTR);
    if (stat_result != 0) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, posix_error(errno)));
    }

    if (!metadata_has_bounded_policy(held_before, min_bytes, max_bytes)) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }
    const auto exact_size = static_cast<std::uint64_t>(held_before.st_size);
    if (exact_size > std::numeric_limits<std::size_t>::max() ||
        !metadata_has_expected_policy(named_before, exact_size) ||
        !same_named_object(held_before, named_before)) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }
#if defined(__APPLE__)
    std::error_code acl_error;
    const auto acl_before = inspect_extended_acl(file_descriptor, acl_error);
    if (acl_before == ExtendedAclState::present) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }
    if (acl_before == ExtendedAclState::unsupported) {
        return close_and_return(stable_read_error(BoundedReadState::unsupported, acl_error));
    }
    if (acl_before == ExtendedAclState::failed) {
        return close_and_return(stable_read_error(BoundedReadState::failed, acl_error));
    }
#endif

    const auto read_size = static_cast<std::size_t>(exact_size);
    try {
        if (!prepare(read_size)) {
            return close_and_return(
                stable_read_error(BoundedReadState::rejected, protocol_error()));
        }
    } catch (const std::bad_alloc&) {
        return close_and_return(stable_read_error(
            BoundedReadState::failed, std::make_error_code(std::errc::not_enough_memory)));
    } catch (...) {
        return close_and_return(
            stable_read_error(BoundedReadState::failed, std::make_error_code(std::errc::io_error)));
    }

    std::array<std::byte, 64U * 1024U> buffer{};
    std::size_t offset = 0;
    while (offset < read_size) {
        const std::size_t requested = std::min(buffer.size(), read_size - offset);
        ssize_t count = -1;
        do {
            count = ::pread(file_descriptor, buffer.data(), requested, static_cast<off_t>(offset));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            return close_and_return(
                stable_read_error(BoundedReadState::failed, posix_error(errno)));
        }
        if (count == 0) {
            return close_and_return(
                stable_read_error(BoundedReadState::rejected, protocol_error()));
        }
        const auto read_count = static_cast<std::size_t>(count);
        if (read_count > requested) {
            return close_and_return(
                stable_read_error(BoundedReadState::rejected, protocol_error()));
        }
        try {
            if (!consume(std::span<const std::byte>(buffer.data(), read_count), offset)) {
                return close_and_return(
                    stable_read_error(BoundedReadState::rejected, protocol_error()));
            }
        } catch (const std::bad_alloc&) {
            return close_and_return(stable_read_error(
                BoundedReadState::failed, std::make_error_code(std::errc::not_enough_memory)));
        } catch (...) {
            return close_and_return(stable_read_error(BoundedReadState::failed,
                                                      std::make_error_code(std::errc::io_error)));
        }
        offset += read_count;
    }

    std::byte trailing{};
    ssize_t trailing_count = -1;
    do {
        trailing_count = ::pread(file_descriptor, &trailing, 1, static_cast<off_t>(offset));
    } while (trailing_count < 0 && errno == EINTR);
    if (trailing_count < 0) {
        return close_and_return(stable_read_error(BoundedReadState::failed, posix_error(errno)));
    }
    if (trailing_count != 0) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }

    struct stat held_after{};
    struct stat named_after{};
    do {
        stat_result = ::fstat(file_descriptor, &held_after);
    } while (stat_result != 0 && errno == EINTR);
    if (stat_result != 0) {
        return close_and_return(stable_read_error(BoundedReadState::failed, posix_error(errno)));
    }
    do {
        stat_result = ::fstatat(*descriptor, leaf.c_str(), &named_after, AT_SYMLINK_NOFOLLOW);
    } while (stat_result != 0 && errno == EINTR);
    if (stat_result != 0) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, posix_error(errno)));
    }
    if (!metadata_has_expected_policy(held_after, exact_size) ||
        !metadata_has_expected_policy(named_after, exact_size) ||
        !same_named_object(held_after, named_after) ||
        native_identity(held_before) != native_identity(held_after)) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }
#if defined(__APPLE__)
    acl_error.clear();
    const auto acl_after = inspect_extended_acl(file_descriptor, acl_error);
    if (acl_after == ExtendedAclState::present) {
        return close_and_return(stable_read_error(BoundedReadState::rejected, protocol_error()));
    }
    if (acl_after == ExtendedAclState::unsupported) {
        return close_and_return(stable_read_error(BoundedReadState::unsupported, acl_error));
    }
    if (acl_after == ExtendedAclState::failed) {
        return close_and_return(stable_read_error(BoundedReadState::failed, acl_error));
    }
#endif

    const auto parent_after = map_parent_policy(inspect_parent_policy(*descriptor));
    if (parent_after.state != BoundedReadState::exact) {
        return close_and_return(parent_after);
    }

    return close_and_return(stable_read_exact(RecordSnapshot{
        .identity = native_identity(held_after),
        .size = exact_size,
    }));
}

[[nodiscard]] MutationResult native_rename_no_replace(int parent_fd, const char* source,
                                                      const char* destination) noexcept {
#if defined(__APPLE__)
    if (::renameatx_np(parent_fd, source, parent_fd, destination, RENAME_EXCL) == 0) {
        return MutationResult::succeeded();
    }
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int rename_noreplace = 1U;
    if (::syscall(SYS_renameat2, parent_fd, source, parent_fd, destination, rename_noreplace) ==
        0) {
        return MutationResult::succeeded();
    }
#else
    (void)parent_fd;
    (void)source;
    (void)destination;
    return MutationResult::unsupported(unsupported_error());
#endif

    const int saved_errno = errno;
    if (saved_errno == ENOENT) {
        return MutationResult::source_missing(posix_error(saved_errno));
    }
    if (saved_errno == EEXIST) {
        return MutationResult::destination_exists(posix_error(saved_errno));
    }
    if (saved_errno == EINTR) {
        return MutationResult::interrupted(posix_error(saved_errno));
    }
    if (saved_errno == ENOSYS || saved_errno == EINVAL || saved_errno == EOPNOTSUPP) {
        return MutationResult::unsupported(posix_error(saved_errno));
    }
    return MutationResult::failed(posix_error(saved_errno));
}

class ProductionRecordOps final : public RecordOps {
public:
    [[nodiscard]] BoundedReadResult read_bounded_at(NativeHandle parent_handle,
                                                    const std::filesystem::path& leaf,
                                                    std::uint64_t min_bytes,
                                                    std::uint64_t max_bytes) noexcept override {
        std::vector<std::byte> bytes;
        const auto prepared = [&](std::size_t exact_size) {
            bytes.resize(exact_size);
            return true;
        };
        const auto captured = [&](std::span<const std::byte> chunk, std::size_t offset) noexcept {
            if (offset > bytes.size() || chunk.size() > bytes.size() - offset) {
                return false;
            }
            std::copy(chunk.begin(), chunk.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            return true;
        };
        const auto outcome = read_stable_relative_file(parent_handle, leaf, min_bytes, max_bytes,
                                                       prepared, captured);
        switch (outcome.state) {
        case BoundedReadState::missing:
            return BoundedReadResult::missing();
        case BoundedReadState::exact:
            if (!outcome.snapshot.has_value()) {
                return BoundedReadResult::failed(protocol_error());
            }
            return BoundedReadResult::exact(std::move(bytes), *outcome.snapshot);
        case BoundedReadState::rejected:
            return BoundedReadResult::rejected(outcome.error);
        case BoundedReadState::interrupted:
            return BoundedReadResult::interrupted(outcome.error);
        case BoundedReadState::unsupported:
            return BoundedReadResult::unsupported(outcome.error);
        case BoundedReadState::failed:
            return BoundedReadResult::failed(outcome.error);
        }
        return BoundedReadResult::failed(protocol_error());
    }

    [[nodiscard]] MutationResult probe_no_replace_at(NativeHandle parent_handle) noexcept override {
        const auto descriptor = parent_descriptor(parent_handle);
        if (!descriptor) {
            return MutationResult::failed(posix_error(EBADF));
        }

        const auto parent_policy = inspect_parent_policy(*descriptor);
        switch (parent_policy.state) {
        case ParentPolicyState::accepted:
            break;
        case ParentPolicyState::rejected:
            return MutationResult::identity_mismatch(parent_policy.error);
        case ParentPolicyState::unsupported:
            return MutationResult::unsupported(parent_policy.error);
        case ParentPolicyState::failed:
            return MutationResult::failed(parent_policy.error);
        }

        // Empty leaf names cannot designate filesystem objects. ENOENT proves
        // that the kernel accepted the handle-relative no-replace operation and
        // reached ordinary source lookup without creating or removing a leaf.
        const auto probed = native_rename_no_replace(*descriptor, "", "");
        if (probed.state() == MutationState::source_missing) {
            return MutationResult::succeeded();
        }
        return probed;
    }

    [[nodiscard]] durable_immutable_file::PublishResult
    publish_pending_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                       std::span<const std::byte> expected_bytes) noexcept override {
        return durable_immutable_file::publish_at(parent_handle, pending_leaf, expected_bytes);
    }

    [[nodiscard]] InspectResult
    inspect_exact_at(NativeHandle parent_handle, const std::filesystem::path& leaf,
                     std::span<const std::byte> expected_bytes) noexcept override {
        const std::uint64_t expected_size = static_cast<std::uint64_t>(expected_bytes.size());
        const auto prepared = [&](std::size_t exact_size) noexcept {
            return exact_size == expected_bytes.size();
        };
        const auto compared = [&](std::span<const std::byte> chunk, std::size_t offset) noexcept {
            if (offset > expected_bytes.size() || chunk.size() > expected_bytes.size() - offset) {
                return false;
            }
            const auto expected_begin =
                expected_bytes.begin() + static_cast<std::ptrdiff_t>(offset);
            return std::equal(chunk.begin(), chunk.end(), expected_begin,
                              expected_begin + static_cast<std::ptrdiff_t>(chunk.size()));
        };
        const auto outcome = read_stable_relative_file(parent_handle, leaf, expected_size,
                                                       expected_size, prepared, compared);
        switch (outcome.state) {
        case BoundedReadState::missing:
            return InspectResult::missing();
        case BoundedReadState::exact:
            if (!outcome.snapshot.has_value()) {
                return InspectResult::failed(protocol_error());
            }
            return InspectResult::exact(*outcome.snapshot);
        case BoundedReadState::rejected:
            return InspectResult::rejected(outcome.error);
        case BoundedReadState::interrupted:
            return InspectResult::interrupted(outcome.error);
        case BoundedReadState::unsupported:
            return InspectResult::unsupported(outcome.error);
        case BoundedReadState::failed:
            return InspectResult::failed(outcome.error);
        }
        return InspectResult::failed(protocol_error());
    }

    [[nodiscard]] MutationResult
    promote_no_replace_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                          const std::filesystem::path& canonical_leaf,
                          const RecordSnapshot& expected_pending) noexcept override {
        const auto descriptor = parent_descriptor(parent_handle);
        if (!descriptor) {
            return MutationResult::failed(posix_error(EBADF));
        }

        struct stat pending_metadata{};
        if (::fstatat(*descriptor, pending_leaf.c_str(), &pending_metadata, AT_SYMLINK_NOFOLLOW) !=
            0) {
            const int saved_errno = errno;
            if (saved_errno == ENOENT) {
                return MutationResult::source_missing(posix_error(saved_errno));
            }
            if (saved_errno == EINTR) {
                return MutationResult::interrupted(posix_error(saved_errno));
            }
            return MutationResult::failed(posix_error(saved_errno));
        }
        if (!named_snapshot_matches(pending_metadata, expected_pending)) {
            return MutationResult::identity_mismatch(protocol_error());
        }

        struct stat canonical_metadata{};
        if (::fstatat(*descriptor, canonical_leaf.c_str(), &canonical_metadata,
                      AT_SYMLINK_NOFOLLOW) == 0) {
            return MutationResult::destination_exists(std::make_error_code(std::errc::file_exists));
        }
        const int canonical_errno = errno;
        if (canonical_errno == EINTR) {
            return MutationResult::interrupted(posix_error(canonical_errno));
        }
        if (canonical_errno != ENOENT) {
            return MutationResult::failed(posix_error(canonical_errno));
        }

        return native_rename_no_replace(*descriptor, pending_leaf.c_str(), canonical_leaf.c_str());
    }

    [[nodiscard]] durable_immutable_file::OperationResult
    sync_parent_directory(NativeHandle parent_handle) noexcept override {
        const auto descriptor = parent_descriptor(parent_handle);
        if (!descriptor) {
            return durable_immutable_file::OperationResult::failed(posix_error(EBADF));
        }
#if defined(__APPLE__)
        const int result = ::fcntl(*descriptor, F_FULLFSYNC);
#else
        const int result = ::fsync(*descriptor);
#endif
        if (result == 0) {
            return durable_immutable_file::OperationResult::succeeded();
        }
        const int saved_errno = errno;
        if (saved_errno == EINTR) {
            return durable_immutable_file::OperationResult::interrupted(posix_error(saved_errno));
        }
        return durable_immutable_file::OperationResult::failed(posix_error(saved_errno));
    }

    [[nodiscard]] durable_immutable_file::PublishResult
    confirm_durable_at(NativeHandle parent_handle,
                       const std::filesystem::path& canonical_leaf) noexcept override {
        return durable_immutable_file::confirm_durable_at(parent_handle, canonical_leaf);
    }

    [[nodiscard]] MutationResult
    remove_exact_at(NativeHandle parent_handle, const std::filesystem::path& pending_leaf,
                    const RecordSnapshot& expected_pending) noexcept override {
        const auto descriptor = parent_descriptor(parent_handle);
        if (!descriptor) {
            return MutationResult::failed(posix_error(EBADF));
        }

        struct stat pending_metadata{};
        if (::fstatat(*descriptor, pending_leaf.c_str(), &pending_metadata, AT_SYMLINK_NOFOLLOW) !=
            0) {
            const int saved_errno = errno;
            if (saved_errno == ENOENT) {
                return MutationResult::source_missing(posix_error(saved_errno));
            }
            if (saved_errno == EINTR) {
                return MutationResult::interrupted(posix_error(saved_errno));
            }
            return MutationResult::failed(posix_error(saved_errno));
        }
        if (!named_snapshot_matches(pending_metadata, expected_pending)) {
            return MutationResult::identity_mismatch(protocol_error());
        }

        if (::unlinkat(*descriptor, pending_leaf.c_str(), 0) == 0) {
            return MutationResult::succeeded();
        }
        const int saved_errno = errno;
        if (saved_errno == ENOENT) {
            return MutationResult::source_missing(posix_error(saved_errno));
        }
        if (saved_errno == EINTR) {
            return MutationResult::interrupted(posix_error(saved_errno));
        }
        return MutationResult::failed(posix_error(saved_errno));
    }
};

#endif

} // namespace

InspectResult::InspectResult(InspectState state, std::optional<RecordSnapshot> snapshot,
                             std::error_code native_error) noexcept
    : state_(state), snapshot_(std::move(snapshot)), native_error_(native_error) {}

InspectResult InspectResult::missing() noexcept {
    return InspectResult(InspectState::missing, std::nullopt, {});
}

InspectResult InspectResult::exact(RecordSnapshot snapshot) noexcept {
    return InspectResult(InspectState::exact, snapshot, {});
}

InspectResult InspectResult::rejected(std::error_code error) noexcept {
    return InspectResult(InspectState::rejected, std::nullopt, error_or_protocol(error));
}

InspectResult InspectResult::interrupted(std::error_code error) noexcept {
    return InspectResult(InspectState::interrupted, std::nullopt, error_or_protocol(error));
}

InspectResult InspectResult::unsupported(std::error_code error) noexcept {
    return InspectResult(InspectState::unsupported, std::nullopt, error_or_protocol(error));
}

InspectResult InspectResult::failed(std::error_code error) noexcept {
    return InspectResult(InspectState::failed, std::nullopt, error_or_protocol(error));
}

BoundedReadResult::BoundedReadResult(BoundedReadState state,
                                     std::optional<std::vector<std::byte>> bytes,
                                     std::optional<RecordSnapshot> snapshot,
                                     std::error_code native_error) noexcept
    : state_(state), bytes_(std::move(bytes)), snapshot_(std::move(snapshot)),
      native_error_(native_error) {}

BoundedReadResult BoundedReadResult::missing() noexcept {
    return BoundedReadResult(BoundedReadState::missing, std::nullopt, std::nullopt, {});
}

BoundedReadResult BoundedReadResult::exact(std::vector<std::byte> bytes,
                                           RecordSnapshot snapshot) noexcept {
    if (bytes.size() != snapshot.size) {
        return failed(protocol_error());
    }
    return BoundedReadResult(BoundedReadState::exact, std::move(bytes), snapshot, {});
}

BoundedReadResult BoundedReadResult::rejected(std::error_code error) noexcept {
    return BoundedReadResult(BoundedReadState::rejected, std::nullopt, std::nullopt,
                             error_or_protocol(error));
}

BoundedReadResult BoundedReadResult::interrupted(std::error_code error) noexcept {
    return BoundedReadResult(BoundedReadState::interrupted, std::nullopt, std::nullopt,
                             error_or_protocol(error));
}

BoundedReadResult BoundedReadResult::unsupported(std::error_code error) noexcept {
    return BoundedReadResult(BoundedReadState::unsupported, std::nullopt, std::nullopt,
                             error_or_protocol(error));
}

BoundedReadResult BoundedReadResult::failed(std::error_code error) noexcept {
    return BoundedReadResult(BoundedReadState::failed, std::nullopt, std::nullopt,
                             error_or_protocol(error));
}

BoundedReadResult RecordOps::read_bounded_at(NativeHandle parent_handle,
                                             const std::filesystem::path& leaf,
                                             std::uint64_t min_bytes,
                                             std::uint64_t max_bytes) noexcept {
    (void)parent_handle;
    (void)leaf;
    (void)min_bytes;
    (void)max_bytes;
    return BoundedReadResult::unsupported(unsupported_error());
}

BoundedReadResult read_bounded_at_with_ops(NativeHandle parent_handle,
                                           const std::filesystem::path& leaf,
                                           std::uint64_t min_bytes, std::uint64_t max_bytes,
                                           RecordOps& ops) noexcept {
    if (const auto invalid =
            validate_bounded_read_request(parent_handle, leaf, min_bytes, max_bytes)) {
        return *invalid;
    }

    try {
        for (;;) {
            auto result = ops.read_bounded_at(parent_handle, leaf, min_bytes, max_bytes);
            if (!valid_bounded_read_result(result, min_bytes, max_bytes)) {
                return BoundedReadResult::failed(protocol_error());
            }
            if (result.state() != BoundedReadState::interrupted) {
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        return BoundedReadResult::failed(std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::filesystem::filesystem_error& error) {
        return BoundedReadResult::failed(error.code());
    } catch (...) {
        return BoundedReadResult::failed(std::make_error_code(std::errc::io_error));
    }
}

BoundedReadResult read_bounded_at(NativeHandle parent_handle, const std::filesystem::path& leaf,
                                  std::uint64_t min_bytes, std::uint64_t max_bytes) noexcept {
    if (const auto invalid =
            validate_bounded_read_request(parent_handle, leaf, min_bytes, max_bytes)) {
        return *invalid;
    }

#if !defined(__APPLE__)
    return BoundedReadResult::unsupported(unsupported_error());
#else
    ProductionRecordOps ops;
    return read_bounded_at_with_ops(parent_handle, leaf, min_bytes, max_bytes, ops);
#endif
}

MutationResult::MutationResult(MutationState state, std::error_code native_error) noexcept
    : state_(state), native_error_(native_error) {}

MutationResult MutationResult::succeeded() noexcept {
    return MutationResult(MutationState::succeeded, {});
}

MutationResult MutationResult::source_missing(std::error_code error) noexcept {
    return MutationResult(MutationState::source_missing, error_or_protocol(error));
}

MutationResult MutationResult::destination_exists(std::error_code error) noexcept {
    return MutationResult(MutationState::destination_exists, error_or_protocol(error));
}

MutationResult MutationResult::identity_mismatch(std::error_code error) noexcept {
    return MutationResult(MutationState::identity_mismatch, error_or_protocol(error));
}

MutationResult MutationResult::interrupted(std::error_code error) noexcept {
    return MutationResult(MutationState::interrupted, error_or_protocol(error));
}

MutationResult MutationResult::unsupported(std::error_code error) noexcept {
    return MutationResult(MutationState::unsupported, error_or_protocol(error));
}

MutationResult MutationResult::failed(std::error_code error) noexcept {
    return MutationResult(MutationState::failed, error_or_protocol(error));
}

RecordPublishResult::RecordPublishResult(RecordPublishStatus status,
                                         RecordPublishDisposition disposition,
                                         std::optional<RecordSnapshot> canonical_snapshot,
                                         std::error_code native_error) noexcept
    : status_(status), disposition_(disposition),
      canonical_snapshot_(std::move(canonical_snapshot)), native_error_(native_error) {}

RecordPublishResult publish_at_with_ops(NativeHandle parent_handle,
                                        const std::filesystem::path& pending_leaf,
                                        const std::filesystem::path& canonical_leaf,
                                        std::span<const std::byte> expected_bytes, RecordOps& ops,
                                        RecordTestHooks hooks) noexcept {
    if (const auto invalid =
            validate_request(parent_handle, pending_leaf, canonical_leaf, expected_bytes)) {
        return *invalid;
    }

    try {
        for (;;) {
            const auto probe = ops.probe_no_replace_at(parent_handle);
            if (!valid_mutation_result(probe)) {
                return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error());
            }
            switch (probe.state()) {
            case MutationState::succeeded:
                break;
            case MutationState::interrupted:
                continue;
            case MutationState::unsupported:
                return make_failure(RecordPublishStatus::platform_unsupported,
                                    probe.native_error());
            case MutationState::failed:
                if (unsupported_native_error(probe.native_error())) {
                    return make_failure(RecordPublishStatus::platform_unsupported,
                                        probe.native_error());
                }
                return make_failure(RecordPublishStatus::promotion_failed, probe.native_error());
            case MutationState::identity_mismatch:
                return make_failure(RecordPublishStatus::invalid_request, probe.native_error());
            case MutationState::source_missing:
            case MutationState::destination_exists:
            default:
                return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error());
            }
            break;
        }

        auto canonical = inspect_retry(ops, parent_handle, canonical_leaf, expected_bytes);
        if (!valid_inspect_result(canonical)) {
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error());
        }
        if (canonical.state() == InspectState::exact) {
            auto pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
            if (!valid_inspect_result(pending)) {
                return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                    RecordPublishDisposition::confirmed_existing);
            }
            if (pending.state() != InspectState::missing &&
                pending.state() != InspectState::exact) {
                return inspect_failure(pending, RecordPublishStatus::pending_conflict,
                                       RecordPublishStatus::pending_cleanup_failed,
                                       RecordPublishDisposition::confirmed_existing);
            }
            const std::optional<RecordSnapshot> pending_snapshot =
                pending.state() == InspectState::exact ? pending.snapshot() : std::nullopt;
            return confirm_canonical_and_cleanup_pending(
                parent_handle, pending_leaf, canonical_leaf, expected_bytes, ops, hooks,
                RecordPublishDisposition::confirmed_existing, *canonical.snapshot(),
                pending_snapshot);
        }
        if (canonical.state() != InspectState::missing) {
            return inspect_failure(canonical, RecordPublishStatus::canonical_conflict,
                                   RecordPublishStatus::canonical_confirm_failed);
        }

        RecordPublishDisposition disposition = RecordPublishDisposition::recovered_pending;
        auto pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
        if (!valid_inspect_result(pending)) {
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error());
        }
        if (pending.state() == InspectState::missing) {
            const auto published =
                ops.publish_pending_at(parent_handle, pending_leaf, expected_bytes);
            if (published.is_durable()) {
                disposition = RecordPublishDisposition::created;
            } else if (published.status() ==
                       durable_immutable_file::PublishStatus::already_exists) {
                disposition = RecordPublishDisposition::recovered_pending;
            } else if (published.status() ==
                       durable_immutable_file::PublishStatus::input_too_large) {
                return make_failure(RecordPublishStatus::input_too_large, published.native_error());
            } else if (published.status() ==
                       durable_immutable_file::PublishStatus::file_ops_contract_violation) {
                return make_failure(RecordPublishStatus::ops_contract_violation,
                                    published.native_error());
            } else if (unsupported_native_error(published.native_error())) {
                return make_failure(RecordPublishStatus::platform_unsupported,
                                    published.native_error());
            } else {
                return make_failure(RecordPublishStatus::pending_publish_failed,
                                    published.native_error());
            }
            pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
        }
        if (pending.state() != InspectState::exact) {
            return inspect_failure(pending, RecordPublishStatus::pending_conflict,
                                   RecordPublishStatus::pending_publish_failed, disposition);
        }
        if (!valid_inspect_result(pending)) {
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                disposition);
        }
        const RecordSnapshot pending_before_durability = *pending.snapshot();

        const auto pending_confirmed = ops.confirm_durable_at(parent_handle, pending_leaf);
        if (!pending_confirmed.is_durable()) {
            if (unsupported_native_error(pending_confirmed.native_error())) {
                return make_failure(RecordPublishStatus::platform_unsupported,
                                    pending_confirmed.native_error(), disposition);
            }
            if (pending_confirmed.status() ==
                durable_immutable_file::PublishStatus::file_ops_contract_violation) {
                return make_failure(RecordPublishStatus::ops_contract_violation,
                                    pending_confirmed.native_error(), disposition);
            }
            return make_failure(RecordPublishStatus::pending_publish_failed,
                                pending_confirmed.native_error(), disposition);
        }

        pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
        if (pending.state() != InspectState::exact) {
            return inspect_failure(pending, RecordPublishStatus::pending_conflict,
                                   RecordPublishStatus::pending_publish_failed, disposition);
        }
        if (!valid_inspect_result(pending) || *pending.snapshot() != pending_before_durability) {
            return make_failure(RecordPublishStatus::pending_conflict, protocol_error(),
                                disposition);
        }
        const RecordSnapshot pending_snapshot = *pending.snapshot();

        if (should_interrupt(hooks, RecordFaultPoint::PendingDurable)) {
            return make_interrupted(disposition);
        }

        const MutationResult promoted = ops.promote_no_replace_at(parent_handle, pending_leaf,
                                                                  canonical_leaf, pending_snapshot);
        if (!valid_mutation_result(promoted)) {
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                disposition);
        }
        switch (promoted.state()) {
        case MutationState::succeeded:
            if (should_interrupt(hooks, RecordFaultPoint::CanonicalPromoted)) {
                return make_interrupted(disposition);
            }
            {
                const auto parent_synced = sync_parent_retry(ops, parent_handle);
                if (parent_synced.state() != durable_immutable_file::OperationState::succeeded) {
                    if (parent_synced.state() != durable_immutable_file::OperationState::failed) {
                        return make_failure(RecordPublishStatus::ops_contract_violation,
                                            protocol_error(), disposition);
                    }
                    if (unsupported_native_error(parent_synced.native_error())) {
                        return make_failure(RecordPublishStatus::platform_unsupported,
                                            parent_synced.native_error(), disposition);
                    }
                    return make_failure(RecordPublishStatus::parent_sync_failed,
                                        parent_synced.native_error(), disposition);
                }
            }

            canonical = inspect_retry(ops, parent_handle, canonical_leaf, expected_bytes);
            if (canonical.state() != InspectState::exact) {
                return inspect_failure(canonical, RecordPublishStatus::canonical_conflict,
                                       RecordPublishStatus::canonical_confirm_failed, disposition);
            }
            if (!valid_inspect_result(canonical) || *canonical.snapshot() != pending_snapshot) {
                return make_failure(RecordPublishStatus::canonical_conflict, protocol_error(),
                                    disposition);
            }

            pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
            if (!valid_inspect_result(pending)) {
                return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                    disposition);
            }
            if (pending.state() != InspectState::missing) {
                if (pending.state() == InspectState::exact) {
                    return make_failure(RecordPublishStatus::pending_conflict, protocol_error(),
                                        disposition);
                }
                return inspect_failure(pending, RecordPublishStatus::pending_conflict,
                                       RecordPublishStatus::pending_cleanup_failed, disposition);
            }
            return confirm_canonical_and_cleanup_pending(
                parent_handle, pending_leaf, canonical_leaf, expected_bytes, ops, hooks,
                disposition, *canonical.snapshot(), std::nullopt);

        case MutationState::destination_exists:
            canonical = inspect_retry(ops, parent_handle, canonical_leaf, expected_bytes);
            if (canonical.state() != InspectState::exact) {
                return inspect_failure(canonical, RecordPublishStatus::canonical_conflict,
                                       RecordPublishStatus::canonical_confirm_failed,
                                       RecordPublishDisposition::confirmed_existing);
            }
            pending = inspect_retry(ops, parent_handle, pending_leaf, expected_bytes);
            if (!valid_inspect_result(pending)) {
                return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                    RecordPublishDisposition::confirmed_existing);
            }
            if (pending.state() != InspectState::exact) {
                return inspect_failure(pending, RecordPublishStatus::pending_conflict,
                                       RecordPublishStatus::pending_cleanup_failed,
                                       RecordPublishDisposition::confirmed_existing);
            }
            if (*pending.snapshot() != pending_snapshot) {
                return make_failure(RecordPublishStatus::pending_conflict, protocol_error(),
                                    RecordPublishDisposition::confirmed_existing);
            }
            return confirm_canonical_and_cleanup_pending(
                parent_handle, pending_leaf, canonical_leaf, expected_bytes, ops, hooks,
                RecordPublishDisposition::confirmed_existing, *canonical.snapshot(),
                pending_snapshot);

        case MutationState::interrupted:
            return make_interrupted(disposition);
        case MutationState::unsupported:
            return make_failure(RecordPublishStatus::platform_unsupported, promoted.native_error(),
                                disposition);
        case MutationState::identity_mismatch:
        case MutationState::source_missing:
            return make_failure(RecordPublishStatus::pending_conflict, promoted.native_error(),
                                disposition);
        case MutationState::failed:
            if (unsupported_native_error(promoted.native_error())) {
                return make_failure(RecordPublishStatus::platform_unsupported,
                                    promoted.native_error(), disposition);
            }
            return make_failure(RecordPublishStatus::promotion_failed, promoted.native_error(),
                                disposition);
        default:
            return make_failure(RecordPublishStatus::ops_contract_violation, protocol_error(),
                                disposition);
        }
    } catch (const std::bad_alloc&) {
        return make_failure(RecordPublishStatus::unexpected_failure,
                            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::filesystem::filesystem_error& error) {
        return make_failure(RecordPublishStatus::unexpected_failure, error.code());
    } catch (...) {
        return make_failure(RecordPublishStatus::unexpected_failure,
                            std::make_error_code(std::errc::io_error));
    }
}

RecordPublishResult publish_at(NativeHandle parent_handle,
                               const std::filesystem::path& pending_leaf,
                               const std::filesystem::path& canonical_leaf,
                               std::span<const std::byte> expected_bytes,
                               RecordTestHooks hooks) noexcept {
    if (const auto invalid =
            validate_request(parent_handle, pending_leaf, canonical_leaf, expected_bytes)) {
        return *invalid;
    }

#if defined(_WIN32) || (!defined(__APPLE__) && !(defined(__linux__) && defined(SYS_renameat2)))
    (void)hooks;
    return make_failure(RecordPublishStatus::platform_unsupported, unsupported_error());
#else
    ProductionRecordOps ops;
    return publish_at_with_ops(parent_handle, pending_leaf, canonical_leaf, expected_bytes, ops,
                               hooks);
#endif
}

} // namespace gnfs::util::durable_immutable_record
