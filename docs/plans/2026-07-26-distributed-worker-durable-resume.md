# Distributed Sieve Durable Wave Resume

Status: implementation in progress (M0 complete, M1 underway)

Branch: `codex/parallel-structured-filter`

Mode: selective expansion

## Outcome

Make one distributed-sieve wave a bounded, durable transaction. If its master
process crashes, a later process can safely:

1. wait for inherited workers to become quiescent;
2. adopt exact completed chunks without running them again;
3. run only attempts that the durable journal still permits;
4. rebuild or reopen one deterministic merged relation corpus;
5. clean worker artifacts only after the merged corpus is durable; and
6. clean the merged corpus only after a durable downstream consumer
   acknowledges an exact successor.

Every successful visible relation payload remains the same ordered sequence,
count, and digest as an uninterrupted durable-policy execution. Retry exhaustion
is a terminal wave error: it never produces, merges, acknowledges, or cleans a
partial “successful” relation result. Durable progress fields have their own
stable oracle; current-process diagnostics such as PID, wait status, reap
source, and signal may legitimately differ after resume and are never part of
the semantic result. Durable resume is opt-in and requires an explicit stable
wave root. The existing PID-derived one-shot path remains the default until the
durable path has passed its crash suite and a downstream successor can be
reopened.

## Premise Challenge

The original proposal assumed that a durable merge receipt could safely precede
worker deletion. That premise is false because the current merge output is only
an in-memory `std::vector<Relation>`. A receipt proves what happened; it is not
the data needed to replay the result.

The corrected premise is:

- a durable `MERGE_COMMIT` is valid only when it binds a finalized durable
  merged OOC corpus;
- worker cleanup is authorized only by that exact commit;
- the merged corpus remains the last durable copy until a durable consumer ACK
  binds a reopenable successor; and
- no in-memory return, destructor, PID, or path string grants cleanup authority.

This increases the scope, but it closes the only design that can survive a crash
after partial cleanup without silently losing relations.

## Scope Alternatives

| Option | Completeness | Decision | Reason |
|---|---:|---|---|
| A. Add only `WORKER_HANDOFF` sidecars | 4/10 | Rejected | It leaves wrong-wave adoption, dual-master execution, retry reset, and post-merge data loss unresolved. |
| B. Bounded durable wave transaction | 10/10 | Accepted | It closes ownership, liveness, replay, cleanup, and downstream handoff without building a general scheduler. |
| C. General append-only distributed campaign platform | 10/10 | Deferred | It adds fork/exec, remote hosts, arbitrary waves, and a large journal before one local wave has measured value. |

The accepted scope deliberately reuses the existing private-lease, OOC V3,
`RelationCorpus`, SHA-256, and crash-test machinery. It adds only the records and
capabilities required for one resumable wave.

## What Already Exists

| Subproblem | Existing source | Reuse decision |
|---|---|---|
| Worker chunking, `fork`, `waitpid`, retry, deterministic merge | `src/sieve/distributed_sieve.cpp` | Keep orchestration; split durable reconciliation into a separate implementation unit. |
| Stable mathematical identity encoding | `include/gnfs/sieve/sieve_run_identity.hpp` | Reuse its explicit field-order approach, but use canonical SHA-256 and include actual distributed execution inputs. |
| OOC V3 paired store and descriptors | `include/gnfs/relation/ooc_relation_store.hpp` | Use for worker and merged corpora; strengthen adoption reads to native handle identity. |
| Private lease and exact cleanup authority | `include/gnfs/relation/ooc_cleanup_transaction.hpp` | Extend narrowly for an immutable no-delete auxiliary handoff and adoption capability. |
| Move-only OOC corpus ownership | `include/gnfs/relation/relation_corpus.hpp` | Return the durable merged corpus through a move-only wave result. |
| Relation sequence replay check | `include/gnfs/relation/relation_sequence_receipt.hpp` | Retain as a semantic ordering check, not as cryptographic authentication. |
| SHA-256 and canonical record builders | `include/gnfs/util/sha256.hpp` and SIQS record codecs | Reuse the primitives; do not copy SIQS campaign semantics into sieve. |
| Durable file publication patterns | `include/gnfs/util/durable_immutable_file.hpp` and SIQS journal stores | Extract the smallest reusable relative-dirfd immutable record publisher needed here. |
| Structured OOC downstream corpus | `RelationCorpus`, `RelationSink`, and structured reduction | Reuse the format, but require a deferred protected successor writer rather than an ordinary destructor-armed sink before pipeline ACK is enabled. |
| Crash-injection test pattern | `tests/test_ooc_cleanup_transaction.cpp` and SIQS self-exec tests | Reuse the core plus self-exec suite structure. |

## Accepted Scope

- A stable wave directory with a permanent wave lock and an immutable manifest.
- Canonical work and execution identities covering every output-affecting input.
- An immutable attempt chain with explicit retry-budget semantics.
- A no-delete worker handoff inside the exact private lease generation.
- Native-handle validation for adopted OOC artifacts.
- Deterministic adoption, missing-attempt execution, merge, and replay.
- A durable merged OOC corpus protected by the same deferred-handoff protocol
  and a self-contained `MERGE_COMMIT`.
- A move-only `DistributedSieveWaveResult` exposing only a non-armable,
  read-only merged-corpus view.
- A durable consumption transaction whose prepared successor precedes ACK.
- A terminal `WaveCompletedV1` record; V1 garbage collection removes only
  authorized artifacts and retains the permanent lock and immutable records.
- Idempotent worker and merged-corpus artifact cleanup with immutable metadata
  retained in V1.
- Observable dispositions proving whether each chunk was executed, adopted,
  empty, terminally failed, or recovered from a merge commit.
- A dedicated protocol/core/crash/integration test binary and Harness mapping.
- Opt-in pipeline integration only after the structured successor can be
  reopened from the ACK.

## NOT in Scope

- Network workers, untrusted hosts, or multi-machine scheduling.
- Windows support for the current POSIX `fork` worker pool.
- Adoption across executables whose SHA-256 or execution contract differs.
- A reusable job queue, arbitrary DAG, or multi-wave campaign journal.
- Automatic deletion of tainted, foreign, replaced, or stale-valid wave roots.
- Protection against a malicious same-user process that can rewrite every
  artifact and recompute its SHA-256.
- A UI or dashboard. Structured logs and deterministic inspection records are
  enough for this infrastructure milestone.
- Changes to GNFS mathematical filtering, relation acceptance, or matrix policy.

## Threat and Failure Boundary

The protocol defends against process death, power-loss publication windows,
accidental stale paths, partial writes, concurrent masters, symlink and hardlink
substitution, directory replacement, wrong configuration, wrong executable,
wrong lease generation, record corruption, and incomplete cleanup.

The protocol does not authenticate against an adversarial process running as the
same user with write access to the wave root. SHA-256 detects accidental drift
and binds records; it is not a MAC. Permissions, owner checks, single-link
checks, frozen directory descriptors, and no-follow opens reduce accidental and
cross-user substitution but do not create a hostile same-user trust boundary.

PID values are diagnostic only. They never authorize adoption, retry, or
deletion because PIDs can be reused and a new master is not the old worker's
parent.

## Durable Authority Ladder

Each record grants exactly one capability.

| Record or capability | Grants | Does not grant |
|---|---|---|
| `WaveManifestV1` | Exact wave identity and immutable chunk plan | Worker completion, cleanup, or retry success |
| Durable private-lease reservation | One exact attempt or successor generation | Consumption of an attempt ordinal |
| `AttemptStartedV1` | Consumption of one attempt ordinal | Proof that a child was forked or completed |
| `WorkerHandoffV1` | Read and adopt one exact finalized worker corpus | Deletion of the corpus |
| `ChunkTerminalFailureV1` | Terminal wave-error evidence after exact cleanup and exhausted budget | A partial successful result, merge, ACK, or cleanup of other chunks |
| Move-only adoption receipt | Current-process use of the exact lease and frozen handles under lock | Durable deletion after a crash |
| `ArtifactCleanupAuthorizedV1` plus typed validator receipt | Convert one exact protected handoff into cleanup intent | Cleanup of any other generation or artifact |
| `ArtifactCleanupCompletedV1` | Durable proof that one authorized artifact namespace is absent | Cleanup authority for another artifact |
| `MergeStartedV1` | One exact merged-build generation | Proof that merged output is finalized |
| `MergePreparedV1` | Reopen one exact finalized merged corpus and finish its commit | Worker or merged-corpus deletion |
| `WaveMergeCommitV1` | Reopen merged corpus and clean its exact worker inputs | Deletion of the merged corpus |
| `ConsumptionStartedV1` | One exact successor build generation | Proof that the successor is durable |
| `SuccessorPreparedV1` | Reopen the exact successor and finish ACK | Deletion of merged input or successor |
| `WaveConsumptionAckV1` | Reopen an exact durable successor and clean merged input | Deletion of a successor it does not own |
| `WaveCompletedV1` | Terminal classification after artifact cleanup | Deletion of retained protocol metadata or successor |
| `TaintedPreserved` classification | Fail-closed terminal diagnosis | Any mutation |

The publication order is:

```text
wave lock
  -> manifest canonical
  -> worker lease reservation durable
  -> attempt-start canonical
  -> fork
  -> worker OOC pair finalized
  -> worker handoff canonical
  -> inherited locks released
  -> adopted/read through native handles
  -> merged lease reservation durable
  -> merge-start canonical
  -> merged OOC corpus finalized
  -> merge-prepared handoff canonical
  -> merge commit canonical
  -> per-worker cleanup authorization/intent/completion
  -> successor lease reservation durable
  -> consumption-start canonical
  -> durable successor finalized
  -> successor-prepared handoff canonical
  -> consumption ACK canonical
  -> merged cleanup authorization/intent/completion
  -> completed record canonical
```

No step may infer a later authority from an earlier record.

The generic private-lease handoff is a dormant, application-bound capability.
The relation layer classifies and protects it but cannot activate deletion from
a path or descriptor alone. `DistributedSieveWaveStore` is the trusted
application validator: after it validates the full merge-commit or ACK chain,
it publishes `ArtifactCleanupAuthorizedV1` outside the target lease and mints a
move-only typed validator receipt bound to that record, lease generation,
handoff digest, native identities, and exact extents. A private generic
authority adapter lets the relation layer combine that receipt with the
matching adoption receipt without parsing sieve records.

Activating the combined capability first publishes and confirms the existing
canonical cleanup-intent mechanism under the same private-lease lock, using a
new versioned intent variant that names the external authorization digest.
Legacy intent bytes and behavior are unchanged and cannot be reinterpreted as a
handoff. The new durable intent then becomes the sole recovery authority; only
afterward is the handoff consumed. Ordinary idempotent cleanup may continue
across processes. After exact namespace removal and parent sync,
the wave store publishes `ArtifactCleanupCompletedV1` outside the lease. If a
crash removes the directory before this completion record, the surviving
authorization plus exact absence permits only that completion publication.
Absence without the matching external authorization is never treated as
successful cleanup.

Implementation is intentionally split at the authority boundary. M1.7a freezes
the pure V2 marker codec without wiring it into cleanup recovery. The fixed
little-endian record binds the frozen native-path digest, external
authorization digest, generic-handoff self-digest, complete lease and V3 pair,
canonical handoff snapshot, optional duplicate-pending observation, and
index/data bindings. `Intent` and `Staged` are distinct digest-bound marker
kinds, and decoding requires the expected kind. Copying bytes between their
leaves therefore fails at the codec boundary. An all-zero SHA-256 value remains
a present value, not an absence sentinel. M1.7b alone may combine a
WaveStore-only typed authorization receipt with a fresh locked adoption
receipt. Before canonical V2 publication, it must remove any exact observed
duplicate pending leaf, sync its parent, reconfirm absence, and revalidate the
canonical handoff and pair. Canonical runtime markers require an absent
`pending_handoff`; any later pending leaf taints recovery and is never
reclaimed. The converter may then publish canonical V2, spend both
capabilities, and consume the canonical handoff. Until M1.7b lands, legacy
runtime entry points reject V2 bytes without mutation.

## Wave Namespace and Locking

Durable mode requires:

- `resume_enabled = true`;
- a nonempty explicit `base_path`;
- an absolute, frozen path with no embedded NUL; and
- a new or exact matching durable wave directory.

