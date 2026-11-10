#pragma once

#include "../core/integer.hpp"

#include <algorithm>
#include <cstddef>
#include <gmp.h>
#include <utility>
#include <vector>

namespace gnfs::sqrt::detail {

/// Reduce the rectangular relation matrix to its nonnegative Smith diagonal in place.
/// Input entries can be small even when the unimodular operations need large integers.
inline void reduce_class_group_relations(std::vector<std::vector<core::Integer>>& matrix) {
    if (matrix.empty())
        return;
    const std::size_t num_rows = matrix.size();
    const std::size_t num_cols = matrix.front().size();
    const std::size_t min_dim = std::min(num_rows, num_cols);
    core::Integer q;
    // Smith Normal Form via row/column operations over Z
    for (std::size_t k = 0; k < min_dim; ++k) {
        // Phase 1: Find smallest nonzero |entry| and move to (k,k)
        while (true) {
            mpz_srcptr smallest = nullptr;
            std::size_t min_r = k, min_c = k;
            bool unit_found = false;
            for (std::size_t i = k; i < num_rows && !unit_found; ++i) {
                for (std::size_t j = k; j < num_cols; ++j) {
                    const auto entry = matrix[i][j].get_mpz();
                    if (mpz_sgn(entry) != 0 && (!smallest || mpz_cmpabs(entry, smallest) < 0)) {
                        smallest = entry;
                        min_r = i;
                        min_c = j;
                        if (mpz_cmpabs_ui(entry, 1) == 0) {
                            unit_found = true;
                            break;
                        }
                    }
                }
            }
            if (!smallest)
                return; // The remaining rectangular submatrix is zero.

            // Move pivot to (k,k)
            if (min_r != k)
                std::swap(matrix[min_r], matrix[k]);
            if (min_c != k) {
                for (std::size_t i = 0; i < num_rows; ++i) {
                    std::swap(matrix[i][min_c], matrix[i][k]);
                }
            }

            // Phase 2: Eliminate row k and column k
            bool changed = true;
            while (changed) {
                changed = false;

                // Eliminate column k
                for (std::size_t i = k + 1; i < num_rows; ++i) {
                    if (!matrix[i][k].is_zero()) {
                        mpz_tdiv_q(q.get_mpz(), matrix[i][k].get_mpz(), matrix[k][k].get_mpz());
                        for (std::size_t j = k; j < num_cols; ++j) {
                            mpz_submul(matrix[i][j].get_mpz(), q.get_mpz(), matrix[k][j].get_mpz());
                        }
                        if (!matrix[i][k].is_zero()) {
                            // Didn't fully eliminate — swap and retry
                            std::swap(matrix[i], matrix[k]);
                            changed = true;
                            break;
                        }
                    }
                }
                if (changed)
                    continue;

                // Eliminate row k
                for (std::size_t j = k + 1; j < num_cols; ++j) {
                    if (!matrix[k][j].is_zero()) {
                        mpz_tdiv_q(q.get_mpz(), matrix[k][j].get_mpz(), matrix[k][k].get_mpz());
                        for (std::size_t i = k; i < num_rows; ++i) {
                            mpz_submul(matrix[i][j].get_mpz(), q.get_mpz(), matrix[i][k].get_mpz());
                        }
                        if (!matrix[k][j].is_zero()) {
                            // Swap columns and retry
                            for (std::size_t i = 0; i < num_rows; ++i) {
                                std::swap(matrix[i][j], matrix[i][k]);
                            }
                            changed = true;
                            break;
                        }
                    }
                }
            }

            // A unit already divides the entire remaining submatrix.
            if (mpz_cmpabs_ui(matrix[k][k].get_mpz(), 1) == 0)
                break;

            // Phase 3: Check divisibility — m[k][k] must divide all entries below
            bool divisible = true;
            for (std::size_t i = k + 1; i < num_rows && divisible; ++i) {
                for (std::size_t j = k + 1; j < num_cols && divisible; ++j) {
                    if (!mpz_divisible_p(matrix[i][j].get_mpz(), matrix[k][k].get_mpz())) {
                        // Add row i to row k, then re-eliminate
                        for (std::size_t c = k; c < num_cols; ++c) {
                            matrix[k][c] += matrix[i][c];
                        }
                        divisible = false;
                    }
                }
            }
            if (divisible)
                break; // done with position k
            // else: loop back — now (k,k) has a smaller value
        }

        // Make diagonal entry positive
        if (matrix[k][k].is_negative())
            matrix[k][k].negate();
    }
}

} // namespace gnfs::sqrt::detail
