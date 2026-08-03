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
{"schema_version":1,"type":"progress","phase":"sieve","phase_progress":0.5,"elapsed_s":1.25,"message":"collecting relations","relations_found":500,"relations_target":1000,"special_q_done":20,"matrix_rows":0,"matrix_cols":0,"dependency_index":0,"dependencies_total":0}
```

`phase_progress` lies between zero and one when the phase has a bounded estimate.
It is `null` when progress is indeterminate. Counters that do not apply to the
current phase remain zero.

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