The default PID-derived base remains one-shot and never participates in durable
adoption.

The wave directory is created relative to a frozen parent directory descriptor.
Creation is followed by parent-directory synchronization and identity
validation. The directory contains only schema-approved leaves and exact
per-attempt private lease directories. Unknown leaves produce
`TaintedPreserved`; the implementation does not guess or delete them.

The permanent `wave.lock` is opened with no-follow semantics, verified as a
regular single-link file owned by the current user, and never unlinked during
normal reuse. Its native identity is frozen into `WaveManifestV1` and
`WaveCompletedV1`. A master acquires an inherited-open-description lock
(`flock`, or a platform adapter with identical fork semantics) before reading
or publishing the manifest, then proves that the open handle and the canonical
`wave.lock` leaf are still the manifest-bound object. All children inherit that
one locked open-file description and close it only on exit. Classic
process-associated `fcntl` locks are not an acceptable implementation.

Consequences:

- a new master receives `Busy` while the old master or any old child remains
  alive;
- two resumers cannot merge or clean the same wave concurrently;
- once a new master owns the lock, every old child is quiescent; and
- no PID probing or `waitpid` call is needed for cross-master liveness.

If an old child still holds the old lock inode while the `wave.lock` path has
been replaced, a new process may lock the replacement inode but must fail the
manifest identity check as `TaintedPreserved` before any mutation. The same
double-snapshot rule applies to root-directory replacement.

Within the wave lock, private leases are acquired in ascending chunk ID and
attempt ordinal. A code path must never acquire the wave lock while already
holding a private lease lock. After `fork`, a child immediately closes every
inherited descriptor except the wave lock, its own lease/native handles, and
its report channel; unrelated lease and report descriptors must not extend
liveness accidentally.

## Immutable Publication Contract

Manifest, attempt, handoff, merge, consumption, ACK, and completion records use
one relative-dirfd publisher:

```text
openat(pending, O_CREAT | O_EXCL | O_NOFOLLOW)
  -> verify regular file, owner, mode, nlink == 1
  -> complete write
  -> fsync(file)
  -> fsync(parent)
  -> revalidate native identity and exact bytes
  -> rename-no-replace pending to canonical
  -> fsync(parent)
  -> reopen canonical with O_NOFOLLOW
  -> verify same identity and exact bytes
```

If rename is visible but the parent synchronization result is uncertain,
recovery reopens, validates, and re-establishes the directory durability barrier
before recognizing the canonical record. An unrecognized pending record never
authorizes work or deletion.

The no-replace step is supplied by a platform adapter
(`renameat2(RENAME_NOREPLACE)`, `renameatx_np(RENAME_EXCL)`, or an equivalently
tested link-based publication); plain overwriting `rename()` is forbidden.
Unsupported platforms fail closed before publication.

V1 freezes the namespace policy:

- wave, attempt, merged, and successor private directories are mode `0700`;
- lock and regular protocol records are mode `0600`;
- every protocol-owned object is owned by the effective UID, has the expected
  type and link count, and is checked with a `07777` mask so setuid, setgid, and
  sticky bits cannot hide behind a `0777` comparison;
- the direct parent is a directory owned by the effective UID and is not group
  or world writable; sticky-root parent semantics are not supported in V1; and
- protocol roots, leaves, and their direct parent must have no effective
  extended ACL that broadens write authority. A platform without a trustworthy
  ACL inspection adapter returns `PlatformUnavailable`.

Creation uses restrictive modes independent of `umask`, then reopens and
revalidates exact mode, owner, ACL state, link count, and native identity.

This follows the POSIX distinction between namespace atomicity and persistence:
`rename()` can atomically change names, while durable recovery still requires
the relevant file and directory synchronization barriers.

## Protocol Records

Record placement is fixed:

- the wave root retains manifest, worker-attempt, terminal-failure,
  merge-start, merge-commit, cleanup authorization/completion,
  consumption-start, ACK, and completed records;
- exact private leases contain the generic protected handoff whose opaque
  payload is `WorkerHandoffV1`, `MergePreparedV1`, or
  `SuccessorPreparedV1`, plus versioned cleanup-intent state; and
- every repeated start has an ordinal in its canonical filename and predecessor
  digest. Old generations are expected immutable leaves, not “latest file”
  competitors.

### `WaveManifestV1`

The manifest freezes:

- schema and wire versions;
- random 128-bit `wave_id`;
- execution-contract version;
- executable SHA-256;
- work SHA-256;
- wave-root native identity;
- permanent lock native identity and lock-semantics version;
- exact effective special-Q range;
- worker count and ordered chunk table;
- each chunk's ID, half-open range, and relative artifact stem;
- per-worker SQ and relation caps;
- maximum worker, merge-build, and consumption attempts; canonical
  generation-local naming; and the rule that a durable start consumes its
  ordinal even if the process dies before execution;
- OOC, relation serialization, handoff, receipt, digest, and merge-policy
  versions; and
- its own canonical digest.

An existing manifest with any mismatch returns `ConfigMismatch`, preserves the
entire root, and starts zero workers. A caller chooses a new explicit wave root
instead of overwriting stale-valid work.

### `AttemptStartedV1`

Each nonempty chunk has a bounded immutable predecessor chain:

```text
attempt 0 -> attempt 1 -> ... -> terminal handoff or terminal failure
```

Each record binds:

- manifest digest;
- chunk ID and exact range;
- attempt ordinal;
- predecessor digest or the manifest digest for ordinal zero;
- the already-reserved private lease generation, owner-marker identity,
  directory native identity, and relative stem;
- retry-policy version; and
- record digest.

The exact lease is durably reserved before this record is published. A crash
after reservation but before `AttemptStartedV1` performs the existing exact
preactive rollback and consumes no attempt. The start record is durable before
`fork`; a crash after publication and before `fork` consumes that attempt. This
rule is conservative, bounded, and identical on every replay. Missing attempts
may be started only when the chain and budget permit.

### `ChunkTerminalFailureV1`

The terminal record binds:

- manifest, chunk, and exact range;
- exhausted attempt count, last attempt digest, and predecessor-chain digest;
- normalized terminal reason and any trustworthy exit/wait facts;
- confirmation that no canonical handoff exists;
- confirmation that the exact attempt lease has converged to clean absence;
- normalized committed progress (`next_sq_index = range_begin`,
  `processed_sq_count = 0`) and zero-row statistics; and
- record digest.

It may be published only while holding the wave lock, after the last attempt is
quiescent, after predecessor durability is reconfirmed, and after exact cleanup
has completed. An uncertain wait becomes the normalized
`NoHandoffAfterInheritedLockQuiescence` reason only when a later master has
acquired the inherited wave lock. A present, ambiguous, or failed-cleanup lease
blocks terminal publication. Uncommitted partial files, pipe reports, or
current-parent diagnostics never contribute rows or semantic progress.

### `WorkerHandoffV1`

The handoff is the opaque application payload of a generic immutable
`OOCPrivateHandoffV1` leaf inside the exact worker private lease, which prevents
retry-generation ABA. It binds:

- manifest and work digests;
- wave, chunk, range, attempt, lease, owner-marker, and directory identities;
- finalized OOC descriptor;
- native identities and exact extents of the index and data files;
- relation sequence receipt;
- order-sensitive corpus SHA-256;
- assigned range, actual processed SQ count, next SQ index, and completion
  reason (`RangeExhausted`, `SqCap`, `RelationCap`, or `ZeroRelations`);
- relation count;
- cleanup-intent absence on the successful no-delete path; and
- record digest.

The existing success path must stop publishing cleanup intent. Finalization and
canonical handoff publication occur under the same private `BaseLock`. Recovery
classifies the exact `RESERVED + OWNED + finalized pair + canonical handoff`
state as `HandoffPresent`, not as preactive rollback. This classification is
performed atomically under that same lock before `reserve_private_lease()` may
recover or reserve anything:

- no handoff or an exact pending-only handoff remains preactive and may roll
  back the exact pair;
- a valid canonical handoff is protected and may only be adopted;
- a corrupt, replaced, or foreign canonical/pending handoff is
  `TaintedPreserved`; and
- `remove_private_lease()` remains creator-bound, while cross-process adoption
  uses a new receipt issued only by locked `HandoffPresent` classification.

Canonical publication is also a durable phase transition:

1. while `RESERVED` still exists, publish and confirm the canonical handoff;
2. from that instant, every cleanup/reservation entry point gives the handoff
   priority and rejects stale private-lease receipts;
3. consume the fresh writer cleanup receipt and durably remove/replace
   `RESERVED`, leaving the canonical handoff as the adoptable phase marker; and
4. on a later typed cleanup authorization, publish canonical cleanup intent
   before consuming the handoff.

A crash between steps 1 and 3 is still protected because the classifier sees
the canonical handoff before `RESERVED`; a stale in-memory private-lease receipt
or writer receipt cannot initiate cleanup. Once cleanup intent is canonical it
dominates both handoff forms and is the only cross-process deletion authority.
Merely adding a leaf to the existing allowlist is explicitly insufficient.

### `ArtifactCleanupAuthorizedV1` and `ArtifactCleanupCompletedV1`

The authorization record is outside the lease being deleted and binds:

- authorizer kind (`MergeCommitWorker` or `ConsumptionAckMerged`);
- manifest plus merge-commit or ACK digest;
- artifact kind and manifest-order ordinal;
- exact private directory, owner marker, lease generation, handoff digest,
  OOC descriptor, native identities, and extents; and
- record digest.

The completion record binds the authorization digest, the exact cleanup-intent
identity when one was published, parent-directory durability confirmation,
expected namespace absence, and record digest. Per-worker authorization and
completion records are published in manifest order. The merge commit remains
valid after worker handoffs disappear because it is self-contained and each
legitimate absence is explained by its external completion record.

Every intra-lease prefix is recoverable and tested: authorization canonical,
cleanup-intent pending/canonical, handoff present/consumed, index/data staging
and removal, owner/lease-marker removal, private-directory removal, and
external completion publication.

### `MergeStartedV1`

After reserving the merged private lease and before writing output, the wave
store publishes a start record binding the manifest/work digests, complete
ordered successful input summary, merge-policy version, and the actual merged
lease generation/owner/directory identity. It is a wave-root immutable record
whose canonical relative name includes a bounded `merge_attempt_ordinal`. It
also binds the previous merge-start digest, or the manifest digest for ordinal
zero.

Reservation without this record is preactive and consumes no merge attempt. A
start without preparation consumes its ordinal and authorizes cleanup of only
that exact incomplete generation. After cleanup completion, the next ordinal
may reserve a new generation and link to the predecessor. Old start records
remain schema-approved immutable leaves and never block the next generation.
Gaps, duplicates, conflicting prepared generations, or exhaustion of the
manifest's merge-attempt budget preserve every worker handoff and return a
terminal merge error; they do not publish a commit or clean workers.

### `MergePreparedV1`

The merged writer uses a separate private lease in deferred-handoff mode; it
must not use an ordinary immediately armed `RelationSink`. Its prepared record
binds:

- manifest/work digests and merge-policy version;
- `MergeStartedV1` digest;
- the ordered terminal input summary;
- input, duplicate, output, and per-chunk retained counts;
- finalized merged OOC descriptor, exact extents, and native identities;
- merged sequence receipt and order-sensitive corpus SHA-256;
- merged lease generation, owner-marker, and directory identity; and
- record digest.

No canonical prepared handoff means the exact provisional merged lease may
perform preactive rollback. A canonical prepared handoff is protected: recovery
reopens it through the same native handles and completes
`WaveMergeCommitV1`; it does not delete and rebuild a valid prepared corpus.

### `WaveMergeCommitV1`

The commit binds:

- manifest and work digests;
- for every chunk in manifest order: successful disposition (`Handoff` or
  `Empty`), assigned SQ
  begin/end, next SQ index, processed SQ count, completion reason, durable
  attempt count and last-attempt digest, lease generation, handoff digest, raw
  count, retained count, and normalized diagnostics;
- the exact `first-ABPair` and chunk-order merge-policy version;
- input, duplicate, and output counts;
- `MergePreparedV1` digest and merged private-lease identity;
- finalized merged OOC descriptor;
- merged index and data native identities and extents;
- merged relation sequence receipt;
- order-sensitive merged-corpus SHA-256; and
- record digest.

