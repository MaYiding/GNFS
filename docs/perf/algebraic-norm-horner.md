# Homogeneous Horner Evaluation for Algebraic Norms

`PolynomialContext::algebraic_norm(a, b)` evaluates the signed homogeneous
polynomial value used by the General Number Field Sieve (GNFS):

$$
H(a,b) = \sum_{k=0}^{d} f_k a^k b^{d-k}.
$$

The implementation uses homogeneous Horner evaluation to avoid materializing
both sets of powers and their term products. It preserves the existing
`a - b*alpha` convention, including negative coordinates, non-monic
polynomials, and `b = 0`. The native `algebraic_norm_i128` path and cofactor
dispatch are unchanged.

## Recurrence and Boundaries

Initialize `R = f_d` and `B = 1`. For `i = d, ..., 1`, compute:

```text
B = B * b
R = R * a + f_(i-1) * B
```

After incorporating coefficient `i`, the invariant is:

$$
B = b^{d-i}, \qquad
R = \sum_{k=i}^{d} f_k a^{k-i} b^{d-k}.
$$

At `i = 0`, this gives `H(a,b)` without division. For `b = 0`, the
recurrence leaves `f_d * a^d`; a constant polynomial returns its constant.
The initial coefficient comes from the effective degree because the context
retains trailing zero coefficients. Empty contexts return zero before any
coefficient access.

The result owns its storage. One thread-local integer holds `B` and is reset
on each call. GMP accepts `long` and `unsigned long` operands in its scalar
multiplication functions. These types are 32 bits on Windows LLP64, so wide
coordinates are converted to `Integer` once per call and reused with
`mpz_mul`. The signed minimum coordinate never requires native negation.

## Reproducible Kernel Experiment

The standalone [benchmark](../../bench/microbench/algebraic_norm_horner.cpp)
compares the frozen implementation from commit
`ca8e3c56820c8e3cdba65a5fb3d84fe968394dbb` with the public production method
compiled from the current header. The legacy body only replaces private
member reads with their public accessors. Both implementations have matching
`noinline` wrappers and share the same timing loop, result digest, and
deadline checks. The measurement includes these common costs and result
destruction.

Each band has 256 fixed-seed synthetic coprime coordinate pairs with
alternating signs. The integers come from repository test cases; the
polynomials use reproducible balanced base-m expansion with
`m = floor(N^(1/d))`. They are not captured sieve coordinates or Kleinjung
selection outputs, and the experiment does not assess irreducibility or
polynomial quality.

| Band | Repository Source | N Bits | Degree | Coordinate Bounds |
|---|---|---:|---:|---|
| `gate81` | `tests/test_regression_gate.cpp` | 81 | 3 | `abs(a) < 2^20`, `b <= 2^16` |
| `kleinjung130` | `tests/test_kleinjung_large.cpp` | 130 | 4 | `abs(a) < 2^24`, `b <= 2^20` |
| `stress50d` | `tests/test_stress.cpp` | 164 | 4 | `abs(a) < 2^28`, `b <= 2^24` |
| `stress60d` | `tests/test_stress.cpp` | 197 | 5 | `abs(a) < 2^28`, `b <= 2^24` |

Raw GMP multiplication verifies each factor pair. An independent power-and-sum
oracle verifies `f(m) = N` when constructing each polynomial. Before each
measured pair, it checks every timed coordinate and 90 boundary coordinate
pairs. Separate fixtures cover effective degrees
zero, one, three, eight, and nine, signed and wide coefficients, zero
polynomials, and trailing zeros. The legacy comparison excludes empty
contexts. Every result limb and sign contributes to the printed digest;
both implementations must produce equal measured digests.

After warmup, seven pairs alternate the order of the two implementations.
The command below requests 1,048,576 calls per implementation per pair.
The cooperative budget is five seconds per band, including correctness
checks and warmup. Deadline checks occur every 256 timed calls. An incomplete
pair is discarded, its band receives no median, and the program exits with
status 2. Use an external timeout to bound a blocked stream or individual GMP
operation.

## Build and Run

First prepare a Release core library through the project runner. The
standalone probe is an explicit performance experiment rather than a new
regular test target. Compile it against the candidate header, since the old
method is frozen inside the benchmark source. Set `GMP_PREFIX` to the local
GMP installation and `CANDIDATE_REVISION` to the candidate commit or source
manifest identifier.

