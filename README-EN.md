> **English** | [中文](README.md)

# GNFS — General Number Field Sieve

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/tests-42%20files-brightgreen.svg)]()
[![LoC](https://img.shields.io/badge/lines%20of%20code-~41K-informational.svg)]()

Industrial-grade **General Number Field Sieve** implementation in C++20.

GNFS is the most powerful known classical algorithm for factoring large composite integers — and the algorithm behind record-breaking factorizations of RSA numbers. This project implements the **complete GNFS pipeline** from polynomial selection through square root extraction, as a high-performance C++ library and CLI tool with ~41,000 lines of code across 53 headers, 14 source files, and 41 test files.

## Table of Contents

- [Highlights](#highlights)
- [Factorization Results](#factorization-results)
- [Quick Start](#quick-start)
- [CLI Usage](#cli-usage)
- [C++ API](#c-api)
- [Architecture](#architecture)
- [Pipeline — 8 Stages](#pipeline--8-stages)
- [Key Algorithms](#key-algorithms)
- [Performance Optimizations](#performance-optimizations)
- [Testing](#testing)
- [Build Options](#build-options)
- [Dependencies](#dependencies)
- [Design Conventions](#design-conventions)
- [Project Structure](#project-structure)
- [Contributing](#contributing)
- [References](#references)
- [License](#license)

## Highlights

- **Multi-method auto-selection**: Automatically picks the optimal algorithm by N size — Trial Division (≤6d) → Pollard Rho (7-24d) → SIQS (25-100d) → GNFS (100d+), with `--method` manual override
- **Complete 8-stage GNFS pipeline**: Polynomial Selection → Factor Base → Sieving → Cofactorization → Relation Collection → Linear Algebra → Square Root → GCD — fully implemented and integrated
- **Verified up to 65 decimal digits (213-bit)**: 65d balanced semiprime in 24s (Release), with progressive test suite + regression gate + 42 test files
- **High performance**: Multi-threaded lattice sieve + bucket sieve, parallel Block Lanczos / Block Wiedemann, Nguyen Hybrid square root (200x speedup)
- **SIQS engine**: Contini self-initializing quadratic sieve — 25d=0.17s, 49d=0.9s, 59d=6.9s, 65d=24s (Release, Apple M1)
- **Dual solver**: Both Block Lanczos and Block Wiedemann GF(2) null-space solvers, with SGE preprocessing and Gaussian fallback
- **Out-of-core support**: mmap-backed CSR matrix and relation storage for memory-constrained large factorizations
- **Multiple cofactorization strategies**: Trial division + SQUFOF (2-word cofactors) + ECM (Montgomery curves, Stage 1+2 BSGS) + smoothness verification
- **Adaptive parameters**: CADO-NFS calibrated parameter tables + L_N heuristic + empirical fine-tuning; Kleinjung adaptive scaling for large inputs
- **Unified CLI**: `./gnfs <number>` one-command factorization, `--method` selection, bilingual UI (Chinese/English), JSON/CSV/report output
- **Clean C++ API**: High-level `factorize()` one-liner + step-by-step `Pipeline` class + method selection engine

## Factorization Results

> Benchmarks in **Release mode** on Apple M1 with **balanced semiprimes**.
> Auto method selection: Trial (≤6d) → Pollard Rho (7-24d) → SIQS (25-100d) → GNFS (100d+)

### By Method

**Trial Division** — O(10^6) divisions, <1ms

| Digits | N (example) | Bits | Time | Notes |
|:------:|-------------|:----:|-----:|-------|
| 3 | 143 = 11 × 13 | 8 | <1ms | Instant |
| 6 | 96091 = 307 × 313 | 17 | <1ms | Instant |
| 10 | 1000036099 = 31 × 32259229 | 30 | <1ms | Small factor |

**Pollard Rho** — O(p^{1/2}) ≈ O(N^{1/4}) for balanced semiprimes

| Digits | N (example) | Bits | Time | Notes |
|:------:|-------------|:----:|-----:|-------|
| 13 | 1000036000099 | 40 | **1.6ms** | Balanced |
| 15 | 100000980001501 | 47 | **1.3ms** | Balanced |
| 19 | 1253371692427905599 | 61 | **2.7ms** | Balanced |
| 20 | 9869605258179967459 | 64 | **3.5ms** | Balanced |
| 22 | 986960447655171329111 | 70 | **14ms** | Balanced |

**SIQS (Quadratic Sieve)** — O(L_N(1/2, 1)), optimal for 25-100d

| Digits | Bits | Time | Polynomials | Notes |
|:------:|:----:|-----:|------------:|-------|
| 25 | 81 | **0.17s** | 411 | |
| 33 | 109 | **0.51s** | 811 | |
| 39 | 127 | **0.30s** | 2,012 | |
| 45 | 147 | **0.72s** | 2,012 | |
| 49 | 160 | **0.90s** | 9,211 | |
| 55 | 180 | **3.8s** | 32,012 | |
| 59 | 193 | **6.9s** | 97,611 | |
| 65 | 213 | **24s** | 106,011 | |

**GNFS (Number Field Sieve)** — O(L_N(1/3, c)), for 100d+ or SIQS fallback

| Digits | Bits | Time | Notes |
|:------:|:----:|-----:|-------|
| 34 | 116 | ~8.9min | Debug mode |
| 45 | 147 | ~20min | Debug (SIQS is 24× faster) |
| 50 | 164 | ~2.6h | Stress test |

### Method Selection Logic

```
Input N
  │
  ├─ Factor ≤ 10^6 ?  ─── Trial Division ───→ <1ms
  │
  ├─ ≤ 24d (80 bit) ?  ─── Pollard Rho ───→ 1-14ms
  │
  ├─ 25-100d ?  ─── SIQS ───→ 0.17s ~ minutes
  │
  └─ > 100d ?  ─── GNFS ───→ hours ~ days
```

Use `--method` to override: `./build/gnfs 12345 --method siqs`

## Quick Start

### Prerequisites

- **C++20** compiler (Clang 14+ or GCC 12+)
- **CMake** 3.20+
- **GMP** (GNU Multiple Precision Arithmetic Library) — required

**Optional**: NTL (Number Theory Library), Metal (macOS GPU framework)

```bash
# macOS
brew install gmp cmake

# Ubuntu / Debian
sudo apt install libgmp-dev cmake build-essential

# Fedora / RHEL
sudo dnf install gmp-devel cmake gcc-c++

# Arch Linux
sudo pacman -S gmp cmake
```

### Build

```bash
git clone <repo-url> && cd GNFS

# Configure & build (Release for optimal performance)
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Run smoke tests (~5s, 23 instant tests)
./scripts/test.sh
```

### Factorize a Number

```bash
# One-command factorization (auto-selects best method)
./build/gnfs 96091

# Specify factorization method
./build/gnfs 1000036000099 --method siqs    # Force SIQS
./build/gnfs 1000036000099 --method rho     # Force Pollard rho
./build/gnfs 1000036000099 --method gnfs    # Force GNFS

# With JSON output
./build/gnfs 1000036000099 --json

# Detailed report saved to file
./build/gnfs 1000036000099 --report -o result.txt

# English UI
./build/gnfs 96091 --lang en

# Interactive REPL mode
./build/gnfs --interactive
```

## CLI Usage

### Terminal Output Example

```
   ╔══════════════════════════════════════╗
   ║   ██████╗ ███╗   ██╗███████╗███████╗ ║
   ║  ██╔════╝ ████╗  ██║██╔════╝██╔════╝ ║
   ║  ██║  ███╗██╔██╗ ██║█████╗  ███████╗ ║
   ║  ██║   ██║██║╚██╗██║██╔══╝  ╚════██║ ║
   ║  ╚██████╔╝██║ ╚████║██║     ███████║ ║
   ║   ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚══════╝ ║
   ╚══════════════════════════════════════╝
   General Number Field Sieve v0.1.0

Factoring: 96091 (17 bits)

   ✓ Factor Base                               [12ms]
   ✓ Sieving                                   [1.35s]
   ✓ Filtering                                 [<1ms]
   ✓ Linear Algebra                            [14ms]
   ✓ Square Root                               [6ms]

   ╔══════════════════════════════════════════════════╗
   ║  FACTORIZATION SUCCESSFUL                        ║
   ╠══════════════════════════════════════════════════╣
   ║  N = 96091                                       ║
   ║      17 bits, 5 digits                           ║
   ║                                                  ║
   ║  = 307 × 313                                     ║
   ╠══════════════════════════════════════════════════╣
   ║  ├─ Polynomial           <1ms    0.0%            ║
   ║  ├─ Factor Base          12ms    0.9%            ║
   ║  ├─ Sieving             1.35s   97.0%            ║
   ║  ├─ Filtering            <1ms    0.0%            ║
   ║  ├─ Linear Algebra       14ms    1.0%            ║
   ║  └─ Square Root           6ms    0.4%            ║
   ║                           ____________           ║
   ║      TOTAL                       1.39s           ║
   ╠══════════════════════════════════════════════════╣
   ║  Rels: 512  Matrix: 512×313  Deps: 64            ║
   ╚══════════════════════════════════════════════════╝
```

### All CLI Options

```
Usage: gnfs <number> [options]

Arguments:
  <number>                         Composite integer to factorize

Output options:
  --json                           Output result as JSON
  --csv                            Output result as CSV
  --report                         Output detailed report with statistics
  -o, --output <file>              Write output to file instead of stdout
  --verbose                        Enable verbose logging with structured log
  --quiet                          Minimal output (result only)
  --no-color                       Disable ANSI color codes

Method selection:
  --method <method>                Factorization method (auto/trial/rho/siqs/gnfs, default: auto)

Language:
  --lang <zh|en>                   UI language (default: zh = Chinese)

Parameter overrides:
  --degree <d>                     Polynomial degree (auto-selected if omitted)
  --fb-rational <n>                Rational factor base bound
  --fb-algebraic <n>               Algebraic factor base bound
  --large-prime <n>                Large prime bound
  --sieve-width <n>                Sieve region width
  --sieve-height <n>               Sieve region height
  --threads <n>                    Worker thread count

Config file:
  -c, --config <file>              Load parameters from key=value config file

Other:
  --interactive                    Interactive REPL mode
  -h, --help                       Show help
  --version                        Show version
```

### Config File Format

```ini
# gnfs.cfg — example configuration
method = auto                  # auto/trial/rho/siqs/gnfs
degree = 4
rational_bound = 50000
algebraic_bound = 100000
large_prime_bound = 3000000
threads = 8
verbose = true
```

## C++ API

### High-Level: One-Call Factorization

```cpp
#include <gnfs/api/factorizer.hpp>

// Simplest usage
auto result = gnfs::api::factorize(gnfs::core::Integer("96091"));
if (result.success) {
    std::cout << result.factors[0].to_string() << " * "
              << result.factors[1].to_string() << "\n";
}

// String convenience
auto result = gnfs::api::factorize("1000036000099");

// Full JSON output
std::cout << result.to_json();

// With custom config + method selection
gnfs::api::Config cfg;
cfg.method = gnfs::api::FactorizationMethod::SIQS;  // force SIQS
cfg.verbose = false;
cfg.threads = 8;
auto result = gnfs::api::factorize(n, cfg);

// Check which method was actually used
std::cout << gnfs::api::method_name(result.stats.method_used) << "\n";
// → "SIQS" (or "Trial Division" if a small factor was found first)

// With progress callback
auto result = gnfs::api::factorize(n, cfg, [](const auto& info) {
    std::cout << gnfs::api::phase_name(info.phase)
              << ": " << info.phase_progress * 100 << "%\n";
});
```

### Mid-Level: Step-by-Step Pipeline

```cpp
#include <gnfs/api/pipeline.hpp>

gnfs::api::Pipeline pipeline(n, config);
pipeline.set_progress_callback(my_progress_cb);
pipeline.set_log_callback(my_log_cb);

// Execute phases individually
auto ctx     = pipeline.select_polynomial();
auto fb      = pipeline.build_factor_base(ctx);
auto rels    = pipeline.sieve_and_collect(ctx, fb);
auto filtered = pipeline.filter(std::move(rels));
auto mr      = pipeline.solve_matrix(std::move(filtered), fb, ctx);
auto result  = pipeline.extract_factors(mr, fb, ctx);

// Or run the entire pipeline at once
auto result = pipeline.run();

// Access detailed statistics
const auto& stats = pipeline.stats();
std::cout << "Matrix: " << stats.matrix_rows << "x" << stats.matrix_cols << "\n";
std::cout << "Total time: " << stats.timings.total_s << "s\n";
```

### Result Structure

```cpp
struct FactorResult {
    bool success;
    Integer n;                    // original input
    std::vector<Integer> factors; // found factors (sorted ascending)
    FactorStats stats;            // detailed statistics

    std::string to_text();   // "N = p * q\nTime: 1.0s (25 digits, 81 bits)"
    std::string to_json();   // full JSON with all stats
    std::string to_csv();    // "N,p,q,bits,digits,time_s,success"
};

struct FactorStats {
    // Input: n_bits, n_digits, degree
    // Factor base: rational/algebraic primes, bounds
    // Sieving: special_q_processed, candidates, relations
    // Filtering: singletons_removed, merged_relations
    // Linear algebra: matrix_rows/cols/weight, dependencies_found
    // Square root: dependencies_tried
    // Timings: per-phase breakdown + total
};
```

## Architecture

```
include/gnfs/           # 54 header files (.hpp)
├── api/           (6)  # Config, Pipeline, Factorizer, Result, Progress, i18n
├── core/          (6)  # Integer, Polynomial, Relation, Params, PolynomialContext, Types
├── polynomial/    (6)  # Kleinjung, Murphy E, Base-m, IntPolynomial, Optimizer, SelectorDispatch
├── factor_base/   (2)  # FactorBase data structure, Builder (Cantor-Zassenhaus)
├── sieve/         (4)  # LatticeSieve (bucket+lattice), LineSieve, Special-Q, LatticeBasis
├── cofactor/      (5)  # Cofactorizer, ECM (Montgomery), SQUFOF, TrialDivision, SmoothCheck
├── relation/      (3)  # Collector (thread-safe), Filter (1LP+2LP merge), OOCRelationStore
├── linalg/        (8)  # MatrixBuilder, BlockLanczos, BlockWiedemann, Gaussian, SGE,
│                       # Schirokauer, SparseMatrix, MmapCSRMatrix
├── sqrt/          (7)  # AlgebraicSqrt, HenselSqrt, Couveignes, RationalSqrt,
│                       # ClassGroup, ModularPoly, NumberField
├── siqs/          (1)  # SIQS — Self-Initializing Quadratic Sieve (Contini 1997)
└── util/          (6)  # SmallVector, ThreadPool, Logger, Timer, MmapFile, SafeMath

src/                    # 14 source files (.cpp)
├── api/           (2)  # pipeline.cpp, factorizer.cpp
├── cli/           (1)  # main.cpp — CLI entry point
├── core/          (3)  # integer, polynomial, relation
├── factor_base/   (1)  # builder (Cantor-Zassenhaus root finding)
├── linalg/        (3)  # block_lanczos, block_wiedemann, matrix_builder
├── polynomial/    (1)  # base_m
├── sieve/         (1)  # lattice_sieve
└── sqrt/          (2)  # algebraic_sqrt, rational_sqrt

tests/                  # 41 test files (.cpp)
```

### Module Overview

| Module | Headers | Key Components | Description |
|--------|:-------:|----------------|-------------|
| **api** | 6 | `Factorizer`, `Pipeline`, `Config`, `Result`, `Progress`, `i18n` | Public API layer with bilingual UI support |
| **core** | 6 | `Integer`, `Polynomial`, `Relation`, `Params`, `PolynomialContext`, `Types` | Fundamental types: GMP-wrapped integers, polynomials, relations, auto-parameters |
| **polynomial** | 6 | `KleinjungSelector`, `MurphyEvaluator`, `BaseMSelector`, `SelectorDispatch` | Polynomial selection: Kleinjung lattice + base-m, Murphy E scoring, adaptive dispatch |
| **factor_base** | 2 | `FactorBase`, `FactorBaseBuilder` | Factor base construction with parallel Cantor-Zassenhaus root finding |
| **sieve** | 4 | `LatticeSieve`, `LineSieve`, `SpecialQ`, `LatticeBasis` | Lattice sieve with Special-Q decomposition, bucket sieve for large factor bases |
| **cofactor** | 5 | `Cofactorizer`, `ECM`, `SQUFOF`, `TrialDivider`, `SmoothCheck` | Multi-strategy cofactorization: trial division → SQUFOF → ECM → smoothness |
| **relation** | 3 | `RelationCollector`, `RelationFilter`, `OOCRelationStore` | Thread-safe collection, 1LP+2LP graph merge, mmap-backed out-of-core storage |
| **linalg** | 8 | `BlockLanczos`, `BlockWiedemann`, `Gaussian`, `SGE`, `MatrixBuilder`, `SparseMatrix`, `MmapCSR`, `Schirokauer` | GF(2) null-space solving with SGE preprocessing, dual solver, Schirokauer maps |
| **sqrt** | 7 | `AlgebraicSqrt`, `HenselSqrt`, `Couveignes`, `RationalSqrt`, `ClassGroup`, `ModularPoly`, `NumberField` | Square root: Nguyen Hybrid (Hensel+CRT) primary, Couveignes fallback; class group characters |
| **util** | 6 | `SmallVector`, `ThreadPool`, `Logger`, `Timer`, `MmapFile`, `SafeMath` | Utilities: stack-allocated small vector, work-stealing thread pool, mmap file I/O |

## Pipeline — 8 Stages

The General Number Field Sieve decomposes factoring into 8 interdependent stages:

```
  ① Polynomial Selection    Kleinjung / base-m → f(x), g(x)
       ↓
  ② Factor Base Build       Parallel Cantor-Zassenhaus root finding
       ↓
  ③ Lattice Sieving         Special-Q + bucket sieve, multi-threaded
       ↓
  ④ Cofactorization         Trial div → SQUFOF → ECM (Stage 1+2)
       ↓
  ⑤ Relation Collection     Thread-safe collection + 1LP/2LP merge
       ↓
  ⑥ Linear Algebra          SGE preprocessing + Block Lanczos / Wiedemann
       ↓
  ⑦ Square Root             Nguyen Hybrid (Hensel+CRT) + Couveignes fallback
       ↓
  ⑧ GCD                     gcd(a ± b, N) → non-trivial factor
```

### Stage Details

**① Polynomial Selection** — The algorithm needs two polynomials f(x) and g(x) with a common root m modulo N. `SelectorDispatch` automatically chooses between base-m selection (small inputs) and Kleinjung lattice search (large inputs). Quality is scored using Murphy's E function, which estimates sieving yield.

**② Factor Base** — For each side (rational and algebraic), we find all primes up to a bound and their roots modulo f(x). Root finding uses **Cantor-Zassenhaus** algorithm (O(d² log p)), parallelized across primes using the thread pool — 460x faster than naive scanning for large factor bases.

**③ Lattice Sieving** — The most time-consuming phase. For each Special-Q prime q, we construct a 2D lattice of (a,b) pairs where the algebraic norm is divisible by q. The sieve identifies candidates whose norms are likely smooth (factorable over the factor base). **Bucket sieve** is used for large factor base primes to reduce cache misses; multi-threaded scatter distributes work across sieve regions.

**④ Cofactorization** — After sieving, candidates have residual cofactors that must be fully factored. The strategy is hierarchical: trial division handles small factors, SQUFOF (10-100x faster than Pollard rho) handles 2-word cofactors, and ECM with Montgomery curves (Stage 1 + Stage 2 BSGS with D=2310) handles the rest.

**⑤ Relation Collection** — Relations are (a,b) pairs where both norms are smooth. The collector is thread-safe with atomic counters. After collection, **large prime merge** combines partial relations: 1-LP uses greedy pairing (weight >= 2), 2-LP uses graph-based processing (weight = 2, LP keys as nodes, relations as edges).

**⑥ Linear Algebra** — The heart of GNFS: finding dependencies among relations over GF(2). First, **SGE** (Structured Gaussian Elimination) preprocesses the matrix by eliminating weight-1 and weight-2 columns, typically reducing matrix size by 30-60%. Then: matrices < 5K rows use **Gaussian elimination** directly; larger matrices use **Block Lanczos** (64-bit block parallel SpMV with ThreadPool) or **Block Wiedemann** (Krylov + Gaussian three-phase, Coppersmith 1994).

**⑦ Square Root** — For each dependency, we compute a rational and algebraic square root. The rational side is straightforward. The algebraic side uses the **Nguyen Hybrid** method: K small primes (~10-bit) with modular polynomial Tonelli-Shanks, Hensel lifting to ~10-20K bits, then CRT with 2^(K-1) sign search — 200x faster than the big-prime CRT approach. **Couveignes** algorithm serves as fallback. Class group characters verify correctness for arbitrary polynomial degrees.

**⑧ GCD** — Finally, gcd(rational_sqrt ± algebraic_sqrt_image, N) yields a non-trivial factor. Multiple dependencies are tried if needed.

## Key Algorithms

| Component | Algorithm | Complexity / Notes |
|-----------|-----------|-------------------|
| **Polynomial Selection** | Kleinjung lattice search + base-m | Murphy E scoring; `SelectorDispatch` auto-selects by N size |
| **Root Finding** | Cantor-Zassenhaus | O(d² log p), 460x faster than naive O(p) scan for large FB |
| **Sieving** | Lattice sieve + Special-Q + Bucket sieve | Multi-threaded; bucket scatter for large FB primes |
| **Cofactorization** | Trial division → SQUFOF → ECM | SQUFOF: 10-100x faster than Pollard rho for 2-word cofactors |
| **ECM** | Montgomery curves, Stage 1+2 BSGS | D=2310, 480 baby steps, differential giant steps |
| **Relation Merge** | 1-LP greedy + 2-LP graph-based | Weight >= 2 for 1LP, weight = 2 for 2LP (GNFS standard) |
| **SGE** | Structured Gaussian Elimination | Weight-1/2 column elimination, 30-60% matrix reduction |
| **Block Lanczos** | Montgomery (1995) | 64-bit block parallel SpMV with ThreadPool; > 5K rows |
| **Block Wiedemann** | Coppersmith (1994) | Krylov + Gaussian three-phase; 4 GB memory guard |
| **Gaussian** | Packed GF(2) elimination | 64-bit word-packed; fallback for matrices < 5K rows |
| **Schirokauer Maps** | GF(2), l=2 only | Split detection, Hensel-based computation; exponent = l^d - 1 |
| **Square Root** | Nguyen Hybrid (primary) | K small primes + Hensel lift + CRT; 200x faster than big-prime CRT |
| **Square Root** | Couveignes (fallback) | Gray Code CRT, 65536-pattern search, per-prime verification |
| **Class Group** | Sturm theorem | Arbitrary-degree real root counting for character verification |

## Performance Optimizations

### Sieving (Phase 3) — Dominant Phase

- **Bucket sieve**: Large factor base primes use bucket-based scatter to reduce cache misses; multi-threaded per-region processing
- **Compact sieve primes**: `CompactSmallPrime` (12 bytes) vs `PrimeEntry` (36 bytes) for better cache utilization
- **SQ pre-division**: Pre-divide algebraic norms by Special-Q prime, skip ~10K SQ-range FB entries
- **Adaptive sieve threshold**: 0.6x factor for <=25-digit inputs — fewer but higher-quality candidates

### Cofactorization (Phase 4)

- **SQUFOF**: 10-100x faster than Pollard rho for 2-word (~128-bit) cofactors
- **p^2 safe early exit**: If cofactor < p^2 and > FB_bound, skip remaining trial division
- **Adaptive Miller-Rabin**: Jaeschke (1993) deterministic bounds — 1-7 witnesses by number size
- **ECM Stage 2 BSGS**: D=2310, 480 baby steps with differential giant steps

### Linear Algebra (Phase 6)

- **SGE preprocessing**: Weight-1 and weight-2 column elimination reduces matrix 30-60%
- **PackedGF2Matrix**: 64-bit word-packed with O(1) bit access
- **Block Lanczos**: 64-bit block width, parallel SpMV via persistent ThreadPool (replaces per-pivot thread creation — 360ms → ~0ms overhead)
- **Block Wiedemann**: Krylov + Gaussian three-phase (Coppersmith 1994); scalar Berlekamp-Massey fails on non-nilpotent B=M·M^T
- **Gaussian threshold**: Matrices < 5K rows use direct Gaussian (faster than BL/BW overhead)

### Square Root (Phase 7)

- **Nguyen Hybrid**: K=3 small primes (~10-bit) + Hensel lift to ~10-20K bits + CRT with 2^(K-1) sign search. L5 sqrt: 200x speedup vs big-prime CRT
- **Couveignes**: Pre-computed expected product, Gray Code CRT, 65536-pattern search, per-prime verification

### Factor Base Construction (Phase 2)

- **Parallel Cantor-Zassenhaus**: Multi-threaded root finding + `mpz_divisible_ui_p` fast divisibility test. FB build: 0.31s → 0.02s for 25-digit

### Out-of-Core Infrastructure

- **MmapCSRMatrix**: Memory-mapped CSR (Compressed Sparse Row) matrix for matrices exceeding available RAM
- **OOCRelationStore**: mmap-backed relation storage for large-scale factorizations
- **MmapFile**: Utility for memory-mapped file I/O across the codebase

## Testing

The project uses `scripts/test.sh` — a unified test runner with automatic compilation, per-test timeouts (zsh native, no GNU coreutils dependency), heartbeat monitoring, and tiered test levels.

### Quick Reference

```bash
# Daily development (most common)
./scripts/test.sh                      # Smoke: 24 instant tests, ~5s
./scripts/test.sh changed              # Auto-detect affected modules from git diff
./scripts/test.sh changed --deep       # Same + cascade to dependent modules

# Module-level
./scripts/test.sh module linalg        # Just linear algebra
./scripts/test.sh module sieve sqrt    # Multiple modules
./scripts/test.sh module all           # All modules (instant+fast only)
./scripts/test.sh module all --slow    # All modules (including slow+heavy)

# Single test
./scripts/test.sh run test_linalg      # Specific test binary
./scripts/test.sh run sqrt             # Auto-prepends test_ prefix

# Merge gate
./scripts/test.sh gate                 # Gate: smoke + regression (17/27/40/81-bit) ~20s
./scripts/test.sh gate --quick         # Quick gate: smoke only ~5s

# E2E & Progressive
./scripts/test.sh e2e                  # Full GNFS pipeline (~5min)
./scripts/test.sh L1                   # Progressive Level 1 only
./scripts/test.sh progressive 1 3      # L1 through L3

# Full suite
./scripts/test.sh full                 # ctest + E2E + Progressive L1-L2
./scripts/test.sh thorough             # All modules + integration + L1-L3
./scripts/test.sh nightly              # Everything + L4 + L5 + stress L1

# Stress tests
./scripts/test.sh stress               # 50/60-digit, all
./scripts/test.sh stress 1 1           # 50-digit only (164-bit, ~2.6h)
./scripts/test.sh stress 1 2           # 50 + 60-digit

# Utilities
./scripts/test.sh list                 # List all tests, modules, timeouts, tiers
./scripts/test.sh build                # Build only, no tests
./scripts/test.sh --no-build smoke     # Skip build, run directly
./scripts/test.sh -t Release full      # Release mode build
./scripts/test.sh --fail-fast full     # Stop at first failure
./scripts/test.sh --timeout 30 run test_kleinjung  # Custom timeout
./scripts/test.sh bench --save         # Performance benchmark + save results
./scripts/test.sh watch                # Watch files, auto-retest (needs fswatch)
```

### Test Tiers

| Tier | Timeout | Tests | Description |
|------|---------|-------|-------------|
| **instant** | 10s | 15 | Unit tests — integer, small_vector, thread_pool, factor_base, special_q, relation_collector, cofactor, linalg, sqrt, sqrt_debug, murphy, api, i18n, method_selection, etc. |
| **fast** | 60s | 1 | Sieve integration (test_sieve_basic) |
| **slow** | 120–300s | 5 | Regression gate, Kleinjung, lattice sieve, E2E pipeline, factor_with_kleinjung |
| **heavy** | 600–3600s | 3 | Progressive L3-L5, 25-digit benchmark, Kleinjung large |
| **stress** | 43200s | 1 | 50-digit (L1) + 60-digit (L2) + 100-digit (L3) |

### Test Files (42 total)

| Category | Tests | Description |
|----------|-------|-------------|
| **Core** | test_integer, test_small_vector, test_thread_pool, test_work_stealing | Fundamental types, threading |
| **Polynomial** | test_base_m, test_int_polynomial, test_polynomial_context, test_polynomial_optimizer, test_murphy, test_kleinjung, test_kleinjung_large | Polynomial selection pipeline |
| **Factor Base** | test_factor_base | Builder + Cantor-Zassenhaus |
| **Sieve** | test_special_q, test_sieve_basic, test_lattice_sieve, test_line_sieve, test_bucket_sieve | Sieve strategies |
| **Cofactor** | test_cofactor, test_squfof | Trial division, ECM, SQUFOF |
| **Relation** | test_relation_collector, test_filter, test_ooc_relations | Collection, merge, OOC |
| **Linear Algebra** | test_linalg, test_block_wiedemann, test_schirokauer_deg4, test_mmap_csr | Solvers, matrix infrastructure |
| **Square Root** | test_sqrt, test_sqrt_debug, test_class_group | Hensel, Couveignes, class group |
| **Integration** | test_integration, test_edge_cases, test_regressions, test_regression_gate | Cross-module, boundary cases |
| **E2E** | test_gnfs_e2e, test_factor_with_kleinjung, test_gnfs_progressive | Full pipeline tests |
| **Benchmark** | test_25digit, test_stress | Performance benchmarks |
| **API** | test_api, test_i18n, test_params, test_method_selection | Public API, i18n, parameters, method selection (35 cases) |

### Scenario Recommendations

| Scenario | Command | Time |
|----------|---------|------|
| Changed a function, quick verify | `./scripts/test.sh` | ~5s |
| Changed linalg module | `./scripts/test.sh module linalg` | ~1s |
| Changed core pipeline | `./scripts/test.sh e2e` | ~5min |
| Not sure what changed | `./scripts/test.sh changed` | auto |
| Feature branch merge gate | `./scripts/test.sh gate` | ~20s |
| Major changes, full regression | `./scripts/test.sh full` | ~10min |
| PR final verification | `./scripts/test.sh thorough` | ~30min |
| Performance benchmark | `./scripts/test.sh bench --save` | ~1hr |
| Verify large factorization | `./scripts/test.sh stress 1 1` | ~2.6h |

## Build Options

### Build Types

```bash
# Debug build (assertions enabled, debug symbols, no optimization)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(sysctl -n hw.ncpu)

# Release build (O3 + LTO + native CPU flags)
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(sysctl -n hw.ncpu)

# Release with debug info
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C build -j$(sysctl -n hw.ncpu)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `GNFS_BUILD_TESTS` | `ON` | Build test executables |
| `GNFS_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer |
| `GNFS_ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer |
| `GNFS_ENABLE_UBSAN` | `OFF` | Enable UndefinedBehaviorSanitizer |

```bash
# Build with AddressSanitizer (detects memory errors)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGNFS_ENABLE_ASAN=ON

# Build with ThreadSanitizer (detects data races)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGNFS_ENABLE_TSAN=ON

# Build without tests
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGNFS_BUILD_TESTS=OFF
```

### Auto-Detected Features

The build system automatically detects and enables:

- **GMP** (required) — arbitrary precision integer arithmetic
- **NTL** (optional) — additional number theory routines; adds `GNFS_HAVE_NTL=1` define
- **Metal** (macOS, optional) — GPU acceleration framework
- **Native CPU flags** — `-mcpu=native` on Apple Silicon, `-march=native` on x86
- **ThinLTO** (Clang on macOS) or **LTO** (GCC) in Release mode

## Dependencies

| Library | Required | Version | Purpose |
|---------|:--------:|---------|---------|
| **GMP** | Yes | 6.0+ | Arbitrary precision integer arithmetic (`mpz_class`) |
| **pthreads** | Yes | — | Multi-threading (`std::thread`, `std::mutex`, etc.) |
| **NTL** | No | 11.0+ | Number theory (polynomial arithmetic, optional features) |
| **Metal** | No | — | macOS GPU acceleration framework (future use) |

All dependencies are well-established, widely available libraries. GMP and pthreads are the only hard requirements.

## Design Conventions

### Element Representation

**Elements are `a - b*alpha` (NOT `a + b*alpha`).** This is the fundamental convention throughout the entire codebase. Changing this would break norm computations, Schirokauer maps, and square root extraction.

### Integer Types

- **`gnfs::core::Integer`** — GMP `mpz_class` wrapper used for all large integer operations
- **`uint64_t`** — used for factor base primes, sieve indices
- **`__uint128_t`** — used for intermediate products where `uint64_t` would overflow
- **`Integer`** — used when even `__uint128_t` is insufficient (Hensel lifting, etc.)

### Thread Safety

- `RelationCollector` uses `std::atomic` counters and per-thread relation buffers
- `LatticeSieve::sieve_parallel()` distributes Special-Q primes across worker threads
- `BlockLanczos` uses `ThreadPool` for parallel SpMV (sparse matrix-vector multiply)
- `FactorBaseBuilder` parallelizes Cantor-Zassenhaus root finding across primes

### Naming Conventions

- Functions and variables: `snake_case`
- Types and classes: `PascalCase`
- Namespaces: `gnfs::core`, `gnfs::linalg`, `gnfs::sieve`, `gnfs::api`, etc.
- Header files: `snake_case.hpp`

### Error Handling

- Internal logic errors: `assert()` or custom `GNFS_ASSERT` macro
- Recoverable runtime errors: `std::optional` or error codes
- Fatal errors: `throw std::runtime_error("description")`
- No empty catch blocks — all exceptions are at least logged

## Project Structure

```
GNFS/
├── include/gnfs/           # 53 header files (.hpp), 10 submodules
│   ├── api/           (6)  # Public API layer
│   ├── core/          (6)  # Fundamental types
│   ├── polynomial/    (6)  # Polynomial selection
│   ├── factor_base/   (2)  # Factor base construction
│   ├── sieve/         (4)  # Lattice/bucket sieve
│   ├── cofactor/      (5)  # Cofactorization strategies
│   ├── relation/      (3)  # Relation collection & merge
│   ├── linalg/        (8)  # Linear algebra solvers
│   ├── sqrt/          (7)  # Square root extraction
│   └── util/          (6)  # Utilities
├── src/                    # 14 source files (.cpp)
│   ├── api/           (2)  # Pipeline, Factorizer
│   ├── cli/           (1)  # CLI main entry point
│   ├── core/          (3)  # Integer, Polynomial, Relation
│   ├── factor_base/   (1)  # Builder
│   ├── linalg/        (3)  # Block Lanczos, Block Wiedemann, Matrix Builder
│   ├── polynomial/    (1)  # Base-m
│   ├── sieve/         (1)  # Lattice Sieve
│   └── sqrt/          (2)  # Algebraic Sqrt, Rational Sqrt
├── tests/                  # 41 test files (.cpp)
├── scripts/
│   ├── test.sh             # Unified test runner (timeout, tiers, heartbeat)
│   └── feature-branch.sh   # Feature branch workflow helper
├── docs/                   # Documentation and design plans
├── CMakeLists.txt          # Build configuration
├── CLAUDE.md               # Development instructions
├── BACKLOG.md              # Open issues (prioritized P1-P3)
├── RESOLVED.md             # Resolved issues & false positives
├── README.md               # Chinese version (default)
├── README-EN.md            # This file (English)
└── LICENSE                 # GPL-2.0
```

### Code Statistics

| Category | Files | Lines | Description |
|----------|:-----:|------:|-------------|
| Headers | 53 | ~18,700 | Template-heavy design; most implementation in `.hpp` |
| Sources | 14 | ~3,600 | Compiled implementation files |
| Tests | 41 | ~19,000 | Unit, integration, regression, E2E, stress |
| **Total** | **108** | **~41,200** | |

## Contributing

Contributions are welcome! Please follow these guidelines:

1. **Fork** the repository
2. **Create a feature branch** with date prefix: `feat/YYMMDD-description`
3. **Ensure all tests pass**: `./scripts/test.sh gate` (merge gate, ~20s)
4. **Follow conventions**: C++20 style, `snake_case` functions, `PascalCase` types
5. **Submit a Pull Request** with clear description

### Commit Convention

This project uses [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short description>

[optional body: explain why, not what]
```

| Type | Purpose | Example |
|------|---------|---------|
| `feat` | New feature | `feat(sieve): add bucket sieve for large factor bases` |
| `fix` | Bug fix | `fix(sqrt): prevent Hensel p^d overflow with Integer` |
| `perf` | Performance | `perf(lanczos): parallelize SpMV with ThreadPool` |
| `refactor` | Refactoring | `refactor(core): extract polynomial context` |
| `test` | Tests | `test(linalg): add Block Wiedemann edge case tests` |
| `chore` | Build/tooling | `chore: update CMakeLists for NTL optional` |
| `docs` | Documentation | `docs: update README with architecture details` |

**Scopes**: `core`, `polynomial`, `factor_base`, `sieve`, `cofactor`, `relation`, `linalg`, `sqrt`, `util`, `api`, `cli`

### Branch Naming

| Type | Format | Example |
|------|--------|---------|
| Feature | `feat/YYMMDD-desc` | `feat/260315-block-wiedemann` |
| Fix | `fix/YYMMDD-desc` | `fix/260308-hensel-overflow` |
| Performance | `perf/YYMMDD-desc` | `perf/260308-lanczos-simd` |
| Experiment | `exp/YYMMDD-desc` | `exp/260308-neon-sieve` |

## References

### Core GNFS Theory

- Lenstra, A.K., Lenstra, H.W. (eds.) *The Development of the Number Field Sieve*, Lecture Notes in Mathematics 1554, Springer (1993)
- Buhler, J.P., Lenstra, H.W., Pomerance, C. *Factoring integers with the number field sieve*, Journal of Cryptology 6(2):85-105 (1993)
- Briggs, M.E. *An Introduction to the General Number Field Sieve*, Master's thesis, Virginia Tech (1998)

### Polynomial Selection

- Kleinjung, T. *On polynomial selection for the general number field sieve*, Mathematics of Computation 75(256):2037-2047 (2006)
- Murphy, B.A. *Polynomial Selection for the Number Field Sieve Integer Factorisation Algorithm*, PhD thesis, Australian National University (1999)

### Linear Algebra

- Montgomery, P.L. *A block Lanczos algorithm for finding dependencies over GF(2)*, Advances in Cryptology — EUROCRYPT '95, pp. 106-120 (1995)
- Coppersmith, D. *Solving homogeneous linear equations over GF(2) via block Wiedemann algorithm*, Mathematics of Computation 62(205):333-350 (1994)

### Square Root

- Couveignes, J.-M. *Computing a square root for the number field sieve*, in *The Development of the Number Field Sieve*, pp. 95-107 (1993)
- Nguyen, P.Q. *A Montgomery-like Square Root for the Number Field Sieve*, ANTS-III, pp. 151-168 (1998)

### Cofactorization

- Lenstra, H.W. Jr. *Factoring integers with elliptic curves*, Annals of Mathematics 126:649-673 (1987)
- Shanks, D. *SQUFOF*, unpublished; analyzed in Gower, J.E. *Square form factorization*, PhD thesis (2004)

### Reference Implementations

- [CADO-NFS](https://cado-nfs.gitlabpages.inria.fr/) — state-of-the-art GNFS implementation (INRIA/LORIA)
- [msieve](https://github.com/radii/msieve) — integer factorization library by Jason Papadopoulos

## License

This project is licensed under the **GNU General Public License v2.0** — see the [LICENSE](LICENSE) file for details.