The merged corpus and prepared handoff are finalized and revalidated before the
commit is published. If the process dies before preparation, all worker
handoffs remain and the merge is deterministically repeatable. If it dies after
preparation, recovery finishes the commit from that protected corpus. Once the
commit is canonical, it is self-contained: validation permits worker handoff
and lease absence for purposes of reopening the relation result and complete
worker summary. Normal cleanup still requires its external
authorization/completion chain; unexplained absence blocks further mutation and
completion but never makes the already committed merged result unreadable.
Worker cleanup is therefore idempotent at every authorized prefix.

The caller-facing result receives only a same-handle, descriptor-bound
`ReadOnlyRelationCorpusView`. It carries no fresh-writer receipt, adoption
receipt, private-lease receipt, `arm_ooc_cleanup()` route, or generic cleanup
token. The durable handoff remains the only dormant merged cleanup capability,
and the wave store can activate it only after ACK.

### `ConsumptionStartedV1`

Before downstream work begins, the consumer reserves a deferred/no-delete
successor lease and publishes a start record binding:

- merge-commit and manifest digests;
- consumer kind and execution-contract version;
- already-reserved successor lease generation, owner-marker, directory
  identity, and deterministic relative stem;
- successor format/version; and
- record digest.

A V1 ACK supports only the explicitly versioned
`StructuredReductionRelationCorpusV1` OOC successor. Other pipeline outputs,
RAM vectors, or arbitrary external formats are rejected rather than accepted
through a generic descriptor.

Like merge starts, consumption starts are wave-root immutable predecessor
records whose canonical names include a bounded `consumption_attempt_ordinal`.
A crash after reservation but before this record rolls back the exact preactive
lease and consumes no ordinal. A crash after the record but before preparation
consumes that ordinal and may clean only that exact incomplete successor; after
cleanup the next ordinal links to the old start and reserves a new generation.
Old starts remain allowed audit leaves. Gaps, conflicting prepared successors,
or budget exhaustion leave the merged corpus in `OutputHeld` and publish no ACK.

### `SuccessorPreparedV1`

After the consumer finalizes the successor, it publishes a protected handoff
binding the consumption-start digest, exact reopenable successor descriptor,
native identities, sequence or semantic digest, counts, and record digest. A
crash after this record and before ACK resumes by validating the prepared
successor and finishing ACK; it does not rebuild or arm destructor cleanup. The
successor remains in `Preserve` state after ACK and is not cleanup authority for
itself.

### `WaveConsumptionAckV1`

The ACK binds:

- merge-commit digest;
- a versioned consumer kind;
- consumption-start and successor-prepared digests;
- an exact reopenable successor descriptor and native identities;
- successor sequence or semantic digest;
- successor cleanup authority identity; and
- record digest.

Only a consumer that has already finalized and validated that successor may
publish the ACK. A RAM vector is not a successor. Until pipeline resume can
reopen the successor, durable distributed mode may return the merged corpus but
must preserve it and must not emit an ACK.

### `WaveCompletedV1`

After ACK and successor revalidation, recovery consumes the ACK-derived
authorization to clean the exact merged handoff. Once merged absence and every
worker cleanup prefix are confirmed, it publishes a self-contained completed
record binding:

- root, permanent lock, manifest, merge-commit, ACK, and successor-prepared
  identities/digests;
- the successful per-chunk completion summary copied from the merge commit;
- exact artifact-cleanup confirmations;
- successor descriptor and semantic digest; and
- record digest.

V1 then stops. It retains `wave.lock`, the manifest, attempts, terminal-failure
records, merge commit, consumption records, ACK, and completed record. Only
exact OOC/private-lease artifacts are garbage-collected. General metadata
compaction and deletion of completed roots remain a future operator-GC
protocol, so V1 never destroys the records needed to explain completion.

## Work Identity Closure

The canonical work preimage uses domain-separated, explicit little-endian
fields and length-prefixed integer/string encodings. It never hashes object
representations, filesystem paths, PIDs, timestamps, unordered-container
iteration order, or host-endian serialization.

| Group | Included fields |
|---|---|
| Polynomial | `n`, `m`, degree, coefficient count and every coefficient by index, exact IEEE-754 skewness bits |
| Factor-base parameters | rational bound, algebraic bound, large-prime bound, log scale |
| Factor-base contents | ordered rational `(p, log_p)`, ordered algebraic `(p, r, log_p, degree)`, sieve algebraic count |
| Sieve parameters | log scale, rational threshold, algebraic threshold, large-prime bound, 2LP and 3LP flags |
| Sieve region | `i_min`, `i_max`, `j_min`, `j_max` |
| Cofactorizer | large-prime bound, 1LP/2LP/3LP flags, maximum factorization attempts |
| Range | original and effective special-Q bounds |
| Distributed policy | worker count, ordered chunks, SQ cap, relation cap, and separate worker/merge-build/consumption attempt budgets |
| Frozen runtime policy | canonical `DistributedSieveExecutionPolicyV1`, including every classified ambient setting reachable from sieve/cofactor execution |
| Semantics | relation serialization, OOC, digest, handoff, retry, chunking, completion, deduplication, and merge-policy versions |

`n` and `m` passed separately to `run_distributed_sieve` must exactly equal the
polynomial context before any namespace mutation.

`DistributedSieveExecutionPolicyV1` is parsed and range-checked before any
callback, namespace mutation, or `fork`. The current inventory is frozen as
follows:

| Classification | Environment settings |
|---|---|
| Relation/control semantic | `GNFS_LATTICE_LLL`, `GNFS_LATTICE_SKEW`, `GNFS_ADAPTIVE_LATTICE`, `GNFS_ADAPTIVE_LATTICE_THRESHOLD`, `GNFS_ADAPTIVE_LATTICE_MAX_RETRIES`, `GNFS_ADAPTIVE_LATTICE_SEED`, `GNFS_SURVIVAL_FILTER`, `GNFS_SURVIVAL_THRESHOLD`, `GNFS_COFACTOR_BRENT`, `GNFS_ECM_BRENT_SUYAMA`, `GNFS_ECM_BS_DEGREE`, `GNFS_ECM_SIGMA_POOL_SIZE`, `GNFS_ECM_CURVE_POOL` |
| Conservatively identity-bound until parity is proven | `GNFS_ECM_BATCH_INV`, `GNFS_COFACTOR_BATCH_SIZE`, `GNFS_BRENT_POLLARD_RHO_THREADS`, `GNFS_ECM_B1_CACHE_SIZE`, `GNFS_ECM_STAGE1_PARALLEL_THREADS`, `GNFS_ECM_STAGE2_PARALLEL`, `GNFS_COFACTOR_RESULT_CACHE_SIZE`, `GNFS_TRIAL_DIV_SIMD`, `GNFS_LATTICE_BASIS_PARALLEL_THREADS`, `GNFS_LATTICE_COORDS_SIMD`, `GNFS_SIEVE_APPLY_TILE_THREADS`, `GNFS_BUCKET_PREFETCH`, `GNFS_SIEVE_ECORE_THREADS`, `GNFS_SIEVE_NO_TINY_SIMD`, `GNFS_SIEVE_NORM_TILE_BITS`, `GNFS_SIEVE_REGION_TILE_BITS`, `GNFS_SIEVE_SATURATED_SUB_SIMD`, `GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD` |
| Diagnostic-only, excluded from semantic identity | `GNFS_COFACTOR_TIMING_ENABLE` |
| Distributed namespace/plan | `GNFS_DISTRIBUTED_SIEVE_WORKERS`, `GNFS_DISTRIBUTED_SIEVE_BASE_PATH`, `GNFS_DISTRIBUTED_SIEVE_SQ_PER_WORKER`; parsed into config, with output-affecting fields bound by the manifest and the root bound by native identity |
| Test-only | fail/corrupt/crash injection variables are replaced in durable mode by explicit nonproduction test hooks and never enter a production worker via `getenv` |

The conservative group remains hashed even when a field is expected to be
performance-only. A field may move to the excluded group only after
bit-for-bit relation parity across off/on settings and a code review showing it
cannot alter attempt ordering, random consumption, factor choice, or success
limits.

Dynamic environment reads are forbidden after the policy is frozen. Durable
workers and every reachable sieve/cofactor helper receive the immutable policy
explicitly; a durable call path may not fall back to a default constructor that
calls `getenv`. A source inventory checker scans `include/gnfs/sieve`,
`include/gnfs/cofactor`, `src/sieve`, and `src/cofactor` and fails on every new
`GNFS_*` read until it is classified. Field-drift tests mutate each normalized
policy value independently, and static/runtime guards prove the durable worker
path performs zero environment reads after freeze.

Randomness is part of the execution contract, not ambient process state.
Durable mode removes `random_device`, time, PID, scheduler order, cache history,
and attempt ordinal from every output-affecting seed. It derives
domain-separated seeds from the canonical work preimage plus stable chunk, SQ,
candidate/cofactor input, and algorithm identities. Adaptive-lattice choices,
ECM sigma/curve pools, Pollard-rho choices, and any later randomized helper must
consume this explicit deterministic source. Retrying the same chunk therefore
serializes the same relation sequence; tests compare full serialized rows and
semantic progress, not only `ABPair` membership. The legacy one-shot route may
keep its historical random policy, so parity baselines for this feature use an
uninterrupted durable execution under the same frozen policy.

Each durable child also constructs a fresh per-worker execution context after
fork. Mutable process-global cache warmth, inherited random engines, and prior
candidate history may accelerate neither control decisions nor result choice;
performance caches must be reset, made context-local, or mechanically proven
semantic-neutral. Hot/cold cache and different scheduling tests use the same
serialized-relation oracle.

## State Classification

| Classification | Evidence | Action |
|---|---|---|
| `NoWave` | No canonical manifest and no exact owned pending record | Publish a new manifest before any attempt. |
| `WavePending` | Exact pending manifest only | Reconcile publication; never fork from pending state. |
| `Busy` | Wave lock cannot be acquired | Return or wait by explicit caller policy; never adopt, clean, or rerun. |
| `LeaseReservedPrestart` | Exact reserved lease but no matching attempt record or canonical handoff | Perform exact preactive rollback; consume no attempt. |
| `ExactOwnedMissing` | Matching manifest and a chunk with available budget but no current lease or terminal record | Reserve its next lease, publish the matching attempt, and run only that chunk. |
| `HandoffPresent` | Exact canonical generic/sieve handoff classified under the private `BaseLock` | Block preactive rollback and issue a cross-process move-only adoption receipt. |
| `Adoptable` | `HandoffPresent` plus same-handle descriptor, identity, sequence, and corpus validation | Include the chunk exactly once. |
| `CleanupOnly` | Exact owned abandoned/failed lease with no canonical handoff | Use preactive recovery, then continue according to the durable attempt chain. |
| `TerminalFailed` | Any exact immutable chunk-failure record at exhausted budget | Return a terminal wave error; preserve every completed handoff and publish no merge, ACK, or cleanup of successful artifacts. |
| `MergeBuilding` | Every nonempty chunk has a valid handoff, no terminal failure, and no valid merge commit | Preserve worker inputs and deterministically build or rebuild merged output. |
| `MergeStarted` | Exact merged lease and start exist, no canonical prepared handoff | Recover/clean only that incomplete generation, then rebuild. |
| `MergeRetryExhausted` | Contiguous merge-start chain consumes the manifest budget without preparation | Return terminal merge error and preserve all worker handoffs. |
| `MergePrepared` | Exact protected merged handoff validates but no commit exists | Publish the self-contained commit from the prepared corpus. |
| `MergeCommitted` | Commit and merged corpus both validate | Reopen merged output and idempotently finish worker cleanup. |
| `ArtifactCleanupAuthorized` | Exact external authorization exists, completion absent | Install/resume the intra-lease intent or, after authorized absence, publish completion. |
| `OutputHeld` | Worker cleanup complete, no valid consumer ACK | Return or reopen merged corpus; preserve it. |
| `ConsumptionBuilding` | Exact consumption start with no prepared successor | Recover or clean only its exact incomplete successor and retry consumption. |
| `ConsumptionRetryExhausted` | Contiguous consumption-start chain consumes the manifest budget without preparation | Keep the merged result `OutputHeld`; publish no ACK or cleanup. |
| `SuccessorPrepared` | Protected exact successor validates but no ACK exists | Publish ACK from the prepared successor without rebuilding. |
| `Consumed` | ACK and exact successor both validate | Reopen successor and idempotently clean the exact merged handoff. |
| `Completed` | `WaveCompletedV1`, retained records, successor, and artifact-absence proofs validate | Return terminal summary; mutate nothing. |
| `ConfigMismatch` | Valid manifest but requested work or executable differs | Preserve the root and start zero workers. |
| `StaleValid` | Valid completed wave under a different explicit namespace decision | Preserve it; require the caller to choose a new root or explicitly consume it. |
| `TaintedPreserved` | Unknown leaf, conflict, replacement, link, corrupt record, or uncertain identity | Stop the whole wave without mutation. |

