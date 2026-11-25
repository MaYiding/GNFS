# CLI Event Stream Protocol

The `--event-stream` option exposes a versioned JSON Lines interface for GUI,
automation, and process supervisors. Each line on standard output is one complete
JSON object. Consumers must read standard error concurrently, but must treat it
as diagnostic text rather than protocol data.

## Invocation

```bash
./build/gnfs 1000036000099 --complete --event-stream
```

`--complete` recursively splits composite remainders until every returned factor
passes GMP probable-prime testing. It also treats a prime input as a successful
one-factor result. This is a high-confidence computational classification, not a
primality certificate.

`--event-stream` cannot be combined with `--help`, `--version`, `--interactive`,
`--json`, `--csv`, `--report`, or `--output`. An incompatible invocation writes
one `error` event and exits with status 1. Options such as `--verbose`,
`--config`, `--method`, and `--threads` remain available.

## Framing and Compatibility

- The encoding is UTF-8 JSON Lines. Every event ends with `\n` and contains no
  embedded physical newline. Invalid bytes in diagnostic input are escaped.
- Every object contains `"schema_version": 1` and a `type` discriminator.
- Input integers and factors are decimal strings. Consumers must not coerce them
  to fixed-width or floating-point numbers.
- Non-finite measurements serialize as `null`; locale settings never change the
  JSON decimal separator.
- Consumers of schema version 1 must ignore unknown fields. Removing a field,
  changing its meaning, or changing event ordering requires a new schema version.
- Standard output belongs exclusively to this protocol while the option is
  active. Standard error is outside the protocol and may contain diagnostics.

The repository enforces standard-output ownership with
`scripts/check_stdout_contract.py`. Production library code may write diagnostic
text only to standard error; `src/cli/main.cpp` owns user-facing standard output.

## Event Sequence

A validated factorization invocation emits events in this order:

```text
started
progress or log (zero or more, in callback arrival order)
result or error (exactly one terminal event)
```

Argument, configuration, and input validation can fail before execution starts.
Those failures emit one `error` event without a preceding `started` event.

If standard output reports a write or flush error, the command exits
unsuccessfully and writes a diagnostic to standard error. A truncated stream
can end with an incomplete line or a missing terminal event.

The CLI serializes callback writes so concurrent progress and log callbacks never
interleave bytes from separate events. The sequence does not promise a fixed
number of progress events or a fixed ratio between progress and log events.

## Events

### `started`

```json
{"schema_version":1,"type":"started","n":"360","n_bits":9,"n_digits":3,"method":"trial","method_name":"Trial Division","method_reason":"3d/9bit: trial division sufficient","complete_factorization":true}
```

The event records normalized decimal input, its size, the selected method, and
whether recursive complete factorization was requested.

### `progress`

```json
{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.5,"elapsed_s":1.25,"message":"collecting relations","relations_found":500,"relations_target":1000,"special_q_done":20,"matrix_rows":0,"matrix_cols":0,"dependency_index":-1,"dependencies_total":0,"method":"gnfs","algorithm":"gnfs","current_number":"360","attempt_index":1,"effective_parameters":{"degree":3,"rational_bound":2000,"algebraic_bound":2000,"large_prime_bound":100000,"sieve_width":64,"sieve_height":40,"max_special_q":1000}}
```

`phase_progress` lies between zero and one when the phase has a bounded estimate.
It is `null` when progress is indeterminate. Counters that do not apply to the
current phase remain zero, and `dependency_index` is `-1` while no dependency is
being tried.

`method` is the method family that owns the step (`auto`, `trial`, `rho`, `siqs`,
or `gnfs`). `algorithm` names the step that runs, such as `primality`, `trial`,
`siqs`, or `gnfs`. `current_number` is the composite that the step operates on.
`attempt_index` is the one-based position of the current split inside a complete
factorization, or zero outside a numbered split. `effective_parameters` lists the
GNFS parameters that the pipeline applied, or is `null` until the GNFS pipeline
starts. The last progress event of a GNFS split has phase `done` and
`phase_progress` 1 when the split finds factors. It has phase `extract` and
`phase_progress` `null` when the dependency search finds none.

### `log`

```json
{"schema_version":1,"type":"log","level":"INFO","phase":"extract","timestamp_s":1.5,"message":"Prime factor confirmed: 5"}
```

Log events appear when the active path emits library log callbacks. Direct
diagnostics from lower-level components can still appear on standard error.

### `result`

```json
{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"360","factors":["2","2","2","3","3","5"]}}
```

The nested result contains the same fields as `FactorResult::to_json()`, including
method evidence, timings, sieve statistics, matrix statistics, and factors. The
short example omits those additional fields for readability.

- `timings.non_gnfs_s` measures the preliminary checks and the non-GNFS
  algorithms.
- `stats.executed_algorithm` names the algorithm that produced the split, or the
  last one attempted, and `stats.algorithms_executed` lists every algorithm that
  ran. A complete factorization over several algorithms reports `mixed`.
- `stats.gnfs_parameters_applied` is true once the GNFS pipeline starts, and
  `stats.sieve_width`, `stats.sieve_height`, and `stats.max_special_q` then hold
  the applied values.
- `stats.siqs_relations`, `stats.siqs_polynomials`, and `stats.siqs_workers` are
  `null` when SIQS returned no statistics.
- `stats.candidates_statistics_complete` is false when distributed workers do not
  report candidates.
- `attempts` holds one entry per composite split of a complete factorization and
  is empty for a single split.

`factorization_complete` means the returned list multiplies to the original
input and contains no remainder classified as composite. `factors_prime` means
every returned factor passed the GMP probable-prime test. Both flags are false
for the ordinary one-split API unless that API explicitly establishes the same
contract.

A `result` event is terminal even when `result.success` is false. The process
then exits with status 1.

### `error`

```json
{"schema_version":1,"type":"error","code":"invalid_number","message":"Invalid number: not-a-number"}
```

The `code` is stable for programmatic handling. The message is diagnostic text
and can vary. Current codes include validation failures such as
`unknown_option`, `multiple_numbers`, `invalid_option_value`, `invalid_threads`,
`incompatible_options`, `missing_number`, `config_error`, `invalid_number`, and
`number_too_small`, plus `runtime_error` for an exception during execution.

## Exit Status

| Status | Terminal event | Meaning |
|---:|---|---|
| `0` | `result` | `result.success` is true |
| `1` | `result` | Factorization finished without a valid factor result |
| `1` | `error` | Validation, configuration, or runtime failure |

Consumers should require a terminal event and a matching process exit status.
End-of-file without a terminal event is an interrupted or invalid run.
