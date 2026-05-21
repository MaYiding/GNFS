# Full-Pipeline Resume Design (Phase 1 + Phase 2 + Phase 3)

Status: implemented 2026-05-21 in `feat/260521-sieve-resume-phase12`.

## Motivation

Long-running factorizations (50d+, 60d+) routinely take hours to days. A crash
or restart anywhere in the pipeline used to discard all prior work. Wave -1
(2026-05-18) added `GNFS_SIEVE_RESUME` to persist Phase 3 (sieve loop) state
plus the OOC relation store, but Phase 1 (Kleinjung polynomial search) and
Phase 2 (Cantor-Zassenhaus factor base build) still re-ran from scratch on
every restart. Phase 1 alone can consume minutes on 50d and hours on 60d.

This change extends checkpoint coverage to Phase 1 and Phase 2 with a
result-only design that minimises in-flight bookkeeping.

## Scope

| Phase | Component                | Checkpoint            | Strategy              |
|-------|--------------------------|-----------------------|-----------------------|
| 1     | Polynomial selection     | `<base>.poly_ckpt`    | Result-only           |
| 2     | Factor base build        | `<base>.fb_ckpt`      | Result-only           |
| 3     | Sieve loop + OOC store   | `<base>.sieve_ckpt`   | Mid-flight (Wave -1)  |

"Result-only" means the checkpoint is written exactly once per phase, after
the phase succeeds. A second run loads the saved result and skips the whole
phase. There is no attempt to capture in-loop state for Phase 1 / Phase 2.

## Rationale for Result-Only on Phase 1 / Phase 2

- **Phase 1** (Kleinjung): multi-threaded random search over leading-coefficient
  candidates. The "position" is not well-defined across thread schedules, and
  the search converges with high variance. Saving the final `(f, g, m)` and
  reusing it directly is both simpler and strictly better than trying to resume
  partial search.
- **Phase 2** (Cantor-Zassenhaus): one-shot parallel root finding. There is no
  incremental progress to preserve mid-loop; the operation either completes or
  does not.

## File Format

All three checkpoint files share the same crash-safety primitive: write
`MAGIC_INCOMPLETE` first, fully serialise the body, then seek back to offset 0
and flip the four-byte magic to the finalised value. A reader that observes
`MAGIC_INCOMPLETE` refuses to load (treats the file as if it were absent).

### `<base>.poly_ckpt`

```text
u64 MAGIC ('GNFSPCKP') / MAGIC_INCOMPLETE
u64 VERSION
Integer N            (i32 sign + u32 byte_count + big-endian bytes)
Integer m            (same)
u32 degree
u32 coeff_count
Integer coeffs[coeff_count]
f64 skewness
f64 murphy_e         (informational)
```

Integer encoding uses `mpz_export` / `mpz_import` with `order=1, size=1,
endian=1, nails=0`, producing host-independent big-endian byte streams. A
1-GB byte-count cap guards against corruption.

### `<base>.fb_ckpt`

```text
u64 MAGIC ('GNFSFCKP') / MAGIC_INCOMPLETE
u64 VERSION
u32 rational_bound
u32 algebraic_bound
u32 special_q_bound
u64 large_prime_bound
u32 log_scale (zero-padded from u8)
u32 ctx_degree
Integer ctx_N        (fingerprint)
u32 rational_count
{ u32 p, u32 log_p } × rational_count
u32 algebraic_count
{ u32 p, u32 r, u32 log_p, u32 degree_pad } × algebraic_count
u64 sieve_algebraic_count
```

Validation on load checks all build parameters plus the polynomial-context
fingerprint (N and degree). Any mismatch returns a non-Ok status and forces
a fresh build.

## ENV Surface

```bash
# Preferred name covering Phase 1 + 2 + 3
GNFS_RESUME=<base_path> ./gnfs <N>

# Legacy alias retained for backward compatibility (also covers all phases)
GNFS_SIEVE_RESUME=<base_path> ./gnfs <N>
```

When either ENV is set, all three checkpoints share the same `<base_path>`
prefix:

```text
<base_path>.poly_ckpt
<base_path>.fb_ckpt
<base_path>.sieve_ckpt
<base_path>.reldata    # OOC relation store (Wave -1)
<base_path>.relidx     # OOC index
```

Precedence: `GNFS_RESUME` takes priority over `GNFS_SIEVE_RESUME` when both
are set. Both empty values are ignored.

## Integration Points

`src/api/pipeline.cpp`:

- `pipeline_resume_base_path()` — helper resolving the ENV precedence.
- `Pipeline::select_polynomial()` — load `<base>.poly_ckpt` if present and
  N matches; otherwise run selection and save the result.
- `Pipeline::build_factor_base()` — load `<base>.fb_ckpt` if present and all
  build params + ctx fingerprint match; otherwise build and save.
- `Pipeline::sieve_and_collect()` — unchanged Phase 3 behaviour, but the
  ENV detection now goes through the shared helper so both env names work.
- SGE-OOC `auto` mode also detects either env via the helper.

`include/gnfs/polynomial/poly_checkpoint.hpp` and
`include/gnfs/factor_base/fb_checkpoint.hpp` are new header-only modules.

## Tests

- `tests/test_poly_checkpoint.cpp` (12 cases, instant): roundtrip with small
  and multi-limb Integers, negative / zero coefficients, Context conversion,
  N validation, exists check, INCOMPLETE rejection, version mismatch,
  corrupt counts, remove, force-load, nonexistent.
- `tests/test_fb_checkpoint.cpp` (9 cases, instant): roundtrip, FactorBase
  conversion, `matches()` Ok plus four mismatch reasons, empty FB, 10K-prime
  FB, INCOMPLETE rejection, version mismatch, remove plus nonexistent.
- `tests/test_full_resume.cpp` (6 cases, slow, 120 s budget): end-to-end via
  `Pipeline` on a 40-bit composite N. Validates fresh-vs-resume wall-time
  delta, alias ENV, no-env default, stale-fb rebuild, wrong-N rejection.

## Measured Overhead and Speedup

On a 40-bit composite (the size used in the e2e test):

| Path     | Wall-time |
|----------|-----------|
| Fresh    | 116 ms    |
| Resume   | 5 ms      |
| Speedup  | ~23x      |

The qualitative win scales with N. On 50d-60d composites the Kleinjung
selector alone runs for minutes to hours, so a resume that loads the prior
result reduces Phase 1 wall-time to essentially zero (single file read).

Saved file sizes are small: a 50d-class poly checkpoint is a few hundred
bytes; an FB checkpoint at 50d-class bounds is a few hundred KB to a few MB.
There is no measurable runtime overhead on the fresh-write path.

## Compatibility

- Default behaviour (no ENV) is unchanged. Pipeline never touches the
  checkpoint files, never opens them, never writes them.
- The existing `<base>.sieve_ckpt` format from Wave -1 is unchanged, so
  Phase 3 resume continues to work alongside the new Phase 1+2 logic.
- The `GNFS_SIEVE_RESUME` ENV remains valid as an alias.

## Limitations

- A Pipeline run that completes successfully does not delete the
  `<base>.poly_ckpt` and `<base>.fb_ckpt` files. They simply remain on
  disk for the next run with the same `<base_path>`. The `<base>.sieve_ckpt`
  is still removed at clean Phase 3 completion (Wave -1 behaviour preserved).
- The FB checkpoint is invalidated by any change in build params or context
  fingerprint, including `GNFS_OVERRIDE_LP_BITS`. This is correct (parameter
  drift would produce a wrong FB) but may surprise users who change ENV
  flags between resume attempts.