`TaintedPreserved` is convergence: the protocol has reached a safe terminal
diagnosis. Convergence never means deleting an artifact whose ownership cannot
be proved.

## State Machine

```text
                       +--------------------+
                       | TaintedPreserved   |
                       +--------------------+
                          ^   ^   ^   ^
                          |   |   |   +---- unknown or replaced artifact
                          |   |   +-------- invalid native identity
                          |   +------------ record/digest conflict
                          +---------------- configuration ambiguity

NoWave -> WavePending -> WaveReady
                         |
                         v
                 LeaseReserved
                         |
                         +---- crash before start ----> CleanupOnly
                         |
                         v
                AttemptStarted ----> CleanupOnly ----> next attempt
                         |                  |
                         v                  +---------> TerminalFailed
                      child
                         |
                         v
                  HandoffPresent
                         |
                         v
                     Adoptable
                         |
                         v
                  MergeBuilding
                         |
                         v
                   MergeStarted
                    |    |
                    |    +---- budget exhausted ----> MergeRetryExhausted
                    |
                         v
                  MergePrepared
                         |
                         v
                 MergeCommitted
                         |
                         +----> worker authorization/intent/completion tail
                         |
                         v
                     OutputHeld
                         |
                         v
               ConsumptionStarted
                    |    |
                    |    +---- budget exhausted ----> ConsumptionRetryExhausted
                    |
                         v
                SuccessorPrepared
                         |
                         v
                     Consumed
                         |
                         v
                 merged GC tail
                         |
                         v
                  WaveCompleted
```

The wave lock surrounds classification and every transition. A process crash
does not create an implicit transition; the next owner derives the state again
from canonical records and exact files.

## Normative Crash Invariant

At every durable prefix, at least one complete relation result and the record
that classifies its authority remain independently reopenable:

```text
worker handoffs -> merge start -> merge prepared -> merge commit -> worker cleanup
       | crash: reopen handoffs      | crash: reopen merged corpus + commit

merged corpus -> consumption start -> successor prepared -> ACK -> merged cleanup
       | crash: preserve/adopt both       | crash: reopen successor + ACK

cleanup completions -> WaveCompletedV1 -> retained immutable metadata
       | crash: authorization explains absence and successor remains reopenable
```

The previous durable result and its classification record survive until the
next result is finalized, independently reopenable, and named by a canonical
record. A cleanup intent cannot be published before that boundary. A missing
artifact without a surviving authorization/completion chain is corruption, not
implicit progress.

## Data Flows

### Fresh Happy Path

1. Freeze runtime policy, validate arguments, compute executable and work
   identities.
2. Open the stable root and acquire the wave lock.
3. Publish and revalidate the manifest.
4. For each nonempty chunk, durably reserve its exact lease, publish attempt
   zero binding that generation, then `fork`.
5. Each worker finalizes its private OOC pair and atomically publishes a
   no-delete handoff while holding the private lock.
6. The master reaps its children; after every inherited holder closes the wave
   lock, it validates each handoff through native handles.
7. Stream chunks in manifest order into a merged OOC sink while applying the
   versioned first-`ABPair` policy, after reserving a separate deferred merged
   lease and publishing `MergeStartedV1`.
8. Finalize the merged corpus, publish and validate `MergePreparedV1`, then
   publish the self-contained merge commit.
9. For each worker in manifest order, publish external cleanup authorization,
   validate it into a move-only capability, consume the exact handoff through
   canonical intent, then publish external cleanup completion.
10. Return `DistributedSieveWaveResult` owning or reopening the merged corpus.
11. Preserve merged output unless a durable consumer completes
    start -> prepared successor -> ACK.
12. After ACK, publish the merged cleanup authorization, consume the exact
    merged handoff through canonical intent, publish cleanup completion, and
    then publish `WaveCompletedV1`; retain all protocol metadata.

### Resume After Master Death

1. A new master opens the exact root.
2. If an old process still owns the inherited wave lock, return `Busy`.
3. After acquisition, validate manifest and execution/work identity.
4. Reconcile every chunk from its exact attempt chain.
5. Under each private lock, protect canonical handoffs, clean exact abandoned
   preactive attempts, and run only journal-permitted missing attempts.
6. Rebuild only an incomplete merge; finish a commit from valid
   `MergePreparedV1`, or reopen an existing self-contained commit.
7. Continue worker cleanup, consumption preparation, ACK, merged cleanup, and
   completion from the exact durable tail.

### Nil and Empty Cases

- `num_workers == 0` and empty base path fail before filesystem mutation.
- An empty effective special-Q range returns an empty non-durable result,
  clears optional statistics, and creates no wave root.
- Workers greater than special-Q count still produce a complete manifest.
  Zero-length chunks are `Empty`, require no attempt or handoff, and never fork.
- A nonempty chunk producing zero relations publishes a valid zero-row handoff;
  it is not treated as missing.
- A null statistics pointer changes no state or cleanup behavior.

### Terminal Failure

The retry budget is part of the manifest. Lease reservation alone consumes no
ordinal; each durable start does. Once the budget is exhausted without a
handoff, the last lease must be quiescent and exactly cleaned before a
terminal-failure record freezes the diagnostic state. The failed attempt's
preactive lease cleanup is a prerequisite to publishing that record. The record
then makes the whole wave a terminal error: no merge start/prepare/commit,
cleanup of successful handoffs, consumption record, ACK, or completed-success
record may follow it. Existing handoffs and immutable records are preserved for
diagnosis; a caller must choose a new explicit root and budget for a new wave.
Therefore every successful wave contains every nonempty chunk and satisfies
uninterrupted durable-policy payload parity.

## Error and Rescue Registry

| Failure | Detection | Rescue | Mutation allowed |
|---|---|---|---|
| Wave lock busy | Nonblocking exclusive lock fails | Return `Busy` or bounded caller wait | None |
| Root or lock replaced | Open/path identity differs from manifest while holding replacement lock | Stop as `TaintedPreserved` | None |
| Manifest pending | Exact pending identity and bytes | Re-establish durability or discard only exact owned pending | Exact pending only |
| Manifest mismatch | Canonical digest or fields differ | Use a new explicit root | None |
| Lease reserved before attempt | Exact reservation has no matching durable start/handoff | Preactive rollback; reserve again if needed | Exact reserved generation only |
| Attempt start without child | Durable attempt has no handoff and inherited lock proves quiescence | Consume ordinal; recover exact lease; start next allowed attempt | Exact owned attempt |
| Worker dies before finalization | No handoff; OOC/lease state exact owned | Existing private recovery, then next attempt | Exact pair only |
| Handoff pending | No canonical handoff | Treat as incomplete; exact recovery after quiescence | Exact pending only |
| Handoff canonical | Locked `HandoffPresent` classifier validates exact pair | Protect and adopt; block ordinary reserve/recovery rollback | None |
| Handoff corrupt or foreign | Codec, digest, identity, link, owner, or extent check fails | Stop as `TaintedPreserved` | None |
| OOC content drift | Handle identity, descriptor, sequence, or SHA-256 differs | Stop as `TaintedPreserved` | None |
| Last attempt exhausted | No handoff, quiescent exact lease, cleanup complete, predecessor reconfirmed | Publish `ChunkTerminalFailureV1`; return terminal wave error and preserve all other handoffs | Terminal record only |
| Merge reservation before start | Exact merged reservation, no start/prepared record | Preactive rollback; consume no merge generation | Exact reservation only |
| Merge dies after start, before preparation | Matching generation-local start, no canonical prepared handoff | Clean only exact incomplete merged lease; publish next linked start if budget remains | Exact provisional merge |
| Merge-attempt budget exhausted | Valid contiguous start chain, no preparation | Return terminal merge error; preserve every worker handoff | None |
| Merge prepared, no commit | Protected prepared handoff validates | Publish commit from prepared corpus | Commit record only |
| Commit exists, merged output invalid | Descriptor or digest mismatch | Stop as `TaintedPreserved` | None |
| Cleanup dies before intent | External authorization validates, completion absent | Re-mint typed receipt and install exact intent | Bound artifact only |
| Cleanup dies inside lease | Canonical intent names external authorization | Idempotently continue every intra-lease prefix | Bound artifact only |
| Lease disappears before completion record | External authorization survives and exact path is absent | Reconfirm parent durability and publish completion | Completion record only |
| Lease absent without authorization/completion | No surviving authority explains absence | Reopen committed merged result read-only; stop cleanup/completion as `TaintedPreserved` | None |
| All worker leases already removed | Self-contained commit and merged corpus validate | Reopen directly; do not require handoffs | None |
| Consumer dies before successor preparation | Generation-local start exists, no protected successor | Recover/clean exact incomplete successor; publish next linked start if budget remains | Exact successor generation only |
| Consumption-attempt budget exhausted | Valid contiguous start chain, no prepared successor | Keep merged corpus in `OutputHeld`; publish no ACK | None |
| Successor prepared, no ACK | Protected successor and start validate | Publish ACK without rebuilding | ACK record only |
| ACK exists, successor invalid | Descriptor or digest mismatch | Stop as `TaintedPreserved` | None |
| Merged cleanup dies mid-prefix | ACK binds exact merged handoff and valid successor | Idempotently continue exact artifact GC | Bound merged input only |
| Cleanup complete, no completed record | ACK, successor, and artifact absence validate | Publish `WaveCompletedV1` | Completed record only |
| `fsync`/rename outcome uncertain | System call error after visible namespace change | Reopen, revalidate, and re-establish barrier | Only after exact identity proof |

No rescue falls back to raw path deletion.

## Failure Modes Registry

