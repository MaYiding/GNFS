#pragma once

// Source-private, origin-neutral proof that one canonical MergePreparedV1 is
// backed by a live authority root. The sealed continuation owns the
// origin-specific state and exposes no paths, descriptors, or cleanup
// capabilities to the next transition.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>
#include <gnfs/util/durable_immutable_record.hpp>
#include <gnfs/util/process.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_resume_detail {

class DistributedSieveWaveStore;

} // namespace gnfs::sieve::distributed_sieve_resume_detail

namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail {

class DistributedSieveWaveMergeCommitAuthorityV1;

} // namespace gnfs::sieve::distributed_sieve_merge_commit_authority_detail

namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail {

class DistributedSieveMergeWriterAuthorityV1;
class DistributedSieveMergePreparedAdmissionV1;
class DistributedSieveCommittedTailAdmissionV1;

/// Exact immutable-record identities for every merge-commit predecessor.
/// Worker attempts are ordered by manifest slot then attempt ordinal; merge
/// starts are ordered by merge-attempt ordinal. The value contains no path,
/// descriptor, publication, or cleanup capability.
struct DistributedSieveMergeCommitPredecessorSnapshotsV1 final {
    std::vector<util::durable_immutable_record::RecordSnapshot> worker_attempts;
    std::vector<util::durable_immutable_record::RecordSnapshot> merge_starts;

    [[nodiscard]] friend bool
    operator==(const DistributedSieveMergeCommitPredecessorSnapshotsV1&,
               const DistributedSieveMergeCommitPredecessorSnapshotsV1&) = default;
};

/// Origin-specific lifetime root behind the sealed prepared continuation.
///
/// Only the commit authority can ask for the retained WaveStore or immutable
/// dependency views. The interface deliberately has no path, descriptor,
/// generic publication, or cleanup operation.
class DistributedSieveMergePreparedOriginV1 {
public:
    DistributedSieveMergePreparedOriginV1() = default;
    DistributedSieveMergePreparedOriginV1(const DistributedSieveMergePreparedOriginV1&) = delete;
    DistributedSieveMergePreparedOriginV1&
    operator=(const DistributedSieveMergePreparedOriginV1&) = delete;
    virtual ~DistributedSieveMergePreparedOriginV1() = default;

private:
    [[nodiscard]] virtual bool
    prepared_origin_valid(const MergePreparedV1* stable_record,
                          std::uint64_t creator_process_id) const noexcept = 0;
    [[nodiscard]] virtual distributed_sieve_resume_detail::DistributedSieveWaveStore*
    retained_wave_store() noexcept = 0;
    [[nodiscard]] virtual std::span<const MergeStartedV1>
    retained_merge_started_chain() const noexcept = 0;
    [[nodiscard]] virtual const DistributedSieveMergeCommitPredecessorSnapshotsV1*
    retained_predecessor_snapshots() const noexcept = 0;
    [[nodiscard]] virtual std::size_t retained_manifest_slot_count() const noexcept = 0;
    [[nodiscard]] virtual const WorkerHandoffV1*
    retained_worker_handoff(std::size_t manifest_slot) const noexcept = 0;

    friend class DistributedSieveMergePreparedAdmissionV1;
    friend class DistributedSieveMergePreparedCommitContextV1;
    friend class DistributedSieveCommittedTailAdmissionV1;
    friend class ::gnfs::sieve::distributed_sieve_merge_commit_authority_detail::
        DistributedSieveWaveMergeCommitAuthorityV1;
};

/// Origin-neutral, single-owner continuation consumed only by the commit
/// authority. Its private accessors provide typed durable facts, never raw
/// paths, handles, or cleanup authority.
class DistributedSieveMergePreparedCommitContextV1 final {
public:
    DistributedSieveMergePreparedCommitContextV1() = delete;
    DistributedSieveMergePreparedCommitContextV1(
        const DistributedSieveMergePreparedCommitContextV1&) = delete;
    DistributedSieveMergePreparedCommitContextV1&
    operator=(const DistributedSieveMergePreparedCommitContextV1&) = delete;
    DistributedSieveMergePreparedCommitContextV1(
        DistributedSieveMergePreparedCommitContextV1&&) noexcept = default;
    DistributedSieveMergePreparedCommitContextV1&
    operator=(DistributedSieveMergePreparedCommitContextV1&&) = delete;
    ~DistributedSieveMergePreparedCommitContextV1() = default;

private:
    DistributedSieveMergePreparedCommitContextV1(
        std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin,
        const MergePreparedV1* stable_record, std::uint64_t creator_process_id) noexcept
        : origin_(std::move(origin)), record_(stable_record),
          creator_process_id_(creator_process_id) {}

    [[nodiscard]] bool valid() const noexcept {
        if (origin_ == nullptr || record_ == nullptr || creator_process_id_ == 0) {
            return false;
        }
        const int process_id = gnfs::util::process_id();
        return process_id > 0 && creator_process_id_ == static_cast<std::uint64_t>(process_id) &&
               origin_->prepared_origin_valid(record_, creator_process_id_);
    }

    std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin_;
    const MergePreparedV1* record_ = nullptr;
    std::uint64_t creator_process_id_ = 0;

    friend class ::gnfs::sieve::distributed_sieve_merge_commit_authority_detail::
        DistributedSieveWaveMergeCommitAuthorityV1;
};

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
        : origin_(std::move(other.origin_)), record_(std::exchange(other.record_, nullptr)),
          creator_process_id_(std::exchange(other.creator_process_id_, 0)) {}
    DistributedSieveMergePreparedAdmissionV1&
    operator=(DistributedSieveMergePreparedAdmissionV1&&) = delete;
    ~DistributedSieveMergePreparedAdmissionV1() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        if (origin_ == nullptr || record_ == nullptr || creator_process_id_ == 0) {
            return false;
        }
        const int process_id = gnfs::util::process_id();
        return process_id > 0 && creator_process_id_ == static_cast<std::uint64_t>(process_id) &&
               origin_->prepared_origin_valid(record_, creator_process_id_);
    }

    [[nodiscard]] const MergePreparedV1& record() const noexcept {
        return *record_;
    }

