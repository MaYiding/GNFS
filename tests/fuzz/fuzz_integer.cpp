#include "gnfs/core/integer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace {

using gnfs::core::Integer;

std::string decimal_from_bytes(const uint8_t* data, std::size_t size, bool reverse) {
    constexpr std::size_t kMaxDigits = 48;
    const std::size_t n = std::min(size, kMaxDigits);

    std::string out;
    out.reserve(n == 0 ? 1 : n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = reverse ? (n - 1 - i) : i;
        out.push_back(static_cast<char>('0' + (data[idx] % 10)));
    }

    const auto first_non_zero = out.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    out.erase(0, first_non_zero);
    return out;
}

[[noreturn]] void invariant_failed() {
#if defined(__clang__) || defined(__GNUC__)
    __builtin_trap();
#else
    std::terminate();
#endif
}

void require(bool ok) {
    if (!ok) {
        invariant_failed();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    try {
        const std::size_t split = std::max<std::size_t>(1, size / 2);
        Integer a(decimal_from_bytes(data, split, false));
        Integer b(decimal_from_bytes(data + split, size - split, true));
        if (b.is_zero()) {
            b = Integer(1);
        }

        const Integer sum = a + b;
        require((sum - a) == b);
        require((sum - b) == a);

        const Integer product = a * b;
        require((product / b) == a);
        require((product % b).is_zero());

        Integer modulus = b + Integer(2);
        if (modulus.is_zero()) {
            modulus = Integer(3);
        }
        const Integer reduced = a % modulus;
        require(reduced < modulus);

        const uint64_t exp = static_cast<uint64_t>(data[0] % 16);
        (void)gnfs::core::powmod(reduced + Integer(1), Integer(exp), modulus);

        const Integer g = gnfs::core::gcd(sum, product);
        require(!g.is_zero());
    } catch (const std::exception&) {
        return 0;
    }

    return 0;
}
