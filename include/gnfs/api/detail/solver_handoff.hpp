#pragma once

#include "../../relation/reduction_engine.hpp"

#include <cstddef>
#include <functional>
#include <utility>

namespace gnfs::api::detail {

struct SolverHandoffInfo {
    size_t relation_rows;
    size_t estimated_effective_columns;
    bool estimated_underbuilt;
};

/// Transfer the finalized reduction to the matrix solver exactly once.
///
/// The collection-time column estimate is diagnostic only. Matrix construction
/// and the solver retain authority over the actual excess/thin decision.
template <typename Diagnostic, typename Solver>
[[nodiscard]]
decltype(auto) handoff_after_collection(relation::RelationReductionResult reduction,
                                        size_t estimated_effective_columns, Diagnostic&& diagnostic,
                                        Solver&& solver) {
    const size_t relation_rows = reduction.size();
    const SolverHandoffInfo info{
        relation_rows,
        estimated_effective_columns,
        relation_rows <= estimated_effective_columns,
    };

    if (info.estimated_underbuilt) {
        std::invoke(std::forward<Diagnostic>(diagnostic), info);
    }

    return std::invoke(std::forward<Solver>(solver), std::move(reduction));
}

} // namespace gnfs::api::detail
