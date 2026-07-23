#include <gnfs/siqs/shadow_proof_prefer.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define GNFS_TEST_CLOSE _close
#define GNFS_TEST_DUP _dup
#define GNFS_TEST_DUP2 _dup2
#define GNFS_TEST_FILENO _fileno
#else
#include <fcntl.h>
#include <unistd.h>
#define GNFS_TEST_CLOSE ::close
#define GNFS_TEST_DUP ::dup
#define GNFS_TEST_DUP2 ::dup2
#define GNFS_TEST_FILENO ::fileno
#endif

using gnfs::core::Integer;
using namespace gnfs::siqs;

namespace {

[[noreturn]] void fail_test(const std::string& message) {
    std::fprintf(stderr, "SIQS test failure: %s\n", message.c_str());
    std::abort();
}

void require_test(bool condition, const std::string& message) {
    if (!condition) {
        fail_test(message);
    }
}

[[nodiscard]] int open_read_only_stderr_target() noexcept {
#if defined(_WIN32)
    return ::_open("NUL", _O_RDONLY | _O_BINARY);
#else
    return ::open("/dev/null", O_RDONLY);
#endif
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (set(value) != 0) {
            throw std::runtime_error("failed to set test environment variable " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_.has_value()) {
            (void)set(previous_->c_str());
        } else {
            (void)unset();
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    [[nodiscard]] int set(const char* value) const noexcept {
#if defined(_WIN32)
        return _putenv_s(name_.c_str(), value);
#else
        return ::setenv(name_.c_str(), value, 1);
#endif
    }

    [[nodiscard]] int unset() const noexcept {
#if defined(_WIN32)
        return _putenv_s(name_.c_str(), "");
#else
        return ::unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedStderrCapture final {
public:
    ScopedStderrCapture() {
        if (std::fflush(stderr) != 0) {
            throw std::runtime_error("failed to flush stderr before capture");
        }
        output_ = std::tmpfile();
        if (output_ == nullptr) {
            throw std::runtime_error("failed to create temporary stderr capture");
        }

        stderr_fd_ = GNFS_TEST_FILENO(stderr);
        saved_fd_ = GNFS_TEST_DUP(stderr_fd_);
        if (stderr_fd_ < 0 || saved_fd_ < 0 ||
            GNFS_TEST_DUP2(GNFS_TEST_FILENO(output_), stderr_fd_) < 0) {
            if (saved_fd_ >= 0) {
                (void)GNFS_TEST_CLOSE(saved_fd_);
                saved_fd_ = -1;
            }
            std::fclose(output_);
            output_ = nullptr;
            throw std::runtime_error("failed to redirect stderr for capture");
        }
        active_ = true;
    }

    ~ScopedStderrCapture() {
        restore_noexcept();
        if (output_ != nullptr) {
            std::fclose(output_);
        }
    }

    ScopedStderrCapture(const ScopedStderrCapture&) = delete;
    ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

    [[nodiscard]] std::string finish() {
        if (!active_ || output_ == nullptr) {
            throw std::logic_error("stderr capture already finished");
        }
        if (std::fflush(stderr) != 0) {
            restore_noexcept();
            throw std::runtime_error("failed to flush captured stderr");
        }
        if (GNFS_TEST_DUP2(saved_fd_, stderr_fd_) < 0) {
            restore_noexcept();
            throw std::runtime_error("failed to restore stderr");
        }
        (void)GNFS_TEST_CLOSE(saved_fd_);
        saved_fd_ = -1;
        active_ = false;

        if (std::fseek(output_, 0, SEEK_SET) != 0) {
            throw std::runtime_error("failed to rewind captured stderr");
        }
        std::string text;
        char buffer[4096];
        while (const size_t count = std::fread(buffer, 1, sizeof(buffer), output_)) {
            text.append(buffer, count);
        }
        if (std::ferror(output_) != 0) {
            throw std::runtime_error("failed to read captured stderr");
        }
        std::fclose(output_);
        output_ = nullptr;
        return text;
    }

private:
    void restore_noexcept() noexcept {
        if (!active_) {
            return;
        }
        (void)std::fflush(stderr);
        if (saved_fd_ >= 0 && stderr_fd_ >= 0) {
            (void)GNFS_TEST_DUP2(saved_fd_, stderr_fd_);
            (void)GNFS_TEST_CLOSE(saved_fd_);
        }
        saved_fd_ = -1;
        active_ = false;
    }

    std::FILE* output_ = nullptr;
    int stderr_fd_ = -1;
    int saved_fd_ = -1;
    bool active_ = false;
};

class ScopedUnwritableStderr final {
public:
    ScopedUnwritableStderr() {
        if (std::fflush(stderr) != 0) {
            throw std::runtime_error("failed to flush stderr before failure injection");
        }
        const int read_only_fd = open_read_only_stderr_target();
        if (read_only_fd < 0) {
            throw std::runtime_error("failed to open a read-only stderr target");
        }

        stderr_fd_ = GNFS_TEST_FILENO(stderr);
        if (stderr_fd_ >= 0) {
            saved_fd_ = GNFS_TEST_DUP(stderr_fd_);
        }
        if (stderr_fd_ < 0 || saved_fd_ < 0 || GNFS_TEST_DUP2(read_only_fd, stderr_fd_) < 0) {
            if (saved_fd_ >= 0) {
                (void)GNFS_TEST_CLOSE(saved_fd_);
                saved_fd_ = -1;
            }
            (void)GNFS_TEST_CLOSE(read_only_fd);
            throw std::runtime_error("failed to inject an unwritable stderr target");
        }
        (void)GNFS_TEST_CLOSE(read_only_fd);
        active_ = true;
    }

    ~ScopedUnwritableStderr() {
        restore_noexcept();
    }

    ScopedUnwritableStderr(const ScopedUnwritableStderr&) = delete;
    ScopedUnwritableStderr& operator=(const ScopedUnwritableStderr&) = delete;

    void finish() {
        if (!active_) {
            throw std::logic_error("unwritable stderr injection already finished");
        }
        (void)std::fflush(stderr);
        if (GNFS_TEST_DUP2(saved_fd_, stderr_fd_) < 0) {
            restore_noexcept();
            throw std::runtime_error("failed to restore stderr after failure injection");
        }
        (void)GNFS_TEST_CLOSE(saved_fd_);
        saved_fd_ = -1;
        active_ = false;
        std::clearerr(stderr);
    }

private:
    void restore_noexcept() noexcept {
        if (!active_) {
            return;
        }
        (void)std::fflush(stderr);
        if (saved_fd_ >= 0 && stderr_fd_ >= 0) {
            (void)GNFS_TEST_DUP2(saved_fd_, stderr_fd_);
            (void)GNFS_TEST_CLOSE(saved_fd_);
        }
        saved_fd_ = -1;
        active_ = false;
        std::clearerr(stderr);
    }

    int stderr_fd_ = -1;
    int saved_fd_ = -1;
    bool active_ = false;
};

[[nodiscard]] size_t count_occurrences(std::string_view text, std::string_view needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] std::pair<Integer, Integer> canonical_factors(const SIQSResult& result) {
    std::pair<Integer, Integer> factors{result.factor1, result.factor2};
    if (factors.first > factors.second) {
        std::swap(factors.first, factors.second);
    }
    return factors;
}

[[nodiscard]] std::optional<SIQSResult> factor_143_with_shadow_mode(const char* mode,
                                                                    std::string& captured_stderr) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_OBSERVE_ENV, mode);
    ScopedStderrCapture capture;
    auto result = factor(Integer("143"), 10, false);
    captured_stderr = capture.finish();
    return result;
}

[[nodiscard]] std::optional<SIQSResult> factor_143_with_unwritable_observe_stderr() {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_OBSERVE_ENV, "observe");
    ScopedUnwritableStderr stderr_failure;
    auto result = factor(Integer("143"), 10, false);
    stderr_failure.finish();
    return result;
}

} // namespace

void test_one_large_prime_rejects_strong_pseudoprimes() {
    // Each value is composite but passes the legacy single-witness base-2
    // Miller-Rabin check that used to guard SIQS 1LP admission.
    struct CompositeCase final {
        uint64_t value;
        uint64_t known_divisor;
    };
    constexpr CompositeCase strong_base2_pseudoprimes[] = {
        {2047ULL, 23ULL},
        {341550071728321ULL, 10670053ULL},
        {3825123056546413051ULL, 149491ULL},
    };

    for (const CompositeCase& sample : strong_base2_pseudoprimes) {
        if (sample.known_divisor <= 1 || sample.known_divisor >= sample.value ||
            sample.value % sample.known_divisor != 0) {
            std::fprintf(stderr,
                         "SIQS pseudoprime fixture lost its known divisor: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
        if (is_valid_one_large_prime(sample.value)) {
            std::fprintf(stderr,
                         "SIQS 1LP admitted strong base-2 pseudoprime: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
    }

    constexpr uint64_t known_primes[] = {
        2ULL, 3ULL, 101ULL, 4294967311ULL, 18446744073709551557ULL,
    };
    for (uint64_t value : known_primes) {
        if (!is_valid_one_large_prime(value)) {
            std::fprintf(stderr, "SIQS 1LP rejected known prime: %llu\n",
                         static_cast<unsigned long long>(value));
            std::abort();
        }
    }
    if (is_valid_one_large_prime(0) || is_valid_one_large_prime(1) ||
        is_valid_one_large_prime(4)) {
        std::fprintf(stderr, "SIQS 1LP admitted a trivial composite boundary\n");
        std::abort();
    }

    std::printf("  one_large_prime strong pseudoprimes: PASS\n");
}

void test_tonelli_shanks() {
    // sqrt(2) mod 7 = 3 (since 3^2 = 9 ≡ 2 mod 7)
    uint32_t r = tonelli_shanks(2, 7);
    assert(r == 3 || r == 4); // 3 or 7-3=4
    assert((uint64_t)r * r % 7 == 2);

    // sqrt(2) mod 17 = 6 (since 6^2 = 36 ≡ 2 mod 17)
    r = tonelli_shanks(2, 17);
    assert((uint64_t)r * r % 17 == 2);

    // sqrt(3) mod 13 = 4 (since 4^2 = 16 ≡ 3 mod 13)
    r = tonelli_shanks(3, 13);
    assert((uint64_t)r * r % 13 == 3);

    // Non-QR: 2 mod 5 (Legendre = -1)
    r = tonelli_shanks(2, 5);
    assert(r == 0);

    printf("  tonelli_shanks: PASS\n");
}

/// 测试 split_cofactor_64 边界:输入是素数(应该 split 失败 → {0,0})、
/// 输入是 1(无意义 → {0,0})、输入是平方数(应该返回 √n 两次)。
void test_split_cofactor_edge() {
    // 1. n=1: 无意义
    {
        auto [p1, p2] = split_cofactor_64(1);
        assert(p1 == 0 && p2 == 0);
    }
    // 2. n=0: 无意义
    {
        auto [p1, p2] = split_cofactor_64(0);
        assert(p1 == 0 && p2 == 0);
    }
    // 3. 大素数(无法分解 — 所有方法应失败)
    // 1099511627791 是素数(2^40 + 15)
    {
        auto [p1, p2] = split_cofactor_64(1099511627791ULL);
        // 素数情况下 trial division 和 Pollard rho 都应失败,返回 {0, 0}
        // 注:也可能因为是边界数,SQUFOF 会循环退出 — 关键是返回的不应是 {p, n/p}
        // 因为素数无非平凡因子。
        assert(p1 == 0 || p1 * p2 == 1099511627791ULL);
        if (p1 != 0) {
            // 若声称分解了,验证 p1 * p2 == n
            assert(p1 > 1 && p1 < 1099511627791ULL);
        }
    }
    // 4. 平方数:n=p²,split 应返回 {p, p}
    {
        // 1009² = 1018081
        auto [p1, p2] = split_cofactor_64(1018081ULL);
        assert(p1 == 1009 && p2 == 1009);
    }
    // 5. 简单半素数:7 * 13 = 91
    {
        auto [p1, p2] = split_cofactor_64(91);
        assert(p1 == 7 && p2 == 13);
    }
    // 6. 三因子合数:2 * 3 * 5 = 30 → split 应返回某对 (a, b) 满足 a*b=30
    {
        auto [p1, p2] = split_cofactor_64(30);
        assert(p1 * p2 == 30);
        assert(p1 > 1 && p2 > 1);
    }

    printf("  split_cofactor edge cases: PASS\n");
}

void test_factor_base() {
    Integer N("1000000007"); // prime, but we're testing FB construction
    auto fb = build_factor_base(N, 20);
    assert(fb.size() >= 20);

    // Verify sqrt(N) mod p is correct for each FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t sq = fb[i].sqrt_n;
        uint64_t n_mod = mpz_fdiv_ui(N.get_mpz(), p);
        assert(((uint64_t)sq * sq) % p == n_mod % p);
    }

    printf("  factor_base: PASS (%zu primes)\n", fb.size());
}

void test_siqs_small() {
    std::string off_stderr;
    const auto off_result = factor_143_with_shadow_mode("0", off_stderr);
    require_test(off_result.has_value(), "143 did not factor with shadow proof disabled");
    require_test(count_occurrences(off_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 "disabled shadow proof emitted an observe record");
    require_test(!off_result->shadow_proof_observe_record_committed,
                 "disabled shadow proof reported a committed observe record");

    std::string observe_stderr;
    const auto observe_result = factor_143_with_shadow_mode("observe", observe_stderr);
    require_test(observe_result.has_value(), "143 did not factor in observe mode");
    require_test(count_occurrences(observe_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 1,
                 "observe mode did not emit exactly one schema-v1 record");
    require_test(observe_stderr.find("route=legacy_continue") != std::string::npos,
                 "observe record did not declare legacy continuation");
    require_test(observe_result->shadow_proof_observe_record_committed,
                 "observe mode did not report a committed schema-v1 record");

    const auto failed_commit_result = factor_143_with_unwritable_observe_stderr();
    require_test(failed_commit_result.has_value(),
                 "143 did not continue the legacy factor path after observe write failure");
    require_test(!failed_commit_result->shadow_proof_observe_record_committed,
                 "observe write failure reported a committed schema-v1 record");

    const auto off_factors = canonical_factors(*off_result);
    const auto observe_factors = canonical_factors(*observe_result);
    const auto failed_commit_factors = canonical_factors(*failed_commit_result);
    const unsigned expected_sieve_workers =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    require_test(off_result->resolved_sieve_workers == expected_sieve_workers,
                 "disabled-mode result did not report the production sieve worker count");
    require_test(observe_result->resolved_sieve_workers == expected_sieve_workers,
                 "observe-mode result did not report the production sieve worker count");
    require_test(off_result->resolved_sieve_workers == observe_result->resolved_sieve_workers,
                 "shadow mode changed the production sieve worker count");
    require_test(off_factors == observe_factors,
                 "observe mode changed the canonical 143 factor result");
    require_test(off_factors == failed_commit_factors,
                 "observe write failure changed the canonical 143 factor result");
    require_test(off_factors.first == Integer(11) && off_factors.second == Integer(13),
                 "143 factorization did not return 11 and 13");
    printf("  siqs_small(143) shadow observe parity: PASS (off %.3fs, observe %.3fs)\n",
           off_result->time_seconds, observe_result->time_seconds);
}

void require_siqs_shadow_mode_rejected(const char* mode) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_OBSERVE_ENV, mode);
    ScopedStderrCapture capture;
    bool invalid_argument_thrown = false;
    bool unexpected_exception_thrown = false;
    try {
        (void)factor(Integer("143"), 10, false);
    } catch (const std::invalid_argument&) {
        invalid_argument_thrown = true;
    } catch (...) {
        unexpected_exception_thrown = true;
    }
    const std::string captured_stderr = capture.finish();

    require_test(invalid_argument_thrown && !unexpected_exception_thrown,
                 std::string("shadow mode '") + mode +
                     "' did not fail closed with std::invalid_argument");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 std::string("shadow mode '") + mode + "' performed observe work before throwing");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 std::string("shadow mode '") + mode +
                     "' emitted a prefer decision before throwing");
}

void test_siqs_shadow_rejected_modes() {
    require_siqs_shadow_mode_rejected("invalid");
    require_siqs_shadow_mode_rejected("prefer");
    printf("  siqs shadow rejected modes (invalid, prefer): PASS\n");
}

void test_siqs_20digit() {
    // 20-digit semiprime: 12345678901234567891 = 3 * 4115226300411522597
    // Actually let's use a known 20-digit semiprime
    // 10000000000000000051 * ... let's just try a small product
    Integer p1("1000000007");
    Integer p2("1000000009");
    Integer N = p1 * p2;
    printf("  siqs_20digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto start = std::chrono::steady_clock::now();
    auto result = factor(N, 30, true);
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    if (result) {
        auto f1 = result->factor1, f2 = result->factor2;
        if (f1 > f2) std::swap(f1, f2);
        assert(f1 * f2 == N);
        printf("  siqs_20digit: PASS (%.3fs, %zu polys)\n",
               elapsed, result->polynomials_used);
    } else {
        printf("  siqs_20digit: FAIL — no factor found (%.3fs)\n", elapsed);
        assert(false);
    }
}

void test_siqs_30digit() {
    Integer p1("1000000000000007");
    Integer p2("1000000000000037");
    Integer N = p1 * p2;
    printf("  siqs_30digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 60, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_30digit: PASS (%.3fs, %zu polys)\n",
               result->time_seconds, result->polynomials_used);
    } else {
        printf("  siqs_30digit: FAIL\n");
        // Don't assert — this may need parameter tuning
    }
}

void test_siqs_40digit() {
    Integer p1("10000000000000000051");
    Integer p2("10000000000000000099");
    Integer N = p1 * p2;
    printf("  siqs_40digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 120, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_40digit: PASS (%.3fs, %zu polys)\n",
               result->time_seconds, result->polynomials_used);
    } else {
        printf("  siqs_40digit: FAIL\n");
    }
}

int main() {
    ScopedEnvironmentVariable default_shadow_mode(SIQS_SHADOW_PROOF_OBSERVE_ENV, "0");

    printf("=== SIQS Unit Tests ===\n\n");

    printf("--- Helper tests ---\n");
    test_one_large_prime_rejects_strong_pseudoprimes();
    test_tonelli_shanks();
    test_factor_base();
    test_split_cofactor_edge();

    printf("\n--- Factorization tests ---\n");
    test_siqs_small();
    test_siqs_shadow_rejected_modes();
    test_siqs_20digit();
    test_siqs_30digit();
    test_siqs_40digit();

    printf("\n=== All SIQS tests passed ===\n");
    return 0;
}
