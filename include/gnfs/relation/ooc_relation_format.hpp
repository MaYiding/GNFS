#pragma once

#include <cstdint>

namespace gnfs::relation {

/// Stable binary-layout constants shared by OOC relation persistence,
/// checkpoint validation, and compatibility readers.
struct OOCRelationStoreFormat final {
    static constexpr uint64_t MAGIC_V1_FINAL = 0x474E46535245494CULL; // 'GNFSREIL'
    static constexpr uint64_t MAGIC_INCOMPLETE_V1 = 0x474E46535245494EULL;
    static constexpr uint64_t MAGIC_V2_FINAL = 0x474E46535232464CULL;      // 'GNFSR2FL'
    static constexpr uint64_t MAGIC_V2_INCOMPLETE = 0x474E46535232494EULL; // 'GNFSR2IN'
    static constexpr uint64_t MAGIC_V3_FINAL = 0x474E46535233464CULL;      // 'GNFSR3FL'
    static constexpr uint64_t MAGIC_V3_INCOMPLETE = 0x474E46535233494EULL; // 'GNFSR3IN'
    static constexpr uint64_t MAGIC_V3_DATA = 0x474E465352334441ULL;       // 'GNFSR3DA'

    static constexpr uint64_t FORMAT_VERSION_V2 = 2;
    static constexpr uint64_t FORMAT_VERSION_V3 = 3;

    static constexpr uint64_t INDEX_FORMAT_VERSION_OFFSET = 8;
    static constexpr uint64_t INDEX_STORE_ID_OFFSET = 16;
    static constexpr uint64_t INDEX_COUNT_OFFSET = 24;
    static constexpr uint64_t INDEX_HEADER_BYTES = 32;
    static constexpr uint64_t INDEX_SENTINEL_BYTES = 8;

    static constexpr uint64_t DATA_FORMAT_VERSION_OFFSET = 8;
    static constexpr uint64_t DATA_STORE_ID_OFFSET = 16;
    static constexpr uint64_t DATA_HEADER_BYTES = 24;
};

} // namespace gnfs::relation
