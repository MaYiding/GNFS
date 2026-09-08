#pragma once

#include <cstddef>

namespace gnfs::core {

/// Maximum logical cells in one materialized lattice-sieve array.
///
/// The limit is deliberately expressed in cells rather than bytes because
/// the backing representation is fixed at uint16_t. It preserves the largest
/// automatic GNFS geometry (512 Mi cells, 1 GiB of score storage) while
/// preventing an explicit configuration from requesting an unbounded vector.
inline constexpr std::size_t SIEVE_MAX_REGION_CELLS = std::size_t{512} * 1024 * 1024;

} // namespace gnfs::core
