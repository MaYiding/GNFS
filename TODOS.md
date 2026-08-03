# GNFS Deferred Development

These items were considered during the structured relation-reduction review and are intentionally deferred until the in-process implementation produces measured evidence.

## Standalone distributed relation reducer

- **What:** Extract reduction into a standalone, checkpointable distributed executable and corpus protocol.
- **Why:** It could isolate very large filtering jobs and enable multi-host processing.
- **Deferred because:** The in-process engine, corpus boundary, and matrix-quality gain must be proven first; building a second infrastructure stack now would hide algorithm risk.
- **Depends on:** M5 scale evidence and a stable versioned `RelationCorpus` format.

## 3LP and cofactor-yield retuning

- **What:** Measure 3LP cofactorization and special-Q yield policies independently of filtering.
- **Why:** More usable relations may be necessary if dependency-preserving filtering alone cannot close the 50-digit gap.
- **Deferred because:** Combining yield and reduction changes would make causal attribution impossible.
- **Depends on:** M5 determining whether the bottleneck remains relation yield after structured reduction.

## Legacy V0/V3 retirement

- **What:** Remove duplicate legacy merge implementations and compatibility flags.
- **Why:** One promoted reducer would reduce maintenance and policy drift.
- **Deferred because:** V0/V3 remain required baselines and rollback paths during the opt-in measurement window.
- **Depends on:** M6 promotion criteria and an explicit compatibility window.

## Automatic reduction-policy tuning

- **What:** Select caps and strategy from measured corpus/matrix cost instead of fixed starting thresholds.
- **Why:** LP distributions change sharply across size bands.
- **Deferred because:** A policy cannot be calibrated before fixed 120-bit and 50-digit corpora establish stable measurements.
- **Depends on:** Reproducible M5 reports across several size bands.

## Cross-build distributed-sieve adoption

- **What:** Certify durable worker adoption across different executable byte
  identities that declare the same execution contract.
- **Why:** It would allow a deployment upgrade to finish an older wave without
  pinning the exact executable.
- **Deferred because:** The first durable-wave protocol deliberately requires an
  exact executable SHA-256; semantic compatibility needs its own mixed-build
  fixture and release policy.
- **Depends on:** The local same-executable crash suite and bounded 50-digit
  evidence for durable resume.

## Multi-wave durable sieve campaign and operator GC

- **What:** Add a general append-only campaign journal, authenticated fork/exec
  workers, remote scheduling, Windows launch, and an inspection-driven garbage
  collector for stale-valid waves.
- **Why:** It would turn the bounded local wave transaction into a reusable
  long-running distributed service.
- **Deferred because:** Those features add scheduling and trust boundaries that
  are not required to recover one local fork-based wave.
- **Depends on:** Stable manifest, handoff, merge-commit, and consumer-ACK
  formats plus measured recovery value.