| ID | Severity | Failure mode | Closure in this plan |
|---|---|---|---|
| F1 | P1 | Merge receipt survives but relation data does not | Durable merged corpus is a prerequisite of commit. |
| F2 | P1 | New master reruns a live old child | Inherited permanent wave lock makes the whole wave `Busy`. |
| F3 | P1 | Concurrent resumers both merge or clean | Exclusive wave lock serializes all reconciliation. |
| F4 | P1 | Restart resets retry count | The actual lease is reserved first; immutable attempt starts then consume a bounded ordinal before fork. |
| F5 | P1 | Cleanup recovery deletes an adoptable worker before classification | `HandoffPresent` is classified atomically under the existing private `BaseLock` before reserve/recovery may roll back. |
| F6 | P1 | Handoff is mistaken for deletion authority | Handoff only protects/adopts; validated commit plus exact handoff mints worker cleanup authority. |
| F7 | P1 | Path-bound mmap follows a replacement | Adoption uses frozen dirfd, no-follow open, single-link check, native identity, and same handles for reading. |
| F8 | P1 | Work digest omits an output-changing environment value | Full sieve/cofactor inventory is classified, parsed once, hashed conservatively, passed explicitly, and guarded by a source checker. |
| F9 | P1 | Different executable adopts old output | Manifest binds exact executable and execution-contract SHA-256. |
| F10 | P1 | Crash after worker cleanup but before result delivery loses data | Merged corpus remains durable until exact consumer ACK. |
| F11 | P1 | Lock path replacement permits two masters on different inodes | Manifest and completed record bind the permanent lock identity; open/path double snapshots precede mutation. |
| F12 | P1 | Finalized merged output is stranded or deleted before commit | Merged output uses deferred handoff; prepared state completes commit, incomplete state alone may roll back. |
| F13 | P1 | ACK window strands or duplicates a successor | Consumption start and successor-prepared records make the window recoverable. |
| F14 | P1 | Worker cleanup removes evidence required to reopen commit | Commit copies the complete per-chunk successful completion summary and validates without worker handoffs. |
| F15 | P1 | Completed artifact cleanup destroys its recovery authority | V1 retains permanent lock and all immutable metadata, and publishes `WaveCompletedV1` after artifact absence. |
| F16 | P1 | Caller arms merged cleanup before ACK | Result exposes a descriptor-bound read-only view with no cleanup receipt or arm route. |
| F17 | P1 | Legitimate missing lease is indistinguishable from tampering | External authorization and completion records survive each intra-lease deletion. |
| F18 | P1 | Retry consumes different random curves and changes output | All randomness is derived from stable semantic identities and excludes attempt/process state. |
| F19 | P1 | Failed merge/consumption generation blocks or aliases the next | Bounded wave-root start chains bind ordinal, predecessor, and exact private generation; old starts remain immutable. |
| F20 | P1 | Retry exhaustion is merged as a partial success | After failed-attempt preactive cleanup, `ChunkTerminalFailureV1` terminates the wave and forbids merge, ACK, and cleanup of successful artifacts. |
| F21 | P2 | Zero-relation success is rerun forever | Completion reason and zero-row handoff are explicit terminal states. |
| F22 | P2 | Unknown leaf is deleted as stale | Unknown content produces `TaintedPreserved`. |
| F23 | P2 | Chunk merge order changes after resume | Manifest order and first-`ABPair` policy are versioned and receipt-bound. |
| F24 | P2 | Retry-generation ABA adopts an old store | Handoff lives in and binds the exact private lease generation. |
| F25 | P2 | Cleanup accumulates forever without downstream checkpoint | Observability reports `OutputHeld`; pipeline enablement waits for a reopenable successor. |
| F26 | P2 | Mode bits or ACLs broaden mutation authority | Exact `07777`, owner, parent-write, and ACL policy checks fail closed. |
| F27 | P2 | Resume tests compare unstable PIDs/wait facts | Semantic, durable-progress, and current-process diagnostic oracles are separate. |

All P1 gaps are design-closed before implementation begins. Tests remain the
evidence gate.

## Architecture

```text
DistributedSieveConfig
  |
  +--> freeze DistributedSieveExecutionPolicy
  |       |
  |       +--> executable SHA-256
  |       +--> work SHA-256
  |
  +--> DistributedSieveWaveStore
  |       |
  |       +--> permanent WaveLock
  |       +--> immutable WaveManifestV1
  |       +--> immutable AttemptStartedV1 chain
  |       +--> immutable ChunkTerminalFailureV1
  |       +--> private-lease WorkerHandoffV1
  |       +--> protected merged OOC + MergePreparedV1
  |       +--> WaveMergeCommitV1
  |       +--> ConsumptionStartedV1 + SuccessorPreparedV1
  |       +--> WaveConsumptionAckV1
  |       +--> WaveCompletedV1
  |
  +--> existing worker orchestrator
  |       |
  |       +--> fork / child collector / waitpid
  |       +--> missing-attempt execution only
  |
  +--> deterministic streaming merge
          |
          +--> DistributedSieveWaveResult
                  |
                  +--> non-armable ReadOnlyRelationCorpusView
                  +--> worker dispositions
                  +--> explicit durable-consumer transaction
```

Dependency direction is one way:

```text
util durable record + SHA
          ^
          |
relation private handoff + OOC corpus
          ^
          |
sieve protocol codecs + wave store
          ^
          |
distributed worker orchestration
          ^
          |
optional pipeline durable successor
```

The relation layer does not parse sieve records. It only manages an opaque
immutable auxiliary payload and exact lease capability.

## Code Quality Decisions

1. Pure value types, validation, codecs, and digests live in
   `distributed_sieve_protocol.hpp/.cpp`; no filesystem calls are allowed there.
2. Filesystem state, locks, publication, and reconciliation live in
   `distributed_sieve_resume.cpp`.
3. Fork/wait and worker execution remain in `distributed_sieve.cpp`.
4. The cleanup transaction gains one narrow generic handoff state and
   cross-process adoption/consumption seam under its existing `BaseLock`. It
   does not learn chunk IDs, wave fields, or merge policy.
5. Record enums are closed and codecs reject unknown values, trailing bytes,
   duplicate chunks, gaps, overlaps, and noncanonical order.
6. Every persisted integer width is explicit. `size_t`, enums, native structs,
   and object representations never appear on wire.
7. State classification returns typed outcomes; exceptions carry context but
   string matching never drives recovery.
8. Cleanup is expressed as consuming a capability, not accepting a path.

## Proposed File Boundaries

| File | Change |
|---|---|
| `include/gnfs/sieve/distributed_sieve_protocol.hpp` | Public value types, enums, validation results, and stable protocol constants |
| `src/sieve/distributed_sieve_protocol.cpp` | Canonical codecs and SHA-256 preimages |
| `include/gnfs/sieve/distributed_sieve_resume.hpp` | Private wave-store and reconciliation API |
| `src/sieve/distributed_sieve_resume.cpp` | Stable root, wave lock, immutable publication, state machine, merge/ACK recovery |
| `include/gnfs/sieve/distributed_sieve.hpp` | Opt-in config, result dispositions, and move-only `DistributedSieveWaveResult` |
| `src/sieve/distributed_sieve.cpp` | Orchestrate durable plan; stop successful child cleanup-intent publication |
| `include/gnfs/relation/ooc_durable_handoff.hpp` | Opaque no-delete private state, locked classifier, cross-process adoption, and authorized exact consumption |
| `include/gnfs/relation/ooc_authorized_cleanup_intent.hpp`, `src/relation/ooc_authorized_cleanup_intent.cpp` | Pure V2 intent/staged marker codec and complete durable binding |
| `include/gnfs/relation/ooc_cleanup_transaction.hpp` | Integrate `HandoffPresent` before preactive recovery/reservation; durable handoff-to-cleanup-intent transition |
| `include/gnfs/relation/relation_corpus.hpp` | Adopt protected OOC corpora with preserve-only ownership and provide a non-armable read-only view |
| `include/gnfs/relation/ooc_relation_store.hpp` | `OOCRelationReader` constructor from owned exact index/data handles; expose frozen native identities |
| `include/gnfs/util/mmap_file.hpp` | Owned-native-file constructor so validation and mapping use the same handle |
| `include/gnfs/util/durable_immutable_record.hpp` | Relative-dirfd pending-to-canonical record publication |
| `scripts/check_distributed_sieve_policy.py` | Closed inventory for reachable `GNFS_*` reads and durable-path environment ban |
| `src/api/pipeline.cpp` | Later opt-in consumption through a reopenable structured successor |
| `tests/test_distributed_sieve_resume.cpp` | Protocol, crash, live-child, concurrent-resumer, merge, and ACK suites |
| `tests/test_ooc_cleanup_transaction.cpp` | Generic lease handoff/adoption crash and authority tests |
| `CMakeLists.txt`, `scripts/test.sh` | Test catalog, labels, timeouts, tiers, modules, and path mapping |
| `docs/testing-ci-policy.md` | Replace the current explicit “whole wave invalidates” limitation only after tests pass |
| `docs/env-flags/sieve.md` and index | Durable mode, stable root, defaults, failure behavior, and compatibility contract |

The final names may be adjusted to existing naming conventions, but the
dependency boundaries are fixed.

The new owned-handle APIs remain cross-platform at the type/build boundary.
POSIX durable mode requires dirfd-relative no-follow opens, inherited
open-description locks, durable directory sync, and ACL inspection. Windows
keeps existing path readers and receives compiling owned-handle/reparse-safe
validation where available, but the fork-based durable wave route returns
`PlatformUnavailable`; it never simulates POSIX ownership or weakens checks.

## Test Diagram

```text
canonical inputs
  |
  +--> work digest field drift --------------------------> core unit tests
  |
  +--> manifest codec/state validation -----------------> core unit tests
  |
  +--> attempt predecessor/retry budget ----------------> core unit tests
  |
  +--> lease handoff publication/adoption
  |       |
  |       +--> pending/canonical/link/replace ----------> relation crash tests
  |       +--> old creator gone, cross-process adopt ---> relation self-exec
  |       +--> commit/ACK authorization consumption ----> authority tests
  |
  +--> fresh real worker wave --------------------------> integration
  |       |
  |       +--> all handoffs adopted after master death -> self-exec integration
  |       +--> mixed adopted/missing/empty/zero --------> self-exec integration
  |       +--> live child holds inherited lock ---------> self-exec integration
  |       +--> concurrent resumers ---------------------> self-exec integration
  |
  +--> streaming deterministic merge
  |       |
  |       +--> prepared-before-commit recovery ---------> self-exec integration
  |       +--> commit crash points ---------------------> self-exec integration
  |       +--> replay from committed merged corpus -----> integration
  |
  +--> worker cleanup tail -----------------------------> crash-prefix matrix
  |
  +--> consumption start/prepared/ACK ------------------> crash-prefix matrix
  |
  +--> completed record and retained metadata ----------> crash-prefix matrix
```

## Functional Test Matrix

| Scenario | Required oracle |
|---|---|
| Fresh wave | Manifest is durable before the first attempt; result, order, counts, and digest equal an uninterrupted run under the same frozen durable policy. |
| Reservation crash before attempt | Exact preactive lease is removed; attempt budget is unchanged. |
| Attempt crash before fork | Durable ordinal is consumed; only the next allowed attempt may run. |
| Retry after worker failure | Stable per-SQ/candidate seeds reproduce the full serialized relation sequence; attempt ordinal changes no random choice. |
| Existing manifest, no handoffs | Only journal-permitted chunks run. |
| All handoffs valid | New worker launch count is zero; every nonempty chunk is `Adopted`. |
| Stale creator receipt after canonical handoff | Cleanup is rejected; canonical handoff remains readable and adoptable. |
| Partial handoffs | Only missing chunks run; mixed output exactly matches the uninterrupted durable-policy baseline. |
| More workers than SQs | Zero-length chunks are `Empty`, never fork, and remain in manifest order. |
| Nonempty chunk, zero relations | Valid zero-row handoff is adopted and never rerun. |
| Empty effective range | Empty result, cleared stats, no root mutation. |
| Null stats pointer | Identical durable behavior. |
| Terminal worker failure | Retry budget is not reset; exact cleanup of the failed attempt precedes a deterministic terminal record, then the API returns a wave error with no merge, ACK, or cleanup of successful handoffs. |
| Failed merge generation | Exact incomplete lease is cleaned; the next bounded start links to its predecessor and old start records remain valid. |
| Merge budget exhausted | After exact cleanup of the last incomplete merge generation, return a terminal merge error; every worker handoff remains and no commit or worker cleanup exists. |
| Prepared merge, no commit | No worker runs and no merge repeats; recovery publishes commit from the protected merged corpus. |
| Existing merge commit | Merged corpus reopens; no worker runs and no merge repeats. |
| Commit after every worker lease is removed | Commit plus merged corpus, without worker leases/handoffs, reopens the exact relation result and complete worker summary; cleanup records separately explain normal deletion. |
| Prepared successor, no ACK | Exact successor reopens, ACK finishes, and no consumer work repeats. |
| Failed successor generation | Exact incomplete successor is cleaned; the next bounded start links to its predecessor without changing merged input. |
| Consumption budget exhausted | Merged result stays `OutputHeld`; no ACK or merged cleanup exists. |
| Consumer ACK | Exact successor reopens and merged cleanup resumes idempotently. |
| Completed wave | Permanent lock and immutable metadata explain completion; no worker or merged-input OOC/private-lease artifact remains, the exact successor stays preserved/reopenable, and no further mutation occurs. |
| Manifest mismatch | `ConfigMismatch`, zero workers, no mutation. |
| Orphan handoff without manifest | `TaintedPreserved`, zero workers, no deletion. |
| Old child holds replaced lock inode | New lock acquisition still taints on manifest identity; zero mutation. |
| POSIX unavailable | Explicit unsupported result; no simulated success. |

Parity is split into three independent oracles:

1. relation semantics: exact ordered payload, count, sequence receipt, and
   corpus SHA-256;
