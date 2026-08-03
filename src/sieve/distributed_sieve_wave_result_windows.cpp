#include <gnfs/sieve/distributed_sieve.hpp>

#if defined(_WIN32)

#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

namespace gnfs::sieve {
namespace {

[[noreturn]] void throw_wave_result_platform_unsupported() {
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                            "distributed sieve wave result is unavailable on Windows");
}

} // namespace

struct DistributedSieveWaveResult::State final {};

DistributedSieveWaveResult::DistributedSieveWaveResult(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

DistributedSieveWaveResult::DistributedSieveWaveResult(DistributedSieveWaveResult&&) noexcept =
    default;

DistributedSieveWaveResult::~DistributedSieveWaveResult() noexcept = default;

bool DistributedSieveWaveResult::valid() const noexcept {
    return false;
}

DistributedSieveWaveResult::operator bool() const noexcept {
    return false;
}

std::size_t DistributedSieveWaveResult::relation_count() const noexcept {
    return 0U;
}

std::size_t DistributedSieveWaveResult::completed_worker_count() const noexcept {
    return 0U;
}

const util::Sha256Digest& DistributedSieveWaveResult::manifest_digest() const& {
    throw_wave_result_platform_unsupported();
}

const util::Sha256Digest& DistributedSieveWaveResult::merge_commit_digest() const& {
    throw_wave_result_platform_unsupported();
}

std::span<const ChunkCommitSummaryV1> DistributedSieveWaveResult::chunks() const& {
    throw_wave_result_platform_unsupported();
}

const relation::ReadOnlyRelationCorpusView& DistributedSieveWaveResult::merged_relations() const& {
    throw_wave_result_platform_unsupported();
}

} // namespace gnfs::sieve

#endif
