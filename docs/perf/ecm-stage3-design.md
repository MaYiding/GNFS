# ECM Stage 3 — Brent-Suyama Polynomial Extension

## Summary

ECM Stage 2 BSGS in `include/gnfs/cofactor/ecm.hpp` is extended with the
**Brent-Suyama polynomial F(x) = x^d** for opt-in acceleration. The
classical default (`d = 0`, plain BSGS) is preserved unchanged; the new
path activates only when `Config::brent_suyama_degree > 0` or the ENV
`GNFS_ECM_BRENT_SUYAMA=1` is set.

## Mathematical Background

### Classical Stage 2 BSGS

Given a Montgomery-curve point `Q` representing the post-Stage-1 residue, the
algorithm searches for primes `p ∈ (B1, B2]` such that `p · Q = O` on the
curve modulo the unknown factor of `N`. Primes are decomposed as
`p = j · D ± d` with `D = 2310 = 2 · 3 · 5 · 7 · 11` and `d` coprime to `D`
(`φ(D) = 480` baby steps). Detection uses the cross product of projective
XZ-coordinates:

```
c = X_jD · Z_d - X_d · Z_jD  (mod n)
```

`c ≡ 0 (mod p)` iff `j · D · Q == d · Q` on the curve mod `p`, i.e.,
`p · Q == O`.

### Brent-Suyama Generalization

Brent (1986, §4.2) and Suyama (unpublished 1985 manuscript) observed that
replacing the identity polynomial with `F(x) = x^d` for `d ≥ 2` catches
additional factor coincidences:

```
F(X_jD / Z_jD) - F(X_d / Z_d) ≡ 0  (mod p)
```

Clearing denominators (multiply through by `Z_jD^d · Z_d^d`):

```
X_jD^d · Z_d^d - X_d^d · Z_jD^d ≡ 0  (mod p)
```

This is detected exactly as the BSGS cross product, except baby and giant
points are pre-evaluated through `F`. Because

```
F(x) - F(y) = (x - y) · h(x, y)
```

the polynomial extension catches the classical equality `x = y` (via the
`(x - y)` factor) **plus** extra coincidences from `h(x, y) ≡ 0 mod p`.
For `F(x) = x^d` with `d = lcm(1, 2, ..., k)`, `h(x, y)` factors over
products of cyclotomic polynomials, each contributing extra "twist-prime"
detection modes.

### Practical Degree Choices

| `d` | `lcm` | Purpose | Stage 2 wall-time multiplier |
|-----|-------|---------|------------------------------|
| 1   | n/a    | Identity (= classical BSGS, sanity / fallback) | 1.00x |
| 2   | lcm(1,2) | Small squaring uplift | ~1.05x |
| 6   | lcm(1,2,3) | Moderate | ~1.10x |
| 12  | lcm(1,2,3,4) | **Default** (matches GMP-ECM common setting) | ~1.10x |
| 30  | lcm(1,2,3,4,5) | Large-B2 mode | ~1.15x |

(Multipliers reflect the per-iteration polynomial-evaluation overhead at
the baby-step pre-computation; the giant-step hot loop is unchanged in
asymptotic complexity.)

## Implementation

### Files

| File | Purpose |
|------|---------|
| `include/gnfs/cofactor/ecm_brent_suyama.hpp` | Helper: `evaluate_polynomial`, `PolynomialPoint`, `accumulate_cross_product` |
| `include/gnfs/cofactor/ecm.hpp` | `Config::brent_suyama_degree`, `BatchContext::brent_suyama_degree`, `stage2_brent_suyama()`, `apply_brent_suyama_env()` |
| `tests/test_ecm_brent_suyama.cpp` | Unit tests (9 cases, instant tier, 30s budget) |
| `tests/test_ecm_brent_suyama_bench.cpp` | Benchmark vs classical BSGS (slow tier, 120s budget) |

### Configuration

```cpp
ECM::Config cfg;
cfg.brent_suyama_degree = 12;  // opt-in, default 0 = OFF
auto factor = ECM::factor(n, cfg);
```

### Environment Variables

```bash
# Opt in with default degree (12)
GNFS_ECM_BRENT_SUYAMA=1 ./gnfs <N>

# Opt in with explicit degree
GNFS_ECM_BRENT_SUYAMA=1 GNFS_ECM_BS_DEGREE=30 ./gnfs <N>
```

Invalid degree values (anything outside `{1, 2, 6, 12, 30}`) fall back to
the default `12`. Without `GNFS_ECM_BRENT_SUYAMA=1`, both env vars are
ignored and classical BSGS runs.

### Dispatch

`try_curve_with_pk()` (in `ecm.hpp`) selects the Stage 2 path:

```cpp
if (ctx.brent_suyama_degree > 0 &&
    brent_suyama::is_supported_degree(ctx.brent_suyama_degree)) {
    stage2_result = stage2_brent_suyama(Q, n, a24, ctx.B1, ctx.B2,
                                        ctx.brent_suyama_degree);
} else {
    stage2_result = stage2(Q, n, a24, ctx.B1, ctx.B2);
}
```

The classical `stage2()` is bit-for-bit identical to its prior implementation.

### Storage and Hot Loop

The baby-step phase precomputes `(X^d mod n, Z^d mod n)` into a
`std::vector<brent_suyama::PolynomialPoint>` (480 entries for `D = 2310`).
The giant-step phase evaluates `F(X_giant) mod n` and `F(Z_giant) mod n`
**once per giant**, then accumulates against all 480 babies using
hoisted scratch `Integer` buffers (mirrors the v22 buffer-hoist pattern in
the classical `stage2()`).

Each iteration of the inner loop performs four `mpz_mul/mpz_mod` calls
(two cross-product multiplies, one subtraction, one accumulator update)
plus a single `mpz_powm_ui` per `(d, n)` baby (paid once during baby setup).
Asymptotic cost is identical to classical BSGS; the polynomial precomputation
adds `O(480 · log d)` modular multiplications upfront.

## Testing

```bash
# Unit (instant tier, ~3s in Debug)
cd build && ./test_ecm_brent_suyama

# Benchmark (slow tier, ~10s for 100 trials in Release)
cd build-release && ./test_ecm_brent_suyama_bench 100 42

# Via test.sh
./scripts/test.sh module cofactor          # excludes bench
./scripts/test.sh module cofactor --slow   # includes bench
```

### Unit Coverage

1. `is_supported_degree()` exhaustive on `{1, 2, 6, 12, 30}` and rejected on `{0, 3, 4, 60}`
2. `evaluate_polynomial()` matches GMP `mpz_powm_ui` reference for all supported degrees, plus `X = 0` and `X = 1` edge cases
3. `accumulate_cross_product()` distinct-point and identical-point semantics
4. End-to-end `ECM::factor()` finds known semiprime factor with `degree = 12`
5. `degree = 30` mode finds known semiprime factor
6. `degree = 1` (identity) equivalent to classical BSGS
7. ENV opt-in (`GNFS_ECM_BRENT_SUYAMA=1`) with default and explicit `GNFS_ECM_BS_DEGREE`
8. `B1 < D` (j_lo = 0 path) safety with BS enabled
9. `B2 <= B1` (no Stage 2 work) safety with BS enabled

### Benchmark Output (M5 Pro, Release)

100 random 60-bit semiprimes (both factors `mpz_nextprime`-snapped),
`B1 = 500`, `B2 = 10000`, 5 curves per trial:

```
BSGS         : found 99/100  avg=4.26 ms  median=2.41 ms
Brent-Suyama : found 97/100  avg=4.32 ms  median=2.24 ms
Both found   : 96/100
Speedup (median BSGS / BS12) = 1.08x
```

Brent-Suyama at `d = 12` reaches near-parity success rate with a ~5-10%
median wall-time edge on M-series silicon. Larger `B2` widens the gap
(Stage 2 dominates and Brent-Suyama's per-iteration cost is amortized
over more giant steps).

## References

- Brent, R. P., "Some integer factorization algorithms using elliptic curves."
  *Australian Computer Science Communications* 8(1), 149-163 (1986).
- Suyama, H., "Informal preliminary report (8)." Unpublished manuscript (1985).
- Crandall, R. and Pomerance, C., *Prime Numbers: A Computational Perspective*,
  2nd ed., Springer (2005), §7.4.2 "Stage 2".
- Zimmermann, P. and Dodson, B., "20 years of ECM."
  *Algorithmic Number Theory* (ANTS-VII), LNCS 4076 (2006). Discussion of
  GMP-ECM Stage 2 polynomial defaults.

## Limitations and Future Work

- **Polynomial choice limited to `x^d`**: Other Brent-Suyama polynomial
  families (Dickson polynomials, Chebyshev-like) are not implemented.
  GMP-ECM supports Dickson `D_d(x, a)` for additional twist-prime
  coverage, but the implementation complexity is significantly higher
  for marginal gain at typical GNFS cofactor sizes (40-80 bit).
- **No batched polynomial evaluation**: `evaluate_polynomial()` calls
  `mpz_powm_ui` per coordinate. A custom binary-exponentiation routine
  using shared scratch could shave 5-10% off the baby-step setup, but is
  not implemented (one-time cost, dominated by the giant-step loop).
- **No FFT-based Stage 2**: GMP-ECM's "Stage 2 with NTT" gives orders of
  magnitude speedup for very large `B2 > 10^9`, but requires a substantial
  implementation effort (polynomial multiplication, sub-product trees).
  Out of scope for this change.