2. durable progress: chunk range, next SQ, processed count, completion reason,
   raw/retained counts, attempt chain, and disposition derived from records; and
3. current-process diagnostics: PID, wait/reap source, signal, and native error,
   which may differ after resume and are compared only for local validity.

## Corruption and Drift Matrix

Every object is tested for bad magic, unsupported version, truncation, trailing
bytes, checksum mismatch, self-digest mismatch, symlink, hardlink, native
identity replacement, wrong owner/mode, and unexpected leaf.

Additional object-specific drift:

- work digest: mutate every included field one at a time; verify excluded path,
  PID, and timing fields do not change it;
- randomness: vary PID, attempt ordinal, scheduling, cache warmth, and wall
  clock; verify adaptive-lattice, ECM, and rho choices remain byte-identical;
- manifest: duplicate, missing, overlapping, unordered, and out-of-range chunks;
- attempt: broken predecessor, duplicate ordinal, skipped ordinal, wrong lease
  generation, and exhausted budget;
- merge/consumption starts: broken predecessor, duplicate or skipped ordinal,
  wrong private generation, conflicting prepared generation, and exhausted
  budget;
- terminal failure: live/unreconciled lease, surviving handoff, incomplete
  cleanup, unconfirmed predecessor, wrong exit normalization, or nonzero rows;
- handoff: wrong wave, chunk, range, lease, descriptor, extent, sequence,
  completion reason, corpus digest, stale writer receipt, stale private-lease
  receipt, or `RESERVED`-consumption prefix;
- merge preparation/commit: missing or duplicate completed chunk,
  terminal-failure reference, wrong order, old handoff, wrong dedup count,
  incomplete per-chunk summary, wrong output descriptor, wrong private lease,
  or wrong corpus digest;
- consumption/successor/ACK: wrong start, commit, consumer kind, lease,
  prepared digest, missing successor, or successor drift; and
- completed: wrong permanent lock, missing retained authority record, surviving
  artifact, wrong successor, or inconsistent terminal summary.

Namespace tests cover owner, exact `0700`/`0600` modes, every special bit,
symlink/hardlink substitution, group/world-writable parent, effective extended
ACL, unsupported ACL adapter, root replacement, lock replacement, and
double-snapshot drift.

No “latest file wins” rule is permitted.

## Crash-Point Matrix

A closed `DistributedSieveResumeCrashPoint` enum is paired with a test table.
Static or runtime checks fail when an enum value lacks a test case.

| Stage | Injected crash or failure boundaries |
|---|---|
| Root and lock | directory create, parent sync, lock create/open, lock acquisition, post-lock identity check, old child holding replaced lock inode |
| Manifest | create, partial write, complete write, file sync, pending parent sync, rename, canonical parent sync, reopen |
| Attempt | lease reservation before record, every manifest-equivalent publication point, after start and before fork |
| Worker OOC | lease reservation, first and second leaf creation, headers, ownership publication, finalization |
| Handoff | every publication point, pending rollback, canonical protection before ordinary recovery/reservation, stale-receipt rejection, `RESERVED` consumption, canonical while child paused, canonical before child exit |
| Adoption | after each record check, each native open, each identity snapshot, sequence replay, corpus digest, adopted-set insertion |
| Missing execution | before and after fork, child live, child exit without handoff, old child completes after master death |
| Terminal failure | after quiescence proof, predecessor confirmation, cleanup, normalized summary, and each publication point |
| Merge | merged lease reservation, every start-chain publication/predecessor point, after each chunk read, after dedup, each merged OOC publication point, finalization, digest, each prepared-handoff point |
| Commit | every publication point and canonical reopen |
| Worker cleanup | external authorization publication, typed conversion, intent pending/canonical, handoff consumption, pair staging/removal, lease-marker removal, directory removal, external completion, and every exact chunk prefix |
| Consumption | successor reservation, every start-chain publication/predecessor point, successor writes/finalization, each prepared-handoff point |
| ACK | every ACK publication point, canonical reopen, successor revalidation |
| Final GC/completion | merged external authorization, every intra-lease prefix, external completion, artifact-absence confirmation, every completed-record point |
| Return | after every durable state but before returning to the caller |

Each create, write, sync, rename, open, mmap/read, and unlink operation also gets
an injected error result. A crash after a durable point does not substitute for
testing a system-call failure at that point.

## Harness Plan

Add one binary, `test_distributed_sieve_resume`, with selectable suites:

| Suite | Content | Initial timeout | Tier |
|---|---|---:|---|
| `core` | Codecs, validation, work drift, state transitions, retry chain | 20 seconds | `instant` only if measured deterministic runtime remains below policy |
| `crash` | Synthetic OOC and every publication/cleanup crash point | 90 seconds | `fast` |
| `integration` | Fresh, adopted, mixed, live child, concurrent resumer, merge, ACK | 180 seconds | `fast` |

The no-argument binary runs every suite for the project runner. It is not added
to smoke. If Debug runtime exceeds the fast-tier policy, split real-process
integration into a second binary or map it to gate; do not hide cost by
unbounded timeout growth.

Update all required Harness surfaces:

- `ALL_TEST_BINARIES`;
- `MODULE_TESTS[sieve]`;
- `TEST_TIMEOUT`;
- `TEST_TIER`;
- slow-path mapping;
- `path_to_module()` for every new source and test; and
- CMake test `LABELS` and `TIMEOUT`; and
- the environment-inventory checker in Harness/CI.

## Performance Review

Resume must avoid materializing every worker corpus twice. The merge streams
each adopted corpus in manifest order into one OOC sink and keeps only the
first-`ABPair` index required by existing semantics. The initial implementation
may retain the current hash-set memory bound, but it must not also create a
full relation vector.

SHA-256 is computed while canonical bytes or relation rows are already being
written/read. Recovery reuses stored record digests and validates the corpus
once per adoption. There is no directory-wide polling loop; one nonblocking
wave lock and exact manifest-derived paths bound work.

Measure:

- fresh-wave overhead versus current distributed execution;
- all-adopted recovery time;
- merge throughput and peak RSS;
- metadata sync count;
- OOC bytes before and after each cleanup phase; and
- mixed recovery with one missing chunk.

Performance never weakens synchronization or identity checks. Optimization
starts only after the correctness suite passes.

## Observability

`DistributedSieveWorkerResult` gains:

- `disposition`: `Executed`, `Adopted`, `Empty`, `TerminalFailed`, or
  `RecoveredMerge`;
- wave ID;
- attempt ordinal and total durable starts;
- lease generation;
- completion reason;
- handoff digest;
- SQ begin/end, next index, processed count;
- raw and merged relation counts; and
- cleanup state.

Semantic/progress fields are record-backed. PID, direct-parent wait status,
reap source, signal, and transient native errors live in a separate diagnostic
subrecord that is neither hashed into work identity nor required to match a
different resumer.

Wave logs include stable short prefixes of manifest, work, handoff, merge, and
ACK digests. They never include absolute private paths in durable records.

One inspection function reports the classified state without mutation. Tests
and operators can prove that adoption occurred without relying on timing or PID.

## Deployment and Rollback

1. Land codecs and core tests with no runtime route.
2. Land generic handoff/adoption/authorized-cleanup primitives with legacy
   behavior unchanged and no durable sieve runtime route.
3. Land wave ownership and worker handoff as test/internal code only. Do not
   replace the production success path while a durable merge is absent.
4. Enable the opt-in direct API only after merge-start/prepared recovery,
   self-contained commit, read-only result, and commit-to-cleanup conversion
   pass their crash suites.
5. Keep the pipeline route on one-shot behavior while running crash and parity
   tests.
6. Enable pipeline consumption only after start/prepared/ACK recovery,
   ACK-to-cleanup conversion, and `WaveCompletedV1` pass their crash suites.
7. Consider changing defaults only after small-gate, 100–150-bit, and bounded
   50-digit evidence.

Rollback disables durable mode. Existing one-shot worker paths and record
formats remain readable. A rollback never deletes a durable wave root created
by a newer executable; it preserves it as unsupported/stale.

## Long-Term Trajectory

This milestone leaves a reusable local wave transaction:

- immutable plan;
- bounded attempt chain;
- exact worker handoff;
- deterministic durable merge;
- explicit consumer succession; and
- capability-based cleanup.

The 12-month ideal adds authenticated fork/exec workers, remote scheduling,
cross-platform launch, multi-wave campaigns, and an operator GC tool. Those
features can reuse this protocol only after local wave measurements show that
distributed resume saves meaningful 50-digit work.

Dream-state delta after this plan:

- local master-crash recovery: closed;
- deterministic durable relation output: closed;
- downstream succession: contract closed, pipeline route conditional;
- cross-build and remote trust: deliberately open;
- Windows worker execution: open; and
- general distributed scheduler: open.

Reversibility score: 5/5. The route is opt-in, versioned, additive, and leaves
the existing one-shot implementation available.

## Implementation Milestones

### M0: Freeze the Protocol

- [x] Finalize this plan and independent reviews.
- [x] Add every canonical record type: manifest, attempt, terminal failure,
  handoff payload, merge start/prepared/commit, cleanup
  authorization/completion, consumption start, successor prepared, ACK, and
  completed.
- [x] Implement full execution policy, deterministic-randomness contract,
  environment inventory checker, work identity, and field-drift tests.
- [x] Add the core test suite and Harness entries.
- [x] Verify with the dedicated target, `module sieve`, and `changed`.

Exit criterion: no filesystem mutation exists yet; every record and digest has
closed validation and drift coverage.

Completed on 2026-07-26. The dedicated core suite, all 19 sieve tests, the
21-test changed selection including GNFS E2E, the policy inventory, and Harness
checks passed in Debug.

### M1: Generic Durable Handoff

- [x] Add the relative-dirfd immutable record publisher.
- [x] Add an opaque no-delete auxiliary record inside a private lease.
- [x] Make canonical handoff dominate recovery, revoke/consume `RESERVED`, and
  invalidate stale writer/private-lease receipts.
- [x] Add locked cross-process adoption with no cleanup authority.
- [x] Bind each adoption capability to its adopter process and retain the
  optional duplicate-pending observation for private conversion.
- [x] Freeze the role-separated V2 authorized-cleanup marker codec and exact
  canonical/optional-pending handoff bindings without enabling runtime use.
- [ ] Add the two-capability application-authorization conversion into
  canonical cleanup intent.
- [x] Add owned-handle `MmapFile`, `OOCRelationReader`, and non-armable corpus
  view APIs; preserve Windows compilation and fail unsupported durability
  semantics closed.
- [x] Cover pending/canonical, rollback, link, replacement, and lock crash
  points in relation tests.

Publisher foundation completed on 2026-07-26. The production path is
handle-relative and no-follow, uses native no-replace promotion, preserves
foreign entries, and verifies exact owner, mode, link count, bytes, native
identity, and durability across every recovery boundary. Its core/crash suites,
the complete utility module, dependency-deep changed selection, GNFS E2E,
Harness checks, and the independent security review passed in Debug.

The M1 handoff protocol and relation-layer phase transition completed on
2026-07-26. `OOCPrivateHandoffV1` binds the complete lease generation, native
identities, finalized V3 pair and extents, payload kind/version, and a bounded
opaque payload through canonical little-endian bytes and independent payload
and record digests. The production classifier uses the bounded relative reader
under the held private-directory handle. Canonical publication now precedes
durable `RESERVED` revocation, blocks stale writer and lease receipts, and
preserves corrupt, foreign, or ambiguous state without mutation. Existing
cleanup intent remains a separate authority.

The focused lease-crash target, the relation module, dependency-deep changed
selection, API contract regression, GNFS E2E, Harness checks, and the
independent security review passed. The security review reported no P0, P1, or
P2 findings after the final receipt-gating regressions. Linux and Windows
generic handoff operations still fail closed before filesystem action.

The owned-handle read-only slice completed on 2026-07-26. `OwnedNativeFile`
provides move-only native-handle ownership, and `MmapFile` maps the same
validated handle without reopening a path. The V3-only `OOCRelationReader`
constructor commits both exact handles together, validates paired identity and
extents through those mappings, and closes both handles after any post-commit
failure. `ReadOnlyRelationCorpusView` exposes only `count()`, `read()`, and
`for_each()`. It captures an opaque reader binding, so default, moved-from, or
rebound readers fail closed instead of appearing as an empty or replacement
corpus. Linear algebra reuses that exact type rather than maintaining a second
adapter with weaker lifetime rules.

