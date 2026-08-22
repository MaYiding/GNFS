# Lattice Sieve Numeric Contract

## Scope

This contract covers lattice-basis reduction, lattice-coordinate projection,
`SieveRegion` geometry, and factor-base values admitted to fixed-width sieve
state. It separates exact integer guarantees from the floating-point geometry
used by skew-aware reduction.

## Exact Unskewed Reduction

The unskewed Gauss and two-dimensional LLL reducers compute signed products,
dot products, squared norms, comparisons, and quotient rounding with a
sign-magnitude value made from two 64-bit limbs. A squared norm or dot-product
magnitude can reach `2^127`; no intermediate is narrowed to `int64_t` before
the exact comparison or division is complete.

The multiplication backend is selected at compile time:

- GCC and Clang use native `__int128` when available.
- MSVC x64 uses `_umul128`.
- MSVC ARM64 uses `__umulh` plus the low 64-bit product.
- Other targets use four 32-bit partial products.

All backends implement the same unsigned 128-bit result. Exact division uses a
single-limb fast path and bounded 128-bit long division otherwise. Quotients
round to nearest with exact halfway cases away from zero. Quotient saturation
is only a totality guard; supported special-Q reduction keeps the quotient
below `2^32`.

SkewLLL is intentionally outside this bit-for-bit integer guarantee. Its
weighted norm uses `double`. Distributed identities restrict skewness to the
encoded exponent band `[2^-511, nextafter(2^512, 0)]`, whose square is normal
and finite under every rounding and flush-to-zero environment. Runtime
reduction also requires every norm, dot product, and quotient to remain finite.
The rounded quotient must fit `int64_t`, and the integral vector update uses the
exact two-limb checked path. Unsupported geometry therefore fails closed before
any floating-point-to-integer conversion. The resulting basis remains subject
to the projection and determinant checks below.

## Candidate Projection

Public candidates store `a` as `int64_t` and positive `b` as `uint64_t`, and use
the project convention `a - b*alpha`. `LatticeBasis::try_to_ab()` evaluates both
affine combinations with the exact two-limb arithmetic and returns no value
when either coordinate is outside `int64_t`. `to_ab()` reports the same
condition with `std::overflow_error`; it never relies on signed-overflow
behavior.

Before every initial or adaptive sieve pass,
`lattice_projection_fits_int64()` checks all four inclusive rectangle corners.
Each output coordinate is affine in `(i,j)`, so those corner extrema are
necessary and sufficient for the complete rectangular region. An
unrepresentable projection fails before prime-entry construction or candidate
collection.

Distributed worker chunk preparation applies the same check to every initial
basis and to the complete zero-hit adaptive retry trajectory before writer
authority is adopted. Actual hit counts can only stop that deterministic
trajectory early. The preflight observes the special-Q cap before advancing the
generator, so unreachable suffix work does not invalidate a capped chunk.

The determinant follows the same policy: `try_determinant()` returns no value
when the exact determinant is outside `int64_t`, and `determinant()` throws.
Lattice membership reduces `a` and `b` modulo nonzero `q` before multiplication,
so validating an extreme `int64_t` candidate cannot overflow.

## Region Geometry and Routing

`SieveRegion` endpoints are inclusive `int32_t` values. A valid region has:

- positive width and height, each at most `INT32_MAX`;
- an area representable as `size_t` and allocatable as `vector<uint16_t>`;
- all four `(a,b)` projection corners representable as `int64_t` for the
  current basis.

The default-region generator adds a policy cap of 256 Mi cells. Explicit
regions are governed by the representation and allocation checks above rather
than that default-generation policy.

The compact row-major path stores small-prime state in `int16_t`, making width
32768 its inclusive upper boundary. A wider valid region routes the complete
prime set through the full region-bucket path. Width 32769 is therefore valid
when the other geometry, projection, and allocation requirements hold.

## Factor-Base Admission

Prime-entry state stores the modulus in `uint32_t`, carry-forward residues in
`int32_t`, and the additive log contribution in `uint16_t`. Rational and
non-projective algebraic factor-base entries entering that state require a
prime in `[2, INT32_MAX]`, so every active affine modulus is safely castable to
`int32_t`, and `log_p <= UINT16_MAX`. Carry-forward additions use
an `int64_t` intermediate before modular subtraction, including the
`INT32_MAX` boundary. Projective algebraic entries and the matching special-Q
are skipped before prime-entry construction. A configured algebraic sieve count
larger than the available factor base fails closed. Distributed work identity
validation enforces the same active-entry representation before rehydration.
When the canonical policy selects LLL with skew enabled, it also binds the
polynomial skewness to the positive finite-square domain required by SkewLLL;
Gauss and unskewed LLL identities retain the broader positive-finite domain.

## Validation

The primary Release-active checks are:

- `test_lll_lattice`: wide multiplication backends, `2^127` norm/dot cases,
  halfway rounding, determinant cancellation, exact `INT64_MIN` handling, and
  projection rejection;
- `test_lattice_sieve`: width 32768/32769 routing parity, extreme row offsets,
  real SkewLLL projection overflow, factor-base admission, and the
  `INT32_MAX` carry-forward boundary;
- `test_distributed_sieve_worker_execution`: special-Q-cap-aware initial and
  adaptive projection preflight before writer adoption;
- `test_distributed_sieve_worker_writer_authority`: full worker-facade phase
  and namespace proof that a failed preflight does not consume writer authority;
- `test_edge_cases`: inclusive region dimensions, endpoint round trips,
  default-area policy, and invalid geometry.

Run these through `scripts/test.sh`; the project runner owns Release
configuration and timeouts.
