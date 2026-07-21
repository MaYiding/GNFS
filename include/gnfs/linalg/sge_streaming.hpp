#pragma once

// SGE-OOC: streaming convenience that combines a streaming matrix build
// with the standard in-memory SGE preprocess. Lets the caller go directly
// from RelationSource → SGEResult without ever materializing
// vector<Relation> in RAM.

#include "../relation/relation_corpus.hpp"
#include "matrix_builder.hpp"
#include "relation_source.hpp"
#include "sge.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnfs::linalg {

/// Combined streaming output: the raw matrix build + the SGE preprocessing
/// result on it. Callers that only want the reduced matrix can use sge().
struct StreamingSGEResult {
    MatrixBuildResult build_result;
    SGEResult sge_result;
};

/// Convert an SGE dependency over matrix rows into a corpus-bound relation
/// selection. The row mapping and expanded dependency must describe the same
/// matrix, and every mapped ordinal must belong to the supplied corpus.
/// Duplicate ordinals are folded with GF(2) parity by RelationSelection.
[[nodiscard]] inline relation::RelationSelection
dependency_to_relation_selection(const relation::RelationCorpus& corpus,
                                 std::span<const std::size_t> row_to_relation,
                                 const std::vector<bool>& expanded_dependency) {
    if (row_to_relation.size() != expanded_dependency.size()) {
        throw std::invalid_argument(
            "dependency_to_relation_selection: row mapping and dependency length mismatch");
    }

    const std::size_t corpus_count = corpus.count();
    std::vector<std::size_t> ordinals;
    for (std::size_t row = 0; row < row_to_relation.size(); ++row) {
        const std::size_t ordinal = row_to_relation[row];
        if (ordinal >= corpus_count) {
            throw std::out_of_range(
                "dependency_to_relation_selection: relation ordinal out of range");
        }
        if (expanded_dependency[row]) {
            ordinals.push_back(ordinal);
        }
    }

    return relation::RelationSelection::from_xor_ordinals(corpus, std::move(ordinals));
}

/// Streaming-source variant of MatrixBuilder + SGE preprocessing.
///
/// Builds the GF(2) matrix row-by-row from any RelationSource (vector or
/// OOC mmap) then runs the standard in-memory SGE on the resulting
/// SparseMatrix. SGE itself works in-memory on the sparse matrix because
/// SGE's working set (col_to_rows index + row_composition) is bounded by
/// matrix nnz, which is already much smaller than the relation vector
/// (sparse matrix is ~100MB for 1M relations, vs ~500MB raw relations).
///
/// The streaming win is in the matrix-build phase: vector<Relation> never
/// exists in memory, so the Phase 5 peak shifts from
///   relations_vec + matrix + sge_state
/// to
///   matrix + sge_state
///
/// Returns both the build result (mapping, row_to_relation) and the
/// SGE result. The caller uses sge_result.expand_dependency to map BL
/// dependencies back to original relation or corpus ordinals via
/// build_result.row_to_relation.
template <RelationSource Source>
[[nodiscard]] inline StreamingSGEResult preprocess_streaming(
        const Source& source,
        const FactorBase& fb,
        const core::PolynomialContext& ctx,
        const MatrixBuilderConfig& mb_config = MatrixBuilderConfig{},
        const SGEConfig& sge_config = SGEConfig{}) {

    StreamingSGEResult out;

    MatrixBuilder mb(mb_config);
    out.build_result = mb.build_with_qc_streaming(source, fb, ctx);
    out.sge_result = SGE::preprocess(out.build_result.matrix, sge_config);
    return out;
}

} // namespace gnfs::linalg