The focused reader, corpus, streaming SGE, relation-sink, and large-prime
contract tests passed. The complete relation module and dependency-deep changed
selection also passed, including the API contract regression and GNFS E2E. The
final independent security review reported no P0, P1, or P2 findings.

The BaseLock stability gate completed on 2026-07-26. POSIX now revalidates that
the held flock inode is still the regular single-link object at the frozen lock
leaf before and after every authority-bearing phase. The same gate surrounds
generic intent publication and cleanup, private-lease publication and
recovery, reservation and activation, and fresh writer construction. Noexcept
publication callbacks use a non-throwing probe and stop before the next
namespace mutation. Deterministic regressions replace the lock at handoff
promotion, intent-pending durability, final lease rename, and RESERVED
revocation; they prove that no stale receipt, lease, or writer authority is
returned and that exact protocol artifacts remain preserved. The common
lock-replacement cases run on both supported POSIX families. A separate
in-process interruption at durable RESERVED removal proves that activation has
already revoked preactive rollback authority, so constructor failure preserves
the committed pair exactly as process-crash recovery does. A post-sync foreign
RESERVED replacement also forces verification failure before activation
returns; the same regression proves that the durability barrier, rather than a
later successful absence check, is the capability transition.

The exact owned-file opener foundation completed on 2026-07-26. The production
macOS path validates a pure relative request, opens one no-follow/nonblocking
leaf below a caller-held directory, and proves exact native identity, extent,
owner, mode, single-link policy, ACL absence, stable metadata, and parent policy
before returning the same move-only handle. Missing leaves are rechecked under
the held parent. Rejected, interrupted, and failed paths close the internal
descriptor without exposing a partial payload; the successful payload is
factory-only and single-consumption. Linux and Windows return unsupported after
pure request validation and before filesystem observation. This utility grants
read ownership for one leaf only: it does not bind the directory's named path,
commit an index/data pair, classify a handoff, or mint adoption authority.

Locked cross-process adoption completed on 2026-07-26. The macOS classifier
holds one parent directory, one private directory, and one persistent
`BaseLock` throughout the operation. It reads every control marker and handoff
relative to those handles, scans through a separately opened directory
description, and opens both OOC files by exact native identity and extent.
Initial and final witnesses bind raw record bytes, snapshots, marker chains,
the allowlist, both directory identities, and the lock leaf. A move-only
receipt retains all handles but exposes neither a path nor cleanup authority.
`OOCPrivateHandoffReader` then consumes both files into the existing
same-handle V3 validator while keeping the directory and lock authority alive.

The lease-crash suite now covers self-exec publisher and adopter death,
zero-row corpora, pending-only and canonical-with-`RESERVED` prefixes, every
adoption interruption boundary, byte-identical inode replacement, pair
replacement before and after exact open, owner/owned replacement, unknown and
legacy leaves, directory replacement, lock replacement, descriptor balance,
and retry after non-mutating interruption. Non-macOS platforms return
unsupported after pure request validation and before filesystem observation.

The first M1.7b capability-hardening slice completed on 2026-07-26. The
move-only adoption receipt now captures the adopter PID and the exact optional
duplicate-pending observation from the final locked witness. A forked
copy reports spent before it can construct a reader, and a reader inherited
after construction rejects new access through its process-checked owner. The
PID is captured before any adoption hook, so a fork during adoption cannot mint
a second valid capability. A const reader reference borrowed before fork
remains ordinary read-only data and grants no conversion or cleanup authority.
The parent receipt or reader remains valid because the child changes only its
copy-on-write state. The pending observation stays private and grants no
unlink, intent, or cleanup authority.

The M1.7a pure authorized-cleanup marker codec completed on 2026-07-26. Its
480-byte frame has independent magic, schema, phase kind, and digest domain.
It captures both the canonical generic-handoff record snapshot and the optional
duplicate pending observation made under lock. The field is pure evidence, not
permission to unlink a post-crash name. Canonical runtime markers will reject a
present value after the precommit normalization described above. Canonical and
pending snapshots bind native identity and exact encoded extent within the
nonadversarial same-user threat boundary stated above; they do not claim to
defeat deliberate inode cycling by a hostile same-user process. The base digest
reuses the private-lease `frozen_path_digest()` rule over raw `path.native()`
code units. Pure tests freeze the complete layout and platform-specific
self-digests, mutate every authority field, cover zero rows, bounds, overflow,
aliases, framing, required expected-kind decoding, and phase separation, and
prove that V1 and V2 codecs cannot reinterpret each other. Legacy cleanup
recovery also rejects raw V2 intent or staged bytes while preserving the pair
and every marker.

The M1.7b two-capability cleanup conversion remains required before the M1
exit criterion is complete. It must use a WaveStore-only mint key, creator-PID
checks, the fixed lock order `wave lock -> target BaseLock`, and a private
relation bridge. It must also enforce the closed canonical-marker validator and
the precommit pending normalization above. No public record, digest, path,
reader, or adoption receipt alone may reach deletion.

Exit criterion: a finalized synthetic private OOC corpus can survive owner
death, reject every stale cleanup receipt, be adopted without deletion
authority, and later be cleaned only after an external durable authorization
is converted into intent. Legacy cleanup tests remain unchanged.

### M2: Wave Ownership and Attempts

- [ ] Add stable wave root validation and permanent inherited lock.
- [ ] Publish/recover `WaveManifestV1`.
- [ ] Reserve each actual lease before publishing/reconciling the bounded
  attempt predecessor chain.
- [ ] Freeze and explicitly thread the parsed execution/randomness policy
  before hashing or forking; durable callees perform zero `getenv` reads.
- [ ] Add lock/root replacement, live-child, descriptor-hygiene, and
  concurrent-resumer tests.

Exit criterion: two masters cannot both act, and restart cannot exceed the
manifest retry budget. This milestone remains test/internal and exposes no
production durable flag.

### M3: Worker Handoff and Adoption

- [ ] Replace successful worker cleanup-intent publication with handoff.
- [ ] Make the handoff phase transition consume old cleanup authority.
- [ ] Reconcile handoffs under the same private lock before any recovery or new
  lease reservation.
- [ ] Read adopted OOC pairs through frozen native handles.
- [ ] Run only exact missing chunks.
- [ ] Emit dispositions, deterministic relation receipts, and preserve every
  worker artifact.

Exit criterion: fresh, all-adopted, and mixed waves match the uninterrupted
durable-policy baseline exactly with worker-launch ledger evidence. The route
remains internal and cannot clean workers until M4 lands.

### M4: Durable Merge Commit

- [ ] Reserve a deferred merged lease, publish/recover the bounded
  predecessor-linked `MergeStartedV1` chain, and stream manifest-order inputs
  into its exact generation.
- [ ] Bind sequence, corpus, descriptor, native identity, and dedup receipts.
- [ ] Publish/recover protected `MergePreparedV1` and a self-contained
  `WaveMergeCommitV1`.
- [ ] Return only a non-armable descriptor/handle-bound result view.
- [ ] Publish per-worker cleanup authorization/completion and convert commit
  authority into exact intra-lease cleanup intent.
- [ ] Cover every merge, authority-conversion, intra-lease, completion, and
  worker-prefix crash.

Exit criterion: a restart after any worker-cleanup prefix reopens the same
merged corpus without worker execution or repeated merge, including when all
worker leases are absent. This is the first milestone allowed to expose the
opt-in direct API.

### M5: Consumer ACK and Pipeline Boundary

- [ ] Add the move-only, non-armable `DistributedSieveWaveResult`.
- [ ] Reserve a deferred successor lease and publish/recover the bounded
  predecessor-linked `ConsumptionStartedV1` chain,
  `SuccessorPreparedV1`, and `WaveConsumptionAckV1`.
- [ ] Bind ACK to a reopenable structured reduction successor.
- [ ] Resume prepared successors before ACK without repeating consumption.
- [ ] Publish merged cleanup authorization/completion, convert ACK authority to
  intent, and keep the successor preserved.
- [ ] Publish/recover `WaveCompletedV1`; retain all immutable metadata and
  garbage-collect artifacts only.
- [ ] Add every consumption, merged-cleanup, and completion crash test.

Exit criterion: pipeline resume can reconstruct its exact successor after an
ACK; no RAM-only path acknowledges consumption, and the completed root retains
its permanent classification anchor.

### M6: Cross-Size Evidence and Promotion Decision

- [ ] Run Debug/Release protocol and crash suites.
- [ ] Run `module sieve`, `changed --deep`, gate-relevant coverage, and e2e when
  the pipeline route lands.
- [ ] Compare the 81-bit gate path, a 100–150-bit path such as
  `test_kleinjung_large`, and a bounded 50-digit first wave.
- [ ] Record runtime, RSS, bytes, recovery time, and relation identity.
- [ ] Keep opt-in or promote based on measured cost and recovered work.

## Worktree Parallelization Strategy

The work is split by ownership boundary, not by arbitrary file count:

```text
Lane A: protocol + digest + core tests
Lane B: relation durable handoff + security/crash tests
Lane C: wave store + process integration tests
Lane D: main integrator, deterministic merge, pipeline boundary, Harness
```

- M0 permits Lane A and test-fixture planning in parallel.
- M1 permits Lane B while Lane A stabilizes public record fixtures.
- M2/M3 permit Lane C only after Lane A record layouts and Lane B adoption API
  are frozen.
- M4/M5 are integrated sequentially because cleanup authority and downstream
  succession share one ordered crash boundary.
- Agents use nonoverlapping files. The main integrator alone edits shared
  public headers, CMake, `scripts/test.sh`, and formal docs.
- Builds and broad suites are serialized through `scripts/test.sh` to avoid
  cache and resource interference.

## Implementation Tasks

Synthesized from CEO and engineering review findings.

- [x] **T1 (P1, human: ~2 days / agent: ~3h)** — Protocol — Freeze all canonical records, execution policy, deterministic randomness, and complete work identity.
  - Surfaced by: security/test review, environment and retry-randomness closure.
  - Files: protocol header/source and core tests.
  - Verify: `./scripts/test.sh run test_distributed_sieve_resume --suite core`.
- [ ] **T2 (P1, human: ~3 days / agent: ~5h)** — Relation storage — Add the rollback-revoking handoff phase, same-handle adoption/read-only view, and typed authorized-cleanup conversion.
  - Surfaced by: cleanup-before-adoption, stale receipt, path-following, and authority-bridge risks.
  - Files: OOC handoff, cleanup transaction, mmap/reader/corpus, relation crash tests.
  - Verify: relation lease-crash suite and `./scripts/test.sh changed --deep`.
- [ ] **T3 (P1, human: ~2 days / agent: ~3h)** — Wave ownership — Add stable root/lock identity, inherited descriptor hygiene, and lease-first bounded attempt chain.
  - Surfaced by: live-child, replaced-lock, dual-master, and retry-reset failure modes.
  - Files: resume store, distributed config, resume crash tests.
  - Verify: live-child and concurrent-resumer self-exec cases.
- [ ] **T4 (P1, human: ~2 days / agent: ~3h)** — Adoption — Add an internal-only worker handoff route and missing-only deterministic execution.
  - Surfaced by: master-crash adoption objective.
  - Files: distributed worker orchestration, resume store, integration tests.
  - Verify: fresh/all-adopted/mixed parity and launch ledger.
- [ ] **T5 (P1, human: ~3 days / agent: ~5h)** — Merge — Add merge start/prepared/commit, non-armable result, and external worker cleanup authorization/completion.
  - Surfaced by: merge receipt without data, provisional orphan, caller-arm, and cleanup-prefix ambiguity.
  - Files: resume store, `RelationCorpus` integration, crash tests.
  - Verify: every merge/cleanup crash prefix.
- [ ] **T6 (P1, human: ~3 days / agent: ~5h)** — Succession — Add consumption start/prepared/ACK, ACK-gated merged cleanup, and completed classification.
  - Surfaced by: ACK-over-RAM, pre-ACK successor, final-cleanup, and metadata-anchor risks.
  - Files: distributed result API, pipeline, structured reduction resume path.
  - Verify: ACK/successor drift and final-GC crash matrix.
