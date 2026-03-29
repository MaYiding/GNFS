# GNFS — General Number Field Sieve

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey.svg)]()

Industrial-grade **General Number Field Sieve** implementation in C++20.

GNFS is the most powerful known classical algorithm for factoring large composite integers. This project implements the complete GNFS pipeline — from polynomial selection through square root extraction — as a high-performance C++ library with ~30K lines of code.

## Highlights

- **Complete pipeline**: All 8 stages of GNFS fully implemented and integrated
- **Verified correctness**: Successfully factors integers from 8-bit to 81-bit (25-digit), with 5-level progressive test suite
- **High performance**: Multi-threaded sieving, parallel Block Lanczos, Hensel sqrt with 12-thread precomputation
- **Production-quality**: 29 test files, 39 headers, overflow-safe arithmetic, thread-safe relation collection

## Factorization Results

| Level | N (example) | Bits | Time |
|-------|-------------|------|------|
| L1 | 143, 9991, 10403 | 8–14 | < 0.2s |
| L2 | 96091, 100160063 | 17–27 | < 1s |
| L3 | 1000036000099 | 40 | ~2s |
| L4 | 100000980001501 | 47 | ~5s |
| L5 | 1253371692427905599 | 61 | ~38s |
| 25-digit | 1669994516749619561652133 | 81 | ~410s |

## Quick Start

### Prerequisites

- C++20 compiler (Clang 14+ or GCC 12+)
- CMake 3.20+
- [GMP](https://gmplib.org/) (GNU Multiple Precision Arithmetic Library)

**Optional**: NTL (Number Theory Library), Metal (macOS GPU)

```bash
# macOS
brew install gmp cmake

# Ubuntu / Debian
sudo apt install libgmp-dev cmake build-essential
```

### Build

```bash
git clone <repo-url> && cd GNFS

# Configure & build
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Run smoke tests (~2s)
./scripts/test.sh
```

### Run a Factorization

```bash
# Progressive test — factors integers at increasing difficulty
./build/test_gnfs_progressive 1 5    # Run Level 1 through 5
```

## Architecture

```
include/gnfs/
├── core/               # Integer, Polynomial, Relation, Params
├── polynomial/         # Kleinjung selection, Murphy E, Base-m
├── factor_base/        # Factor base construction (Cantor-Zassenhaus)
├── sieve/              # Lattice sieve, Special-Q enumeration
├── cofactor/           # Trial division, ECM (Montgomery), smoothness
├── relation/           # Thread-safe collection, 1-LP partial merge
├── linalg/             # GF(2) sparse matrix, Block Lanczos, Schirokauer maps
├── sqrt/               # Hensel lifting, Couveignes, class group, rational sqrt
└── util/               # SmallVector, ThreadPool, Logger, Timer, SafeMath

src/                    # 10 implementation files
tests/                  # 29 test files
scripts/test.sh         # Unified test runner with timeout protection
```

### GNFS Pipeline

The algorithm proceeds through 8 stages:

```
 1. Polynomial Selection ──→  Kleinjung algorithm picks f(x), g(x)
 2. Factor Base Build    ──→  Rational & algebraic primes via Cantor-Zassenhaus
 3. Lattice Sieving      ──→  Multi-threaded sieve with Special-Q decomposition
 4. Cofactorization      ──→  Trial division + ECM for residual cofactors
 5. Relation Collection  ──→  Gather smooth relations (with 1-large-prime merge)
 6. Linear Algebra       ──→  GF(2) matrix + Block Lanczos null space
 7. Square Root          ──→  Hensel lifting (primary) / Couveignes (fallback)
 8. GCD                  ──→  gcd(a ± b, N) yields non-trivial factor
```

## Testing

The project uses `scripts/test.sh` — a unified test runner with automatic compilation, per-test timeouts, and tiered test levels.

```bash
# Smoke tests — 15 instant tests, <2s
./scripts/test.sh

# Run a specific module
./scripts/test.sh module linalg
./scripts/test.sh module sqrt

# Auto-detect affected modules from git diff
./scripts/test.sh changed

# Full end-to-end GNFS pipeline (~5min)
./scripts/test.sh e2e

# Complete regression suite
./scripts/test.sh full

# List all available tests
./scripts/test.sh list
```

### Test Tiers

| Tier | Timeout | Count | Description |
|------|---------|-------|-------------|
| **instant** | 10s | 15 | Unit tests — integer, polynomial, linalg, sqrt, etc. |
| **fast** | 60s | 1 | Sieve integration |
| **slow** | 180–300s | 4 | Kleinjung, lattice sieve, E2E pipeline |
| **heavy** | 600–3600s | 3 | Progressive L3–L5, 25-digit benchmark |

## Key Implementation Details

### Algorithms

| Component | Algorithm | Notes |
|-----------|-----------|-------|
| Polynomial Selection | Kleinjung (simplified) | Murphy E scoring, coefficient optimization |
| Root Finding | Cantor-Zassenhaus | O(d^2 log p), 460x faster than naive for large FB |
| Sieving | Lattice sieve + Special-Q | Multi-threaded, projective root support |
| Cofactorization | Trial division + ECM | Montgomery curves, Stage 1+2 BSGS (D=2310) |
| Relation Merge | 1-Large-Prime partial | Partial relations merged before linear algebra |
| Linear Algebra | Block Lanczos / Gaussian | BL for >10K rows, Gaussian fallback for small |
| Schirokauer Maps | GF(2), l=2 only | Split detection, Hensel-based computation |
| Square Root | Hensel lifting (primary) | 12-thread parallel precomputation; Couveignes fallback |

### Design Conventions

- **Element representation**: `a - b*alpha` (not `a + b*alpha`) throughout
- **Integer type**: `gnfs::core::Integer` wrapping GMP `mpz_class`
- **Overflow safety**: `__uint128_t` for intermediate products, `Integer` for unbounded
- **Thread safety**: `std::atomic` counters, per-thread RNG, sorted sparse rows

### Performance Optimizations

- `PackedGF2Matrix`: 64-bit word-packed with O(1) bit access
- Block Lanczos: 64-bit block parallel with ThreadPool SpMV
- Hensel sqrt: Precomputed expected product, 12-thread parallel lifting
- Couveignes: Gray Code CRT, 65536-pattern search, per-prime verification
- Cantor-Zassenhaus root finding: replaces naive O(p) scan

## Project Structure

```
GNFS/
├── CMakeLists.txt          # Build configuration
├── CLAUDE.md               # Development instructions
├── LICENSE                  # GPL-2.0
├── README.md               # This file
├── BACKLOG.md              # Open issues (prioritized)
├── RESOLVED.md             # Fixed issues & false positives
├── include/gnfs/           # 39 header files (9 modules)
├── src/                    # 10 source files
├── tests/                  # 29 test files
├── scripts/test.sh         # Test runner
└── docs/                   # Documentation
```

### Module Overview (39 headers)

| Module | Headers | Description |
|--------|---------|-------------|
| `core` | 5 | Integer, Polynomial, Relation, Params, PolynomialContext |
| `polynomial` | 5 | Base-m, Kleinjung, Murphy E, IntPolynomial, Optimizer |
| `factor_base` | 2 | Builder (Cantor-Zassenhaus), FactorBase data structure |
| `sieve` | 3 | LatticeSieve, Special-Q, LatticeBasis |
| `cofactor` | 4 | Cofactorizer, ECM, TrialDivision, SmoothCheck |
| `relation` | 2 | Collector (thread-safe), Filter (1-LP merge) |
| `linalg` | 5 | MatrixBuilder, BlockLanczos, Gaussian, Schirokauer, SparseMatrix |
| `sqrt` | 6 | AlgebraicSqrt, HenselSqrt, Couveignes, RationalSqrt, ClassGroup, ModularPoly |
| `util` | 5 | SmallVector, ThreadPool, Logger, Timer, SafeMath |

## Build Options

```bash
# Debug build (with assertions)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(sysctl -n hw.ncpu)

# Release build (optimized, LTO)
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j$(sysctl -n hw.ncpu)

# Disable tests
cmake -B build -DGNFS_BUILD_TESTS=OFF
```

The build automatically detects:
- **GMP** (required) — arbitrary precision arithmetic
- **NTL** (optional) — additional number theory routines
- **Metal** (macOS, optional) — GPU acceleration framework
- **Native CPU flags** — `-mcpu=native` (Apple Silicon) or `-march=native` (x86)

## Dependencies

| Library | Required | Version | Purpose |
|---------|----------|---------|---------|
| GMP | Yes | 6.0+ | Arbitrary precision integer arithmetic |
| NTL | No | 11.0+ | Number theory (polynomial arithmetic) |
| pthreads | Yes | — | Multi-threading |
| Metal | No | — | macOS GPU acceleration (future) |

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`feat/YYMMDD-description`)
3. Ensure all tests pass (`./scripts/test.sh full`)
4. Submit a Pull Request

The project follows [Conventional Commits](https://www.conventionalcommits.org/) for commit messages.

## License

This project is licensed under the **GNU General Public License v2.0** — see the [LICENSE](LICENSE) file for details.

## References

- Lenstra, A.K., Lenstra, H.W. (eds.) *The Development of the Number Field Sieve* (1993)
- Buhler, J.P., Lenstra, H.W., Pomerance, C. *Factoring integers with the number field sieve* (1993)
- Kleinjung, T. *On polynomial selection for the general number field sieve* (2006)
- Couveignes, J.-M. *Computing a square root for the number field sieve* (1993)
- Montgomery, P.L. *A block Lanczos algorithm for finding dependencies over GF(2)* (1995)
