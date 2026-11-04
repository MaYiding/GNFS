# SIQS Multiplier Candidate Plan

This plan isolates multiplier ranking and attempt admission from deadline and
shadow-proof changes. It records a baseline experiment, not an implemented
portfolio or a timeout fix.

## Baseline and Scope

The inspected integration commit is
`00d3ddf17b11751434457b3d9311a10db8f3c0e8`. Its
[`siqs.hpp`](../../include/gnfs/siqs/siqs.hpp) exposes `select_multiplier()` and
one monolithic `factor()` call. It has no `rank_multiplier_candidates()`,
`factor_once()`, portfolio loop, or attempt counter. Production selects one
multiplier and constructs `kN` from it.

There is therefore no premature portfolio truncation to fix independently on
this baseline. Adding a retry controller would also move setup, timing, and
shadow finalization out of the existing facade. This document leaves those
runtime contracts unchanged.

The historical alternatives are:

| Proposal | Exact Head | Relevant Behavior |
|---|---|---|
| PR #252 | `ab9853b68f55ea7e7e8d910d27bd26da06fe880a` | Rank three candidates; return a proper shared divisor directly; add internal cancellation checks |
| PR #253 | `27617fe888dfe3a80bfbdce303b09930d0fef5ad` | Rank all 21 candidates; non-coprime candidates do not consume portfolio slots |

The experiment below uses the exact ranking function from #253. It models an
incorrect partial extraction that ranks only three candidates before applying
the coprime guard. It does not claim that #252 executes this combination:
#252 uses a direct-factor route instead of #253's skip policy.

## Dynamic Baseline

The probe compiles the integration header and the extracted #253 ranking
function in the same executable. Apple Clang 21.0.0 on macOS arm64 produced
identical output with `-O0 -g` and `-O2 -DNDEBUG`. All checks remain active in
both builds. Ranking's first candidate matched the integration selector for
all 995 odd inputs from 15 through 2003, inclusive.

| Input N | Raw First Three | Truncate, Then Filter | Filter All, Then Take Three |
|---|---|---|---|
| 303 | 3, 7, 23 | 7, 23 | 7, 23, 1 |
| 15015 | 7, 1, 3 | 1 | 1, 23, 43 |
| 17 | 17, 5, 13 | 5, 13 | 5, 13, 1 |
| 105 | 1, 17, 29 | 1, 17, 29 | 1, 17, 29 |

For `N=15015=3*5*7*11*13`, two positions in the raw prefix are inadmissible.
Discarding the rest of the ranking loses two available coprime candidates.
For `N=17`, the first candidate has `gcd(N,k)=N`; a guard that rejects only
proper divisors would still admit it. The fixed candidate `k=1` is coprime to
every positive input.

This evidence establishes candidate availability and historical selector
parity. The probe does not call `factor()`, run a sieve, test deadline behavior,
or measure a factoring speedup. Floating-point scores retain the historical
formula; near-tie ordering across platforms is not a golden contract.

## Minimal Implementation Sequence

1. Extract the fixed 21-candidate ranking from #253 into `siqs.hpp`. Preserve
   its score formula, raw unfiltered ranking, and historical ordinal tie-break.
   A zero limit returns an empty vector; a positive limit requires positive
   `N`. Keep `select_multiplier(N)` as the first ranked candidate for supported
   positive inputs. Do not silently replace it with a coprime selector.
2. Add an internal candidate-admission helper that scans the complete ranking.
   Reject `gcd(N,k)>1`, including `gcd==N`, before polynomial setup. Apply an
   eligible-candidate limit after filtering, preserving relative order. For
   any supplied ranking and limit, its output must equal the corresponding
   prefix of a complete coprime-filtered reference sequence.
3. Integrate attempt counting only after a separate, reviewed facade extraction
   provides a single-attempt entry point without changing existing timing or
   shadow behavior. Increment the counter immediately before starting an
   eligible attempt, after the existing admission/budget check. Skipped
   candidates and calls rejected before setup consume no attempt. Stop after
   success, the existing budget stop, exhaustion, or three started attempts.

The ranking and admission helpers can be reviewed independently. Until step
three is available, do not advertise a production portfolio. No new stage
executor, cancellation API, clock policy, runtime flag, budget partition, or
direct-GCD return is needed for candidate admission.

The future controller must retain the requested shadow mode on the first
actual attempt, not on the first ranked candidate. It must keep retries free
of duplicate shadow records and preserve the current pre-emit result/record
commit contract. Changing zero-budget behavior, finalizer placement, or solver
cancellation belongs in separate work with its own acceptance evidence.

## Validation Contract

Extend `tests/test_siqs.cpp` with Release-active checks for:

- Raw ranking size, uniqueness, limit behavior, deterministic same-build order,
  and compatibility with the historical selector on supported positive inputs.
- Complete filtering before an eligible limit, using the counterexamples above
  plus a synthetic ranking where several leading candidates share factors.
  Include `k=N`, `k=1`, zero limit, and limits above the fixed candidate count.
- A future controller's exact started-attempt count. Exercise a first actual
  attempt after skipped candidates, failure followed by a second attempt,
  success stopping further attempts, and the existing budget stop. A test that
  accepts any count from one through three does not prove retries or counting.
- Unchanged existing off/observe/prefer tests. Candidate changes must not
  silently alter terminal record count or the prefer commit point.

Run the narrow helper tests in Debug and Release through the project runner:

```bash
./scripts/test.sh -t Debug run test_siqs
./scripts/test.sh -t Release run test_siqs
```

After production facade wiring, also run `./scripts/test.sh module siqs` and
`./scripts/test.sh changed --deep`. This plan does not claim those future
runtime validations have passed.

## Reproduce the Baseline

Use the inspected integration tree with this document present. The #253 commit
object must be available locally. The diagnostic compiles only its small
executable and `src/core/integer.cpp`; it needs a C++20 compiler and GMP. This
direct compiler invocation is a diagnostic exception because the normal test
catalog cannot compare functions from these two revisions in one process.
On systems where GMP is outside the default compiler search path, set
`GMP_PREFIX` to its installation prefix first. For Homebrew, use
`export GMP_PREFIX="$(brew --prefix gmp)"`.

Run the following block from the repository root. It extracts the C++ block
below, adds the exact historical function without copying its implementation
into this document, and leaves outputs under the ignored `.cache/` directory.

```bash
python3 - <<'PY'
from pathlib import Path
import os
import shlex
import subprocess

cache = Path('.cache/multiplier-portfolio-plan')
cache.mkdir(parents=True, exist_ok=True)
document = Path('docs/plans/2026-11-05-siqs-multiplier-candidates.md').read_text()
probe = document.split('```cpp\n', 1)[1].split('\n```', 1)[0]
(cache / 'probe.cpp').write_text(probe + '\n')
revision = '27617fe888dfe3a80bfbdce303b09930d0fef5ad'
source = subprocess.check_output(
    ['git', 'show', revision + ':include/gnfs/siqs/siqs.hpp'], text=True)
begin = source.index('inline std::vector<uint32_t> rank_multiplier_candidates(')
end = source.index('\n/// Select the top-ranked multiplier', begin)
header = ('#include <gnfs/siqs/siqs.hpp>\n'
          'namespace portfolio_253 {\nusing gnfs::core::Integer;\n'
          'using gnfs::siqs::mod_pow32;\n' + source[begin:end] + '\n}\n')
(cache / 'ranking253.hpp').write_text(header)
gmp = []
if prefix := os.environ.get('GMP_PREFIX'):
    gmp = ['-I' + str(Path(prefix) / 'include'), '-L' + str(Path(prefix) / 'lib')]
for mode, flags in [('Debug', ['-O0', '-g']), ('Release', ['-O2', '-DNDEBUG'])]:
    binary = cache / ('probe-' + mode)
    command = shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', *flags, '-Iinclude', '-I' + str(cache),
        str(cache / 'probe.cpp'), 'src/core/integer.cpp', *gmp, '-lgmp', '-o', str(binary)]
    subprocess.run(command, check=True, timeout=90)
    output = subprocess.check_output([str(binary)], text=True, timeout=30)
    (cache / (mode + '.log')).write_text(output)
    print(mode + '\n' + output, end='')
if (cache / 'Debug.log').read_bytes() != (cache / 'Release.log').read_bytes():
    raise SystemExit('Debug/Release output differs')
PY
```

```cpp
#include "ranking253.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <gnfs/siqs/siqs.hpp>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::vector<uint32_t> coprime_prefix(const gnfs::core::Integer& n,
                                     const std::vector<uint32_t>& ranked) {
    std::vector<uint32_t> result;
    for (uint32_t k : ranked) {
        const auto residue = static_cast<uint32_t>(mpz_fdiv_ui(n.get_mpz(), k));
        if (std::gcd(k, residue) > 1)
            continue;
        result.push_back(k);
        if (result.size() == 3)
            break;
    }
    return result;
}
void print_values(const char* label, const std::vector<uint32_t>& values) {
    std::cout << ' ' << label << '=';
    for (size_t i = 0; i < values.size(); ++i)
        std::cout << (i == 0 ? "" : ",") << values[i];
}
int main() {
    using gnfs::core::Integer;
    using portfolio_253::rank_multiplier_candidates;
    size_t parity_count = 0;
    for (unsigned n = 15; n <= 2003; n += 2) {
        const Integer modulus(n);
        const auto ranked = rank_multiplier_candidates(modulus, 21);
        require(ranked.size() == 21, "candidate set was truncated");
        require(ranked.front() == gnfs::siqs::select_multiplier(modulus),
                "ranking top candidate differs from integrated selector");
        ++parity_count;
    }
    std::cout << "selector/ranking parity cases=" << parity_count << '\n';
    for (const char* text : {"303", "17", "105", "15015"}) {
        const Integer n(text);
        const auto ranked = rank_multiplier_candidates(n, 21);
        const auto limited = rank_multiplier_candidates(n, 3);
        const auto premature = coprime_prefix(n, limited);
        const auto correct = coprime_prefix(n, ranked);
        std::cout << "N=" << text << " selector=" << gnfs::siqs::select_multiplier(n);
        print_values("raw_first_three", limited);
        print_values("truncate_then_filter", premature);
        print_values("filter_then_take_three", correct);
        std::cout << '\n';
        if (std::string_view(text) == "303") {
            require(gnfs::siqs::select_multiplier(n) == 3, "303 selector fixture changed");
            require(premature.size() == 2 && correct.size() == 3,
                    "303 did not demonstrate lost coprime candidate");
            require(correct[0] == premature[0] && correct[1] == premature[1],
                    "filtering changed relative order");
        }
        if (std::string_view(text) == "17") {
            require(ranked.front() == 17, "equal-candidate fixture changed");
            require(std::find(correct.begin(), correct.end(), 17) == correct.end(),
                    "gcd equal to N was not rejected");
        }
    }
}
```