```bash
GNFS_BUILD_DIR=build-release ./scripts/test.sh run test_polynomial_context -t Release -j 2
c++ -std=c++20 -O3 -DNDEBUG -Iinclude -isystem "${GMP_PREFIX}/include" \
  "-DGNFS_NORM_BENCH_REVISION=\"${CANDIDATE_REVISION}\"" \
  bench/microbench/algebraic_norm_horner.cpp build-release/libgnfs_core.a \
  -L"${GMP_PREFIX}/lib" -lgmp -pthread -o build-release/algebraic_norm_horner
build-release/gnfs_test_process_supervisor --timeout-ms 30000 -- \
  "${PWD}/build-release/algebraic_norm_horner" --rounds 7 --calls 1048576 --band-ms 5000
```

Record the compiler, flags, GMP version, architecture, source and header
hashes, and complete output. On platforms with different library naming or
link conventions, adapt the compiler command while preserving the two
wrappers and optimization settings. Do not run competing builds during the
measurement.

## Recorded Production Run

The production comparison completed on November 11, 2026, using an Apple M5
running macOS arm64, Apple Clang 21.0.0, and GMP 6.3.0. The compilation used
`-std=c++20 -O3 -DNDEBUG -mcpu=native`, the Release core library, and matching
optimization settings for both wrappers. All seven pairs completed in every
band; all 28 measured digest pairs matched. No competing build or test from
this experiment ran during measurement. The process took 8.33 seconds.

Ratios below divide legacy time by candidate time. Each band has 256 native
dispatch probes; the timed calls always invoke the GMP norm kernel.

| Band | Maximum Norm Bits | GMP Fallback Probes | Median Ratio | Ratio Range |
|---|---:|---:|---:|---|
| `gate81` | 77 | 0/256 | 1.725607 | 1.649322–1.766959 |
| `kleinjung130` | 119 | 134/256 | 1.605916 | 1.580400–1.707708 |
| `stress50d` | 134 | 256/256 | 1.759050 | 1.716830–1.816466 |
| `stress60d` | 170 | 256/256 | 1.703044 | 1.684776–1.784486 |

The candidate was measured on base commit
`10efbc800cbba4210911b76f2254882259dd4baa` with the source changes in this
document's accompanying implementation. SHA-256 fingerprints identify the
exact benchmark and candidate content independently of the base commit:

```text
benchmark source:
37603bfeaa9fe2c90503d7de942f1781798851f71e8df97f44e736f47f8024e8
candidate include/gnfs/core/polynomial_context.hpp:
8d26654696bea8bc75124900f23bd20d7841b70ff3a3e8e963ac9f81b1e8f18a
legacy include/gnfs/core/polynomial_context.hpp at ca8e3c56:
b8a7f08a01f34cc174da13816e99a15b6029a19c66ffce958eafe5f7b698f895
```

The registered norm regression passed against the old implementation in
Release, then against the replacement in Release, Debug, and combined
AddressSanitizer/UndefinedBehaviorSanitizer builds. The regression source was
identical across those runs. Actual Release compile and link commands
included `-DNDEBUG`.

On base `ca8e3c56`, `changed --deep` passed 167 tests with one Windows-only
test skipped on macOS, and `gate` passed all 192 entries. The initial
10-minute wrapper expired after building the targets and completing 73
tests, without a failure. The completed deep run reused those binaries with
`--no-build` and a 30-minute outer limit, finishing in 381 seconds. After
integrating `10efbc80`, the unchanged candidate sources passed the norm
regression, all five factor-base module tests, and the 17/27/40/81-bit
regression gate again. Selected integration targets were prepared with
`cmake --build ... -j 2 --target ...` before running the registered
`module factor_base` and `run test_regression_gate` commands with
`--no-build`.

## Scope of the Evidence

These measurements concern the GMP norm kernel. Cofactorization first tries
`algebraic_norm_i128` where the compiler supports it; native hits bypass this
function. The program prints native hit and fallback counts for its synthetic
sample set. Those fractions are not production traffic measurements and
cannot establish a pipeline speedup. Platforms without native i128 use the
GMP path, but performance still requires measurement on those platforms.

The registered `test_polynomial_context` regression compares the public
method with an independent full-width GMP oracle at 100 coordinate/degree
combinations. It also checks the sign of `(a-2b)(a+3b)`, trailing zeros,
result ownership across subsequent calls, and concurrent callers. Its new
expectations remain active under `NDEBUG`. The thread fixture is regression
coverage rather than an exhaustive proof of all schedules.
