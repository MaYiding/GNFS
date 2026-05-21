# Bai-Brent Non-Monic Polynomial Selection

## Background

The Kleinjung 2008 polynomial selection algorithm produces polynomials of the
form `f(x) = a_d * x^d + ... + a_0`, where `a_d` is constrained to be a smooth
integer (a product of small primes ≤ 31). Smooth `a_d` permits a wider search
over translations and rotations because the smooth factors translate into
favourable behaviour of `f mod p` for small primes.

The Bai 2011 thesis (Section 2.3) generalises this by **removing the
smoothness constraint on `a_d`** and instead requiring only
`gcd(a_d, m) = 1`. This widens the Stage 1 search space and, in conjunction
with rotation, admits skewness ranges that the smooth-only enumeration cannot
reach.

## Algorithm

`BaiBrentSelector` (`include/gnfs/polynomial/bai_brent_selector.hpp`)
mirrors the structure of `KleinjungSelector`:

| Stage | Kleinjung | Bai-Brent |
|-------|-----------|-----------|
| 1: `a_d` search | smooth-only (`generate_smooth_coefficients`) | **arbitrary `a_d ∈ [ad_min, ad_max]`**, smooth-first, then linear sweep, with `gcd(a_d, m) = 1` enforced |
| 1: `m` near `m_est` | within `search_radius`, drop if `\|a_{d-1}\| > m` | identical |
| 2: translation + rotation | translate `t ∈ [-T, T]`, closed-form rotation, top-K L²/cheap-alpha ranking | identical (reused verbatim) |
| 2: Murphy E re-ranking | top-K finalists scored by full Murphy E | identical |

Stage 2 is reused without modification because the rotation
`f_new = f_old + k * (x - m)` preserves both `f(m) ≡ 0 (mod N)` and the
non-monic shape of `f`.

## ENV Gate

The selector is **OFF by default**. Enable via:

```bash
GNFS_POLY_BAI_BRENT=1 ./gnfs <N>
```

When `GNFS_POLY_BAI_BRENT=1`, `SelectorDispatch` routes degree-5 and
degree-6 selection through `BaiBrentSelector` first. On failure it falls
back to `KleinjungSelector`, then to `BaseMSelector`.

For degree 3 and 4, `BaseMSelector` remains the primary path; the ENV has
no effect.

## Why Default OFF

- The theoretical Murphy E gain depends heavily on N size and chosen
  parameters; for small N (≤ 50-digit) the Kleinjung baseline already
  saturates the achievable Murphy E.
- Stage 1 search is wider, so wall-clock cost is higher. Profiling per
  problem size is advised before enabling in production.
- All existing tests and pipelines see no change with the ENV unset.

## Testing

- `tests/test_bai_brent_poly.cpp` — 15 unit tests
  (`fast` tier, 60s timeout). Covers basic select, non-monic output,
  `gcd(a_d, m) = 1` invariant, `f(m) % N == 0`, degrees 4 / 5 / 6, Murphy
  score finiteness, cancellation, zero / negative N, dispatch ENV routing,
  candidate dedup.
- `tests/test_bai_brent_benchmark.cpp` — standalone (not in CTest)
  side-by-side Murphy E comparison vs Kleinjung. Run manually:
  ```bash
  ./build/test_bai_brent_benchmark
  ```

## Integration Points

- `include/gnfs/polynomial/bai_brent_selector.hpp` — selector + params + dispatch helper
- `include/gnfs/polynomial/selector_dispatch.hpp` — ENV gate `GNFS_POLY_BAI_BRENT`
- `src/api/pipeline.cpp` — no change required; `SelectorDispatch::select`
  is already the indirection point.

## Caveats

- For very small N (≤ 30-bit), `BaiBrentSelector` may produce the same
  output as `KleinjungSelector` (both pick `a_d = 1` because larger `a_d`
  values run out of valid `m` candidates).
- The `a_d` enumeration upper bound `ad_max` is currently derived from
  `GNFSParams::leading_coeff_bound`. Tuning per problem size has not been
  empirically optimised; the value is inherited from the Kleinjung
  defaults.
- Rotation-incremental alpha (CADO-NFS `ropt_stage2.cpp`) is not
  implemented — the same deferred work item applies to `BaiBrentSelector`
  as to `KleinjungSelector`.
