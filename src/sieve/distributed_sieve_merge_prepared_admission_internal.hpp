#pragma once

// Source-private, origin-neutral proof that one canonical MergePreparedV1 is
// backed by a live authority root. The anchor owns the origin-specific state;
// the stable record pointer and validator expose no paths, handles, or writer
// capabilities to the next transition.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/process.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveWaveStore;

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

class DistributedSieveMergeWriterAuthorityV1;

/// Read-only, move-only proof of canonical MergePrepared publication. Fresh
/// and resumed origins share this boundary while retaining their complete,
/// origin-specific lifetime behind a type-erased shared anchor.
class DistributedSieveMergePreparedAdmissionV1 final {
public:
    DistributedSieveMergePreparedAdmissionV1() = delete;
    DistributedSieveMergePreparedAdmissionV1(const DistributedSieveMergePreparedAdmissionV1&) =
        delete;
    DistributedSieveMergePreparedAdmissionV1&
    operator=(const DistributedSieveMergePreparedAdmissionV1&) = delete;
    DistributedSieveMergePreparedAdmissionV1(
        DistributedSieveMergePreparedAdmissionV1&& other) noexcept
        : lifetime_anchor_(std::move(other.lifetime_anchor_)),
          record_(std::exchange(other.record_, nullptr)),
          creator_process_id_(std::exchange(other.creator_process_id_, 0)),
          origin_validator_(std::exchange(other.origin_validator_, nullptr)) {}
    DistributedSieveMergePreparedAdmissionV1&
    operator=(DistributedSieveMergePreparedAdmissionV1&&) = delete;
    ~DistributedSieveMergePreparedAdmissionV1() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        if (lifetime_anchor_ == nullptr || record_ == nullptr || creator_process_id_ == 0 ||
            origin_validator_ == nullptr) {
            return false;
        }
        const int process_id = gnfs::util::process_id();
        return process_id > 0 && creator_process_id_ == static_cast<std::uint64_t>(process_id) &&
               origin_validator_(lifetime_anchor_.get(), record_, creator_process_id_);
    }

    [[nodiscard]] const MergePreparedV1& record() const noexcept {
        return *record_;
    }

private:
    using OriginValidatorV1 = bool (*)(const void* lifetime_anchor,
                                       const MergePreparedV1* stable_record,
                                       std::uint64_t creator_process_id) noexcept;

    explicit DistributedSieveMergePreparedAdmissionV1(std::shared_ptr<const void> lifetime_anchor,
                                                      const MergePreparedV1* stable_record,
                                                      std::uint64_t creator_process_id,
                                                      OriginValidatorV1 origin_validator) noexcept
        : lifetime_anchor_(std::move(lifetime_anchor)), record_(stable_record),
          creator_process_id_(creator_process_id), origin_validator_(origin_validator) {}

    std::shared_ptr<const void> lifetime_anchor_;
    const MergePreparedV1* record_ = nullptr;
    std::uint64_t creator_process_id_ = 0;
    OriginValidatorV1 origin_validator_ = nullptr;

    friend class DistributedSieveMergeWriterAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
};

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail
