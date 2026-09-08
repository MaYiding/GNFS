#include "gnfs/linalg/schirokauer.hpp"

#include <array>
#include <cstdint>
#include <iostream>

using gnfs::linalg::FastPoly;

int main() {
    // For f(x) = x^6 + 1 over Z/256Z, x^5*x^5 = x^10 = -x^4.
    // This exercises the full raw convolution buffer: two legal degree-five
    // operands reach index 10 before reduction.
    constexpr std::uint64_t modulus = 256;
    constexpr std::uint32_t polynomial_degree = 6;
    const std::array<std::uint64_t, polynomial_degree + 1> f = {1, 0, 0, 0, 0, 0, 1};

    FastPoly lhs;
    lhs.deg = polynomial_degree - 1;
    lhs.coeffs[lhs.deg] = 1;
    FastPoly rhs = lhs;

    const auto reduced = FastPoly::mul(lhs, rhs, f.data(), polynomial_degree, modulus);
    if (reduced.deg != 4 || reduced.coeff(4) != modulus - 1) {
        std::cerr << "degree-6 FastPoly reduction mismatch: deg=" << reduced.deg
                  << " coeff[4]=" << reduced.coeff(4) << '\n';
        return 1;
    }
    for (std::size_t i = 0; i < reduced.deg; ++i) {
        if (i != 4 && reduced.coeff(i) != 0) {
            std::cerr << "degree-6 FastPoly reduction has unexpected coeff[" << i
                      << "]=" << reduced.coeff(i) << '\n';
            return 1;
        }
    }

    std::cout << "FastPoly degree-6 product/reduction passed\n";
    return 0;
}
