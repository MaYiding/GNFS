# Hensel Denominator Recovery

`HenselSqrt` reconstructs an algebraic square root from modular lifts. For a
non-monic defining polynomial, multiplying by its derivative alone does not
always give integer coefficients. Increasing the lifting precision cannot
remove those coefficient denominators.

## Counterexample

Let $f(x)=2x^2+x+1$, $m=14$, and $N=407$. Then $f(m)=N$, and both the leading
coefficient and $f'(m)=57$ are invertible modulo $N$. Four copies of the
relation $(a,b)=(1,1)$ have the square root

$$
\beta=(1-\alpha)^2=\frac{1-5\alpha}{2},\qquad
S=\beta f'(\alpha)=\frac{11+9\alpha}{2}.
$$

The roots obtained from $\pm\beta$ are $169$ and $238$, whose squares equal $71$
modulo $407$. Centering residues of $S$ as integer coefficients fails even
with additional precision. A dependency containing 100 copies has the same
problem in the Chinese remainder theorem (CRT) reconstruction path.

## Sufficient Denominator

Assume $f$ is an irreducible integer polynomial of degree $d\geq1$, with
nonzero leading coefficient $c$, and the relation product is $P=\beta^2$ in
$K=\mathbb Q(\alpha)$. Let $k$ count factors with $b\ne0$ and put

$$
t=\lceil k/2\rceil,\qquad \theta=c\alpha,\qquad
F(X)=c^{d-1}f(X/c).
$$

The polynomial $F$ is monic and integral, so $\theta$ is integral. Write
$A$ for the product of the constant factors. Then

$$
(c^t\beta)^2=c^{2t-k}A\prod_{b_i\ne0}(ca_i-b_i\theta).
$$

Consequently $\gamma=c^t\beta$ is integral. For the order
$R=\mathbb Z[\theta]$, the trace dual is $R^\vee=F'(\theta)^{-1}R$.
Since the trace of $\gamma R$ is integral, $\gamma\in R^\vee$, and hence

$$
F'(\theta)\gamma=c^{t+d-2}\beta f'(\alpha)\in\mathbb Z[\theta]
\subseteq\mathbb Z[\alpha].
$$

Because $F$ is monic, the element on the left has a representative
$\sum_{j<d}z_j\theta^j$ with $z_j\in\mathbb Z$. Substituting $\theta=c\alpha$
gives integer coefficients $z_jc^j$ in a representative of degree below $d$.

This uses the power-order trace-dual identity in
[Keith Conrad, The Different Ideal, Theorem 3.7](https://kconrad.math.uconn.edu/blurbs/gradnumthy/different.pdf#page=7).
Thus a sufficient common denominator for the degree-below-$d$ representative
of $S$ is

$$
D=|c|^q,\qquad q=\max(0,\lceil k/2\rceil+d-2).
$$

The only negative unclamped exponent occurs when $d=1$ and $k=0$. In that
case $\beta\in\mathbb Q$ and $\beta^2\in\mathbb Z$, so $\beta$ is already
an integer. The bound is sufficient but need not be minimal. The four-relation
example only needs $D=2$, whereas the formula gives $D=4$.

## Reconstruction and Precision

The Newton iteration continues to lift the unscaled $S$. After the last
lift, multiply each coefficient by $D$ modulo that lift's modulus. The
single-prime path does this immediately before centering. The CRT path
scales all completed prime slots, including replacements, before forming
CRT residues or sign deltas.

Reconstruct the integral polynomial $T=DS$ and evaluate

$$
Y=T(m)(D f'(m))^{-1}\pmod N.
$$

If $B_S$ bounds the absolute coefficients of $S$, centered recovery requires
$M>2DB_S$. The precision estimate therefore includes
$q\log_2|c|+1$ in addition to the coefficient bound and safety margin.
The implementation computes powers only modulo $N$ or a lift modulus; it
does not allocate the full integer $D$. The exponent uses an arbitrary
precision integer for modular exponentiation, preserving its width on
Windows LLP64. The relation count and degree adjustment use checked
`size_t` arithmetic. For $c=\pm1$, scaling is unnecessary.

## Compatibility and Failure Behavior

The derivative must remain invertible modulo $N$. When $D$ is not
invertible, the solver retains the previous unscaled recovery path. Some
such products already have integral $S$; rejecting every nonunit leading
coefficient would discard those successes. This fallback has no general
denominator-recovery guarantee. Its exhausted CRT search does not suppress
the bounded precision retries or the classic fallback.

Every returned candidate must satisfy $Y^2\equiv P(m)\pmod N$. The optional
root result never carries a discovered gcd in place of a square root. Each
`compute()` call resets its prime and CRT diagnostics, including early
returns. Exhausted CRT signs record a bounded search failure, not a proof
that the dependency is mathematically invalid.

## Validation Scope

The Release-active cases in [test_sqrt.cpp](../../tests/test_sqrt.cpp) cover
classic and CRT reconstruction, odd relation counts, negative and wide
leading coefficients, linear and cubic fields, nonunit-leading
compatibility, noninvertible derivatives, nonsquares, and reused solver
state. The wider precision regressions also cover coefficients beyond the
finite range of `double`.

Validate with the sqrt module and pipeline gate or E2E. The sufficient
denominator grows with the number of nonconstant factors and can increase
lifting cost. Cross-size recovery experiments must distinguish known-square
fixtures from full factorization runs; successful reconstruction alone is
not evidence of complete large-input factorization or a performance gain.