- [ ] **T7 (P2, human: ~1 day / agent: ~1h)** — Harness/docs — Register suites and document opt-in operations.
  - Surfaced by: current policy explicitly denies completed-worker adoption.
  - Files: CMake, test runner, CI policy, sieve env docs.
  - Verify: `./scripts/test.sh list`, checker, diff/path/secret scan.
- [ ] **T8 (P2, human: ~2 days / agent: ~3h)** — Evidence — Run cross-size correctness and cost experiments.
  - Surfaced by: GNFS size-sensitive behavior.
  - Files: bounded experiment reports only.
  - Verify: small gate, 100–150-bit path, bounded 50-digit wave.

## CEO Review

### Section 1: Architecture Review

The first pass found three structural issues: merge metadata lacked durable
data, cross-master ownership was undefined, and cleanup authority was conflated
with worker completion. Independent and external adversarial passes then found
the missing rollback-revoking handoff phase, authority conversion, merge and
successor prepare states, external cleanup completion proof, terminal schema,
lock identity, deterministic randomness, read-only result, and final
classification anchor. The bounded transaction now models each explicitly
without creating a general scheduler.

### Section 2: Error and Rescue Map

Twenty-nine concrete failure classes are mapped in the registry. Rescue always
requires exact record and native identity proof. Foreign or ambiguous state has
no destructive rescue.

### Section 3: Security and Threat Model

The final registry contains twenty P1 boundaries. In addition to the initial
cleanup-before-adoption, merge-without-data, PID-liveness, retry, path-following,
and work-identity gaps, it closes stale receipts, replaced lock inodes,
provisional merge orphans, pre-ACK successors, ambiguous deletion absence,
caller-armed cleanup, random retry drift, generation-chain aliasing, partial
success after exhaustion, and lost completion anchors. The same-user
nonadversarial assumption is stated instead of overstating SHA-256.

### Section 4: Data Flow and Interaction Edge Cases

Twenty-four classified states cover fresh, resume, reserved-prestart, zero-row,
terminal failure, protected handoff, merge start/prepared/commit,
authorization/cleanup, consumption start/prepared/ACK, completion, busy,
mismatch, and taint. No user-facing UI exists, so there are no visual
interaction states.

### Section 5: Code Quality Review

The primary risk was putting sieve semantics into the already large cleanup
header. The plan confines relation changes to generic protected-handoff states,
same-handle readers, and a two-capability authority adapter. Sieve codecs,
filesystem reconciliation, and process orchestration remain separate; the
caller-facing result has no cleanup route.

### Section 6: Test Review

The current suite proves only same-master retry. A dedicated resume binary,
closed crash-point enum, full intra-lease prefix matrix, stale-receipt and
permission tests, native corruption matrix, deterministic-randomness checks,
and real live-child/concurrent-resumer tests fill the gap.

### Section 7: Performance Review

The corrected plan uses a streaming OOC merge rather than a second full vector.
All added hashes run on bytes already read or written. No synchronization check
may be removed for speed.

### Section 8: Observability and Debuggability Review

Existing worker stats cannot prove adoption. Dispositions, attempt ordinals,
lease generations, completion reasons, and stable digest prefixes make replay
and hidden reruns observable.

### Section 9: Deployment and Rollout Review

The path is additive and opt-in. Worker handoff remains internal until durable
merge exists; the direct API waits for M4; pipeline ACK waits for successor,
cleanup-authority, and completed-record recovery in M5. Rollback preserves
unsupported durable roots.

### Section 10: Long-Term Trajectory Review

The plan creates the smallest local platform that future remote execution could
reuse. Full campaigns, remote trust, Windows, and cross-build certification
remain separate evidence-driven work.

### Section 11: Design and UX Review

Skipped because there is no UI scope.

## Engineering Review

### Step 0: Scope Challenge

Actual code review showed that `run_distributed_sieve` returns an in-memory
vector, removes successful workers before merge, uses path-based readers, and
reads ambient/random process state inside workers. Therefore the original scope
was insufficient. Scope was expanded to protected durable merge, explicit
authority conversion, frozen deterministic execution, recoverable consumer
succession, and terminal classification; network scheduling and a general
journal were kept out.

### Section 1: Architecture

Six load-bearing boundaries are explicit: pure protocol/execution policy,
generic relation handoff and same-handle storage, durable record publisher,
wave store/authority validator, worker orchestration, and downstream
succession. The dependency graph is acyclic and the identity-bound wave lock
removes concurrent state mutation.

### Section 2: Code Quality

Six risks are controlled: duplicated durable publication, filesystem code
inside codecs, sieve knowledge in cleanup, untyped recovery strings, caller
cleanup capability, and hidden environment/random reads. The plan assigns one
implementation owner to each and requires typed outcomes.

### Section 3: Test Review

The test diagram maps every new flow to unit, injected, self-exec, or integration
coverage. The largest gaps in the current tree are master-death adoption,
cross-master liveness, durable merge replay, and cleanup-prefix recovery. All are
P1 tasks.

### Section 4: Performance

The OOC design bounds relation payload memory, exact path lookup avoids scanning,
and all-adopted recovery avoids worker execution. The remaining measured risk is
the fsync and SHA cost; cross-size evidence decides promotion.

## Scope Expansion Decisions

| Proposal | Effort | Decision | Rationale |
|---|---:|---|---|
| Inherited wave lock | M | Accepted | Required to prove quiescence and serialize resumers. |
| Durable merged OOC corpus | L | Accepted | Required to survive cleanup and return-window crashes. |
| Recoverable consumption start/prepared/ACK | L | Accepted | Required to protect the last durable relation copy and close the pre-ACK successor window. |
| Attempt predecessor chain | M | Accepted | Required for restart-equivalent retry bounds. |
| Adoption dispositions | S | Accepted | Required to prove no hidden rerun. |
| Exact executable identity | M | Accepted | Required to enforce the deferred cross-binary boundary. |
| Rollback-revoking handoff phase | M | Accepted | Required so canonical handoff dominates stale `RESERVED` and creator receipts. |
| External cleanup authorization/completion | L | Accepted | Required to bridge commit/ACK authority into cleanup and explain legitimate absence. |
| Merge start and prepared records | M | Accepted | Required to recover finalized output before commit. |
| Non-armable result facade | S | Accepted | Required to enforce ACK-only merged cleanup. |
| Deterministic execution randomness | L | Accepted | Required for retry-equivalent ordered relations. |
| Completed classification record | M | Accepted | Required to retain the final recovery anchor. |
| General multi-wave journal | XL | Deferred | Not required for a bounded local wave. |
| Remote/Windows workers | XL | Deferred | Separate launch and trust problem. |
| UI dashboard | M | Skipped | Structured inspection is sufficient. |

## Decision Audit Trail

| # | Phase | Decision | Principle | Rationale | Rejected |
|---:|---|---|---|---|---|
| 1 | Premise | Merge commit must bind durable data | Correctness first | Metadata cannot replay an in-memory vector after cleanup | Receipt-only completion |
| 2 | Scope | Select bounded durable wave transaction | Complete the local safety chain | Closes crash recovery without a scheduler rewrite | Handoff-only and general campaign |
| 3 | Ownership | Use one permanent inherited wave lock | Explicit over clever | It proves old-child quiescence across master death | PID probing and pidfd |
| 4 | Retry | Reserve actual lease, then durable start consumes an ordinal before fork | Reproducibility | Prestart crash costs no budget; every recorded start does | Intended generation and reset after crash |
| 5 | Handoff | Canonical private handoff revokes rollback and stale receipts | Eliminate ABA | Lease generation, artifact identity, and phase transition stay one transaction | Allowlist-only sidecar |
| 6 | Cleanup | Validated commit/ACK plus exact handoff creates external authorization, then intent | Least authority | Application semantics and generic deletion remain separate and crash-recoverable | Descriptor/path factory and direct success intent |
| 7 | Last copy | Consumer ACK requires reopenable durable successor | Fail closed | RAM ownership cannot survive process death | ACK after vector copy |
| 8 | Identity | Hash executable, contract, and every output field | Reproducibility | Rejects incompatible old waves | Best-effort field list |
| 9 | Corruption | Foreign or ambiguous state becomes `TaintedPreserved` | Preserve evidence | Unknown ownership cannot be safely converged by deletion | Automatic quarantine/delete |
| 10 | Rollout | Keep durable mode opt-in through crash and cross-size evidence | Reversible rollout | Existing one-shot route remains the rollback path | Immediate default promotion |
| 11 | Parallelism | Parallelize by ownership boundary and serialize shared integration | Minimize merge risk | Agents can progress without overlapping authority code | Concurrent shared-header edits |
| 12 | Design | Skip UI review | Scope discipline | This is a storage/process protocol | Dashboard work |
| 13 | Merge build | Use start plus protected prepared handoff | Preserve last copy | Finalized precommit output remains adoptable | Untracked provisional sink |
| 14 | Result API | Expose a non-armable read-only corpus view | Least authority | Callers cannot bypass ACK | Owned corpus with public arm route |
| 15 | Consumption | Prepare successor before ACK and resume it | Exactly-once succession | Pre-ACK crash neither leaks nor repeats work | Finalize then untracked ACK |
| 16 | Deletion proof | Retain external authorization and completion records | Auditability | Legitimate absence is distinguishable from tampering | Absence-only progress |
| 17 | Randomness | Derive seeds from stable work/chunk/SQ/candidate identity | Reproducibility | Attempt, PID, time, and scheduling cannot change output | `random_device` in durable workers |
| 18 | Completion | Retain lock and immutable metadata after `WaveCompletedV1` | Recoverability | Final classification survives every artifact-GC prefix | Metadata deletion in V1 |
| 19 | Repeated builds | Use bounded predecessor-linked merge/consumption start chains | Convergence | Old generations remain valid audit leaves while exact new generations can progress | Singleton start file and “latest wins” |
| 20 | Exhaustion | Treat worker retry exhaustion as terminal wave error | Correctness first | Every successful result contains all nonempty chunks and preserves payload parity | Zero-row partial-success merge |

## Completion Summaries

### CEO Review Completion

```text
Mode: SELECTIVE EXPANSION
System audit: data, authority conversion, retry, TOCTOU, identity, and randomness corrected
Architecture: initial 3 issues plus independent/adversarial authority gaps closed in plan
Errors: 29 paths mapped, 0 unresolved design gaps
Security: 20 P1 failure modes closed in protocol
Data flow: 24 states and all nil/empty/error branches mapped
Code quality: 6 boundary risks controlled
Tests: crash, corruption, permission, stale-receipt, and parity diagrams produced
Performance: streaming OOC design, measurement gate retained
Observability: 5 disposition classes and durable identity fields added
Deployment: opt-in staged rollout, reversible 5/5
Future: 4 items deliberately deferred
Design: skipped, no UI scope
NOT in scope: written
What already exists: written
Dream state delta: written
Error/rescue registry: 29 methods, 0 unresolved critical gaps
Failure modes: 27 total, 0 unresolved critical gaps
Scope proposals: 15 proposed, 12 accepted, 2 deferred, 1 skipped
Unresolved decisions: 0
```

### Engineering Review Completion

```text
Scope challenge: expanded to protected merge, authority bridge, deterministic policy, successor transaction, and completed record
Architecture: 6 component boundaries frozen
Code quality: 6 risks addressed
Test review: normative crash invariant and closed prefix matrices produced
Performance: fsync/SHA/RSS measurement plan written
NOT in scope: written
What already exists: written
Failure modes: 27 mapped, 0 unresolved design gaps
Parallelization: 4 lanes, 3 parallelizable and 2 ordered integration phases
Unresolved decisions: 0
```

## Unresolved Decisions

None. Every implementation-affecting choice is frozen above. Measured promotion
after M6 is a future evidence decision, not an implementation ambiguity.

## GSTACK REVIEW REPORT

- CEO review: clean; 15 scope/architecture findings incorporated.
- Engineering review: clean; 18 findings incorporated, 0 critical gaps.
- External adversarial review: 11 findings incorporated, 0 unresolved.
- Independent second-pass review: clean; no remaining P0, P1, or P2.
- Review mode: selective expansion with full engineering and outside-voice
  validation.

NO UNRESOLVED DECISIONS