private:
    explicit DistributedSieveMergePreparedAdmissionV1(
        std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin,
        const MergePreparedV1* stable_record, std::uint64_t creator_process_id) noexcept
        : origin_(std::move(origin)), record_(stable_record),
          creator_process_id_(creator_process_id) {}

    std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin_;
    const MergePreparedV1* record_ = nullptr;
    std::uint64_t creator_process_id_ = 0;

    friend class DistributedSieveMergeWriterAuthorityV1;
    friend class ::gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
    friend class ::gnfs::sieve::distributed_sieve_merge_commit_authority_detail::
        DistributedSieveWaveMergeCommitAuthorityV1;
};

/// Read-only continuation after one exact WaveMergeCommitV1 is canonical and
/// durable. The origin anchor retains the merged corpus and every worker
/// handoff/read lock for the later cleanup transition, while this facade grants
/// no cleanup or arbitrary namespace capability.
class DistributedSieveCommittedTailAdmissionV1 final {
public:
    DistributedSieveCommittedTailAdmissionV1() = delete;
    DistributedSieveCommittedTailAdmissionV1(const DistributedSieveCommittedTailAdmissionV1&) =
        delete;
    DistributedSieveCommittedTailAdmissionV1&
    operator=(const DistributedSieveCommittedTailAdmissionV1&) = delete;
    DistributedSieveCommittedTailAdmissionV1(DistributedSieveCommittedTailAdmissionV1&&) noexcept =
        default;
    DistributedSieveCommittedTailAdmissionV1&
    operator=(DistributedSieveCommittedTailAdmissionV1&&) = delete;
    ~DistributedSieveCommittedTailAdmissionV1() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] const WaveMergeCommitV1& record() const noexcept {
        return commit_;
    }

    [[nodiscard]] const util::durable_immutable_record::RecordSnapshot&
    canonical_snapshot() const noexcept {
        return canonical_snapshot_;
    }

private:
    DistributedSieveCommittedTailAdmissionV1(
        std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin,
        const MergePreparedV1* prepared_record, WaveMergeCommitV1 commit,
        DistributedSieveMergeCommitPredecessorSnapshotsV1 predecessor_snapshots,
        util::durable_immutable_record::RecordSnapshot canonical_snapshot,
        std::uint64_t creator_process_id) noexcept
        : origin_(std::move(origin)), prepared_record_(prepared_record), commit_(std::move(commit)),
          predecessor_snapshots_(std::move(predecessor_snapshots)),
          canonical_snapshot_(canonical_snapshot), creator_process_id_(creator_process_id) {}

    std::shared_ptr<DistributedSieveMergePreparedOriginV1> origin_;
    const MergePreparedV1* prepared_record_ = nullptr;
    WaveMergeCommitV1 commit_;
    DistributedSieveMergeCommitPredecessorSnapshotsV1 predecessor_snapshots_;
    util::durable_immutable_record::RecordSnapshot canonical_snapshot_;
    std::uint64_t creator_process_id_ = 0;

    friend class ::gnfs::sieve::distributed_sieve_resume_detail::DistributedSieveWaveStore;
    friend class ::gnfs::sieve::distributed_sieve_merge_commit_authority_detail::
        DistributedSieveWaveMergeCommitAuthorityV1;
};

} // namespace gnfs::sieve::distributed_sieve_merge_writer_authority_detail
