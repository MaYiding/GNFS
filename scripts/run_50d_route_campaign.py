#!/usr/bin/env python3
"""Run and validate an interleaved complete-first-round 50-digit route campaign."""

from __future__ import annotations

import argparse
import copy
from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, NoReturn, Sequence

try:
    from cleanup_50d_probe_artifacts import (
        ProbeCleanupError,
        cleanup_successful_probe_directory,
    )
except ModuleNotFoundError:
    from scripts.cleanup_50d_probe_artifacts import (
        ProbeCleanupError,
        cleanup_successful_probe_directory,
    )


RECORD_PREFIX = "GNFS_EXPERIMENT_V2"
SUMMARY_PREFIX = "GNFS_50D_ROUTE_CAMPAIGN_V1"
ARTIFACT_FORMAT = "gnfs_50d_route_campaign_evidence"
DIAGNOSTIC_FORMAT = "gnfs_50d_route_campaign_failure_diagnostic"
ARTIFACT_VARIANT = "complete_first_round_abba"
SCOPE = "complete_50d_first_round_route_campaign"
CLAIM_BOUNDARY = "relation_reduction_and_matrix_shape_only"
PROBE_N = "16000000000000004000000216000000000000027000000729"
MAX_SPECIAL_Q = 8192
JSON_SAFE_INTEGER_MAX = (1 << 53) - 1
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1

ROUTE_FIELDS = """
scope claim_boundary stop_after pipeline_batch_mode candidate_chunk_size
candidate_rss_sample_policy cofactor_inner_parallel_policy status failure_stage
n n_digits n_bits max_special_q max_special_q_batch_workers special_q_processed
special_q_batch_worker_limit special_q_batch_peak_workers special_q_batch_count
special_q_batch_peak_size max_local_sieve_threads_requested local_sieve_thread_budget
special_q_batch_peak_assigned_threads special_q_worker_peak_sieve_threads candidates_total
candidate_batch_peak_workers candidate_batch_total_chunks candidate_batch_peak_chunks
candidate_batch_peak_candidates candidate_batch_rss_sample_candidates
candidate_batch_after_generation_current_rss_bytes
candidate_batch_after_cofactor_current_rss_bytes
candidate_batch_after_release_current_rss_bytes rational_fb_columns algebraic_fb_columns
base_factor_columns initial_raw_target first_round_complete sieve_rounds_completed
sieve_stop_reason resume_scope attempted_resume attempted_distributed sge_attempted
solver_attempted sqrt_attempted factorization_attempted route route_evidence strategy storage
generation raw_rows raw_duplicates input_lp_columns input_lp_w1 input_lp_w2 input_lp_w3
input_lp_w4plus output_rows output_lp_columns structured_commits structured_emitted_rows
structured_stop incidence_shards incidence_requested_workers incidence_peak_workers
raw_digest_low raw_digest_high output_digest_low output_digest_high matrix_rows matrix_cols
matrix_nonzeros matrix_signed_delta matrix_row_mapping_identity structured_filter_records
structured_matrix_records raw_pair_observed raw_pair_removed output_pair_observed
output_pair_retained_by_matrix output_pair_removed output_lease_removed process_rss_scope
process_rss_backend process_current_rss_supported process_peak_rss_supported
process_current_rss_bytes process_peak_rss_bytes rss_start_current_bytes rss_start_peak_bytes
rss_after_polynomial_current_bytes rss_after_polynomial_peak_bytes
rss_after_factor_base_current_bytes rss_after_factor_base_peak_bytes
rss_after_sieve_current_bytes rss_after_sieve_peak_bytes rss_after_matrix_current_bytes
rss_after_matrix_peak_bytes rss_after_cleanup_current_bytes rss_after_cleanup_peak_bytes
polynomial_ms factor_base_ms sieve_ms candidate_generation_s candidate_cofactor_s matrix_ms
wall_ms error
""".split()

IDENTITY_FIELDS = """
scope claim_boundary stop_after pipeline_batch_mode candidate_chunk_size
candidate_rss_sample_policy cofactor_inner_parallel_policy n_digits n_bits n
max_special_q max_special_q_batch_workers special_q_processed
special_q_batch_worker_limit special_q_batch_peak_workers special_q_batch_count
special_q_batch_peak_size max_local_sieve_threads_requested local_sieve_thread_budget
special_q_batch_peak_assigned_threads special_q_worker_peak_sieve_threads
candidates_total candidate_batch_peak_workers candidate_batch_total_chunks
candidate_batch_peak_chunks candidate_batch_peak_candidates
candidate_batch_rss_sample_candidates rational_fb_columns algebraic_fb_columns
base_factor_columns initial_raw_target sieve_rounds_completed first_round_complete
resume_scope attempted_resume attempted_distributed sge_attempted solver_attempted
sqrt_attempted factorization_attempted raw_rows raw_duplicates input_lp_columns
input_lp_w1 input_lp_w2 input_lp_w3 input_lp_w4plus raw_digest_low raw_digest_high
raw_pair_observed raw_pair_removed
""".split()

ROUTE_STABILITY_FIELDS = """
sieve_stop_reason output_rows output_lp_columns structured_commits structured_emitted_rows
structured_stop output_digest_low output_digest_high matrix_rows matrix_cols matrix_nonzeros
matrix_signed_delta matrix_row_mapping_identity
""".split()

BOOLEAN_FIELDS = {
    "first_round_complete",
    "attempted_resume",
    "attempted_distributed",
    "sge_attempted",
    "solver_attempted",
    "sqrt_attempted",
    "factorization_attempted",
    "matrix_row_mapping_identity",
    "raw_pair_observed",
    "raw_pair_removed",
    "output_pair_observed",
    "output_pair_retained_by_matrix",
    "output_pair_removed",
    "output_lease_removed",
    "process_current_rss_supported",
    "process_peak_rss_supported",
}
OPTIONAL_UINT_FIELDS = {
    "candidate_batch_after_generation_current_rss_bytes",
    "candidate_batch_after_cofactor_current_rss_bytes",
    "candidate_batch_after_release_current_rss_bytes",
    "process_current_rss_bytes",
    "process_peak_rss_bytes",
    "rss_start_current_bytes",
    "rss_start_peak_bytes",
    "rss_after_polynomial_current_bytes",
    "rss_after_polynomial_peak_bytes",
    "rss_after_factor_base_current_bytes",
    "rss_after_factor_base_peak_bytes",
    "rss_after_sieve_current_bytes",
    "rss_after_sieve_peak_bytes",
    "rss_after_matrix_current_bytes",
    "rss_after_matrix_peak_bytes",
    "rss_after_cleanup_current_bytes",
    "rss_after_cleanup_peak_bytes",
}
FLOAT_FIELDS = {"candidate_generation_s", "candidate_cofactor_s"}
SIGNED_OPTIONAL_FIELDS = {"matrix_signed_delta"}
BIG_UINT_FIELDS = {"n"}
STRING_FIELDS = {
    "scope",
    "claim_boundary",
    "stop_after",
    "pipeline_batch_mode",
    "candidate_rss_sample_policy",
    "cofactor_inner_parallel_policy",
    "status",
    "failure_stage",
    "sieve_stop_reason",
    "resume_scope",
    "route",
    "route_evidence",
    "strategy",
    "storage",
    "structured_stop",
    "process_rss_scope",
    "process_rss_backend",
    "error",
}
UINT_FIELDS = set(ROUTE_FIELDS) - (
    BOOLEAN_FIELDS
    | OPTIONAL_UINT_FIELDS
    | FLOAT_FIELDS
    | SIGNED_OPTIONAL_FIELDS
    | BIG_UINT_FIELDS
    | STRING_FIELDS
)

CURRENT_RSS_FIELDS = (
    "process_current_rss_bytes",
    "rss_start_current_bytes",
    "rss_after_polynomial_current_bytes",
    "rss_after_factor_base_current_bytes",
    "rss_after_sieve_current_bytes",
    "rss_after_matrix_current_bytes",
    "rss_after_cleanup_current_bytes",
)
PEAK_RSS_FIELDS = (
    "process_peak_rss_bytes",
    "rss_start_peak_bytes",
    "rss_after_polynomial_peak_bytes",
    "rss_after_factor_base_peak_bytes",
    "rss_after_sieve_peak_bytes",
    "rss_after_matrix_peak_bytes",
    "rss_after_cleanup_peak_bytes",
)
CANDIDATE_RSS_FIELDS = (
    "candidate_batch_after_generation_current_rss_bytes",
    "candidate_batch_after_cofactor_current_rss_bytes",
    "candidate_batch_after_release_current_rss_bytes",
)

SUMMARY_FIELDS = """
schema_version status scope schedule slots_planned slots_completed samples_per_route
identity_fields identity_sha256 source_commit source_tree binary_sha256 max_special_q
max_special_q_batch_workers max_local_sieve_threads per_slot_timeout_s route_stability
structured_positive_delta_samples wall_ratio_ppm peak_rss_ratio_ppm
matrix_nonzeros_ratio_ppm directional_budget artifact_published promotion
""".split()

ROOT_KEYS = {
    "artifact_format",
    "artifact_format_version",
    "record_schema",
    "record_schema_variant",
    "status",
    "claim_boundary",
    "promotion",
    "source",
    "parameters",
    "schedule",
    "identity",
    "slots",
    "route_stability",
    "directional_budget",
    "failure",
    "summary_record",
}
SOURCE_KEYS = {
    "commit",
    "tree",
    "binary_sha256",
    "build_type",
    "host_system",
    "host_machine",
}
PARAMETER_KEYS = {
    "n",
    "max_special_q",
    "max_special_q_batch_workers",
    "max_local_sieve_threads",
    "per_slot_timeout_s",
}
SCHEDULE_KEYS = {
    "policy",
    "samples_per_route",
    "slots_planned",
    "slots_completed",
    "order",
    "started_utc",
    "completed_utc",
}
IDENTITY_KEYS = {"field_count", "field_order", "values", "sha256"}
SLOT_KEYS = {
    "ordinal",
    "route",
    "route_sample_ordinal",
    "status",
    "elapsed_ms",
    "record",
    "record_fields",
    "identity_sha256",
    "artifact_lifecycle",
    "failure",
}
STABILITY_KEYS = {
    "field_count",
    "field_order",
    "legacy_sha256",
    "structured_sha256",
    "legacy_stable",
    "structured_stable",
    "mismatches",
}
BUDGET_KEYS = {
    "status",
    "structured_positive_delta_samples",
    "structured_samples",
    "wall_ratio_ppm",
    "wall_limit_ppm",
    "wall_pass",
    "peak_rss_ratio_ppm",
    "peak_rss_limit_ppm",
    "peak_rss_pass",
    "matrix_nonzeros_ratio_ppm",
    "matrix_nonzeros_limit_ppm",
    "matrix_nonzeros_pass",
    "promotion",
}
FAILURE_KEYS = {"stage", "slot_ordinal", "code", "message", "diagnostic_directory"}
DIAGNOSTIC_ROOT_KEYS = ROOT_KEYS | {"artifact_published"}

UINT_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")
SIGNED_RE = re.compile(r"(?:0|[1-9][0-9]*|-[1-9][0-9]*)\Z")
FLOAT_RE = re.compile(r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\Z")
TOKEN_RE = re.compile(r"[A-Za-z0-9_.-]+\Z")
HEX40_RE = re.compile(r"[0-9a-f]{40}\Z")
HEX64_RE = re.compile(r"[0-9a-f]{64}\Z")
UTC_TIMESTAMP_RE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\Z")


class CampaignError(RuntimeError):
    """A fail-closed campaign contract violation."""

    def __init__(
        self,
        message: str,
        *,
        stage: str = "campaign",
        code: str = "contract_failure",
        slot_ordinal: int | None = None,
    ) -> None:
        super().__init__(message)
        self.stage = stage
        self.code = code
        self.slot_ordinal = slot_ordinal


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CampaignError(message)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def lexical_absolute(path: Path) -> Path:
    """Make a path absolute without following its final symlink component."""
    return Path(os.path.abspath(os.fspath(path)))


def parse_uint(value: str, field: str, *, bounded: bool = True) -> int:
    require(UINT_RE.fullmatch(value) is not None, f"{field} is not canonical unsigned decimal")
    number = int(value)
    if bounded:
        require(number <= UINT64_MAX, f"{field} exceeds uint64")
    return number


def parse_optional_uint(value: str, field: str) -> int | None:
    if value == "na":
        return None
    return parse_uint(value, field)


def parse_route_record(line: str) -> dict[str, str]:
    require(line.isascii(), "route record is not ASCII")
    require("\n" not in line and "\r" not in line and "\x00" not in line,
            "route record contains a forbidden control byte")
    tokens = line.split(" ")
    require(tokens[0] == RECORD_PREFIX, "route record prefix mismatch")
    require(len(tokens) == len(ROUTE_FIELDS) + 1, "route record field count mismatch")

    fields: dict[str, str] = {}
    observed: list[str] = []
    for token in tokens[1:]:
        require(token.count("=") == 1, "route token must contain one equals sign")
        key, value = token.split("=", 1)
        require(key != "" and value != "", "route field key/value must be nonempty")
        require(key not in fields, f"duplicate route field {key}")
        fields[key] = value
        observed.append(key)
    require(observed == ROUTE_FIELDS, "route field set or order differs from the closed schema")

    for field in BOOLEAN_FIELDS:
        require(fields[field] in {"true", "false"}, f"{field} is not canonical boolean")
    for field in UINT_FIELDS:
        parse_uint(fields[field], field)
    for field in BIG_UINT_FIELDS:
        parse_uint(fields[field], field, bounded=False)
    for field in OPTIONAL_UINT_FIELDS:
        parse_optional_uint(fields[field], field)
    for field in SIGNED_OPTIONAL_FIELDS:
        value = fields[field]
        require(value == "na" or SIGNED_RE.fullmatch(value) is not None,
                f"{field} is not canonical signed decimal or na")
        if value != "na":
            number = int(value)
            require(INT64_MIN <= number <= INT64_MAX, f"{field} exceeds int64")
    for field in FLOAT_FIELDS:
        value = fields[field]
        require(FLOAT_RE.fullmatch(value) is not None,
                f"{field} is not canonical nonnegative decimal")
        try:
            number = Decimal(value)
        except InvalidOperation as error:
            raise CampaignError(f"{field} is not a finite decimal") from error
        require(number.is_finite() and number >= 0 and
                number <= Decimal(str(sys.float_info.max)),
                f"{field} must be a finite nonnegative binary64-range decimal")
    for field in STRING_FIELDS:
        require(TOKEN_RE.fullmatch(fields[field]) is not None,
                f"{field} contains a non-token value")

    require(fields["scope"] == "bounded_50d_prefix_probe", "route scope mismatch")
    require(fields["claim_boundary"] == CLAIM_BOUNDARY, "route claim boundary mismatch")
    require(fields["stop_after"] == "matrix_build", "route stop boundary mismatch")
    require(fields["pipeline_batch_mode"] == "two_stage_candidate_batch",
            "route pipeline batch mode mismatch")
    require(fields["candidate_rss_sample_policy"] == "first_max_candidates",
            "route candidate RSS policy mismatch")
    require(fields["cofactor_inner_parallel_policy"] == "forced_sequential",
            "route cofactor inner policy mismatch")
    require(fields["status"] == "pass" and fields["failure_stage"] == "none",
            "campaign received a non-pass route record")
    require(fields["error"] == "none", "pass route record carries an error")
    require(fields["resume_scope"] == "none", "route unexpectedly reports resume scope")
    require(fields["process_rss_scope"] == "self_lifetime", "route RSS scope mismatch")
    require(fields["route"] in {"legacy", "structured"}, "unknown route")
    require(fields["route_evidence"] in {"production_legacy_ooc", "production_direct_ooc"},
            "unknown route evidence")
    require(fields["strategy"] in {"standard_v0", "structured"}, "unknown strategy")
    require(fields["storage"] in {"in_memory", "finalized_ooc"}, "unknown storage")
    require(fields["structured_stop"] in {
        "not_started", "no_candidates", "budget_limit", "persistence_limit"
    }, "unknown structured stop reason")
    require(fields["process_rss_backend"] in {
        "unsupported", "unobserved", "darwin_getrusage", "linux_getrusage", "windows_psapi"
    }, "unknown process RSS backend")
    require(parse_uint(fields["base_factor_columns"], "base_factor_columns") ==
            parse_uint(fields["rational_fb_columns"], "rational_fb_columns") +
            parse_uint(fields["algebraic_fb_columns"], "algebraic_fb_columns"),
            "factor-base column total mismatch")
    require(fields["matrix_signed_delta"] != "na", "pass route lacks matrix delta")
    require(int(fields["matrix_signed_delta"]) ==
            parse_uint(fields["matrix_rows"], "matrix_rows") -
            parse_uint(fields["matrix_cols"], "matrix_cols"),
            "matrix signed delta mismatch")

    candidate_values = [fields[key] for key in CANDIDATE_RSS_FIELDS]
    require(all(value == "na" for value in candidate_values) or
            all(value != "na" for value in candidate_values),
            "candidate RSS fields are partially populated")
    current_supported = fields["process_current_rss_supported"] == "true"
    peak_supported = fields["process_peak_rss_supported"] == "true"
    require(all((fields[key] != "na") == current_supported for key in CURRENT_RSS_FIELDS),
            "current RSS support/value linkage mismatch")
    require(all((fields[key] != "na") == peak_supported for key in PEAK_RSS_FIELDS),
            "peak RSS support/value linkage mismatch")
    require((candidate_values[0] != "na") == current_supported,
            "candidate/process current RSS support mismatch")
    require(fields["process_current_rss_bytes"] == fields["rss_after_cleanup_current_bytes"],
            "terminal current RSS mismatch")
    require(fields["process_peak_rss_bytes"] == fields["rss_after_cleanup_peak_bytes"],
            "terminal peak RSS mismatch")
    if fields["process_rss_backend"] in {"unsupported", "unobserved"}:
        require(not current_supported and not peak_supported,
                "unsupported RSS backend reports supported values")
    else:
        require(current_supported and peak_supported, "supported RSS backend omitted values")
        phase_currents = [int(fields[key]) for key in CURRENT_RSS_FIELDS[1:]]
        phase_peaks = [int(fields[key]) for key in PEAK_RSS_FIELDS[1:]]
        require(all(current <= peak for current, peak in zip(phase_currents, phase_peaks)),
                "phase current RSS exceeds lifetime peak")
        require(phase_peaks == sorted(phase_peaks), "lifetime peak RSS regressed")
    return fields


def validate_route_contract(
    fields: dict[str, str],
    route: str,
    max_batch_workers: int,
    max_local_threads: str,
) -> None:
    require(fields["route"] == route, "route record identifies a different route")
    require(fields["n"] == PROBE_N and fields["n_digits"] == "50" and fields["n_bits"] == "164",
            "route target identity mismatch")
    require(fields["candidate_chunk_size"] == "256",
            "route candidate chunk size differs from the production constant")
    require(fields["max_special_q"] == str(MAX_SPECIAL_Q), "route special-Q cap mismatch")
    require(fields["max_special_q_batch_workers"] == str(max_batch_workers),
            "route batch-worker request mismatch")
    requested_threads = "0" if max_local_threads == "auto" else max_local_threads
    require(fields["max_local_sieve_threads_requested"] == requested_threads,
            "route local-thread request mismatch")
    require(fields["first_round_complete"] == "true" and
            fields["sieve_rounds_completed"] == "1",
            "route did not complete exactly the first sieve round")
    require(fields["sieve_stop_reason"] in {
        "adaptive_round_limit_reached", "effective_column_excess"
    }, "route stopped outside the complete-first-round boundary")
    require(fields["attempted_resume"] == "false" and
            fields["attempted_distributed"] == "false" and
            fields["sge_attempted"] == "false" and
            fields["solver_attempted"] == "false" and
            fields["sqrt_attempted"] == "false" and
            fields["factorization_attempted"] == "false",
            "route crossed the matrix-only campaign boundary")
    require(fields["raw_pair_observed"] == "true" and fields["raw_pair_removed"] == "true",
            "route did not prove the raw OOC pair lifecycle")

    special_q_processed = int(fields["special_q_processed"])
    worker_limit = int(fields["special_q_batch_worker_limit"])
    local_budget = int(fields["local_sieve_thread_budget"])
    peak_batch_size = min(4, special_q_processed)
    require(0 < special_q_processed <= MAX_SPECIAL_Q,
            "complete-first-round route processed an invalid special-Q count")
    require(local_budget > 0 and worker_limit == min(max_batch_workers, local_budget),
            "special-Q worker limit differs from the frozen effective cap")
    if max_local_threads != "auto":
        require(local_budget <= int(max_local_threads),
                "effective local-thread budget exceeds its explicit request")
    require(int(fields["special_q_batch_count"]) == (special_q_processed + 3) // 4 and
            int(fields["special_q_batch_peak_size"]) == peak_batch_size and
            int(fields["special_q_batch_peak_workers"]) == min(worker_limit, peak_batch_size) and
            int(fields["special_q_batch_peak_assigned_threads"]) == local_budget,
            "special-Q batch topology differs from the fixed-width schedule")
    final_batch_size = special_q_processed % 4 or peak_batch_size
    final_batch_workers = min(worker_limit, final_batch_size)
    expected_peak_worker_threads = (
        local_budget + final_batch_workers - 1
    ) // final_batch_workers
    require(int(fields["special_q_worker_peak_sieve_threads"]) ==
            expected_peak_worker_threads,
            "per-worker sieve-thread topology differs from the total budget")

    candidates_total = int(fields["candidates_total"])
    candidate_total_chunks = int(fields["candidate_batch_total_chunks"])
    candidate_peak_chunks = int(fields["candidate_batch_peak_chunks"])
    candidate_peak_candidates = int(fields["candidate_batch_peak_candidates"])
    require(candidates_total > 0 and candidate_total_chunks > 0 and
            0 < candidate_peak_chunks <= candidate_total_chunks and
            int(fields["candidate_batch_peak_workers"]) ==
            min(local_budget, candidate_peak_chunks) and
            0 < candidate_peak_candidates <= candidates_total and
            int(fields["candidate_batch_rss_sample_candidates"]) ==
            candidate_peak_candidates,
            "candidate batch topology differs from the frozen two-stage schedule")
    require(Decimal(fields["candidate_generation_s"]) > 0 and
            Decimal(fields["candidate_cofactor_s"]) > 0,
            "candidate batch timings were not recorded")

    raw_rows = int(fields["raw_rows"])
    input_lp_columns = int(fields["input_lp_columns"])
    output_rows = int(fields["output_rows"])
    output_lp_columns = int(fields["output_lp_columns"])
    matrix_rows = int(fields["matrix_rows"])
    matrix_cols = int(fields["matrix_cols"])
    require(int(fields["generation"]) > 0, "reduction generation is zero")
    require(int(fields["raw_duplicates"]) == 0,
            "collector unique-prefix route reported raw duplicates")
    require(sum(int(fields[key]) for key in (
                "input_lp_w1", "input_lp_w2", "input_lp_w3", "input_lp_w4plus"
            )) == input_lp_columns,
            "input LP histogram differs from the unique-column count")
    require(int(fields["initial_raw_target"]) > 0 and
            raw_rows >= int(fields["initial_raw_target"]),
            "complete-first-round route lacks its raw-target evidence")
    require(output_rows <= raw_rows and output_lp_columns <= input_lp_columns,
            "reduction output exceeds its raw input evidence")
    require(matrix_rows <= output_rows and
            matrix_cols >= int(fields["base_factor_columns"]),
            "matrix shape crosses its reduction/factor-base boundary")

    expected = {
        "legacy": {
            "route_evidence": "production_legacy_ooc",
            "strategy": "standard_v0",
            "storage": "in_memory",
            "structured_stop": "not_started",
        },
        "structured": {
            "route_evidence": "production_direct_ooc",
            "strategy": "structured",
            "storage": "finalized_ooc",
        },
    }[route]
    for key, value in expected.items():
        require(fields[key] == value, f"{route} field {key} mismatch")

    if route == "legacy":
        require(int(fields["structured_commits"]) == 0 and
                int(fields["structured_emitted_rows"]) == 0 and
                int(fields["incidence_shards"]) == 0 and
                int(fields["incidence_requested_workers"]) == 0 and
                int(fields["incidence_peak_workers"]) == 0 and
                int(fields["structured_filter_records"]) == 0 and
                int(fields["structured_matrix_records"]) == 0 and
                fields["matrix_row_mapping_identity"] == "true" and
                all(fields[key] == "false" for key in (
                    "output_pair_observed", "output_pair_retained_by_matrix",
                    "output_pair_removed", "output_lease_removed",
                )),
                "legacy route reported structured/OOC output activity")
    else:
        incidence_requested = int(fields["incidence_requested_workers"])
        incidence_peak = int(fields["incidence_peak_workers"])
        require(fields["structured_stop"] != "not_started" and
                int(fields["structured_filter_records"]) == 1 and
                int(fields["structured_matrix_records"]) == 1 and
                int(fields["incidence_shards"]) > 0 and
                incidence_requested > 0 and 0 < incidence_peak <= incidence_requested and
                all(fields[key] == "true" for key in (
                    "output_pair_observed", "output_pair_retained_by_matrix",
                    "output_pair_removed", "output_lease_removed",
                )),
                "structured route lacks direct-OOC lifecycle/topology evidence")


def canonical_digest(fields: dict[str, str], order: Sequence[str]) -> str:
    payload = "".join(f"{key}={fields[key]}\n" for key in order).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def campaign_order(samples_per_route: int) -> list[str]:
    require(2 <= samples_per_route <= 9, "samples_per_route must be in 2..9")
    cycle = ("legacy", "structured", "structured", "legacy",
             "structured", "legacy", "legacy", "structured")
    return [cycle[index % len(cycle)] for index in range(samples_per_route * 2)]


def build_probe_command(
    executable: Path,
    route: str,
    max_batch_workers: int,
    max_local_threads: str,
    raw_base: Path,
) -> list[str]:
    require(route in {"legacy", "structured"}, "probe command route is invalid")
    require(1 <= max_batch_workers <= 4, "probe command batch-worker limit is invalid")
    require(max_local_threads == "auto" or
            (UINT_RE.fullmatch(max_local_threads) is not None and
             0 < int(max_local_threads) <= UINT32_MAX),
            "probe command local-thread limit is invalid")
    command = [
        str(executable),
        "--strategy", route,
        "--max-special-q", str(MAX_SPECIAL_Q),
        "--max-special-q-batch-workers", str(max_batch_workers),
    ]
    if max_local_threads != "auto":
        command.extend(["--max-local-sieve-threads", max_local_threads])
    command.extend(["--ooc-base", str(raw_base)])
    require(command.count("--strategy") == 1 and
            command.count("--max-special-q") == 1 and
            command.count("--max-special-q-batch-workers") == 1 and
            command.count("--ooc-base") == 1 and
            command.count("--max-local-sieve-threads") ==
            (0 if max_local_threads == "auto" else 1),
            "probe command contains a missing or duplicate option")
    return command


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(project_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(project_root), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise CampaignError(f"git {' '.join(arguments)} failed: {detail}",
                            stage="source", code="git_failure")
    try:
        return result.stdout.decode("ascii").strip()
    except UnicodeDecodeError as error:
        raise CampaignError("git provenance output is not ASCII",
                            stage="source", code="git_failure") from error


def tracked_source_is_clean(project_root: Path) -> bool:
    for arguments in (("diff", "--quiet", "--ignore-submodules", "--"),
                      ("diff", "--cached", "--quiet", "--ignore-submodules", "--")):
        result = subprocess.run(
            ["git", "-C", str(project_root), *arguments],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode == 1:
            return False
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            raise CampaignError(f"git cleanliness check failed: {detail}",
                                stage="source", code="git_failure")
    return True


@dataclass(frozen=True)
class SourceIdentity:
    commit: str
    tree: str
    binary_sha256: str


def capture_source_identity(project_root: Path, executable: Path) -> SourceIdentity:
    require(tracked_source_is_clean(project_root), "tracked source is dirty")
    commit = git_output(project_root, "rev-parse", "HEAD")
    tree = git_output(project_root, "rev-parse", "HEAD^{tree}")
    require(HEX40_RE.fullmatch(commit) is not None, "source commit is not canonical SHA-1")
    require(HEX40_RE.fullmatch(tree) is not None, "source tree is not canonical SHA-1")
    binary_sha256 = sha256_file(executable)
    require(HEX64_RE.fullmatch(binary_sha256) is not None, "binary SHA-256 is malformed")
    return SourceIdentity(commit=commit, tree=tree, binary_sha256=binary_sha256)


def verify_release_build(executable: Path) -> None:
    cache_path = executable.parent / "CMakeCache.txt"
    try:
        metadata = os.lstat(cache_path)
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1 and
                0 < metadata.st_size <= 16 * 1024 * 1024,
                "probe build cache is not a bounded single-link regular file")
        cache_text = cache_path.read_text(encoding="utf-8")
    except CampaignError:
        raise
    except (OSError, UnicodeDecodeError) as error:
        raise CampaignError(
            f"cannot read probe build cache: {error}",
            stage="source",
            code="build_type_unverified",
        ) from error
    build_type_lines = [
        line for line in cache_text.splitlines()
        if line.startswith("CMAKE_BUILD_TYPE:")
    ]
    require(build_type_lines == ["CMAKE_BUILD_TYPE:STRING=Release"],
            "probe executable is not bound to a single-config Release CMake cache")


def verify_source_identity(
    project_root: Path, executable: Path, expected: SourceIdentity, *, slot: int | None
) -> None:
    try:
        observed = capture_source_identity(project_root, executable)
    except CampaignError as error:
        raise CampaignError(str(error), stage="source", code=error.code,
                            slot_ordinal=slot) from error
    if observed != expected:
        raise CampaignError(
            "source commit/tree or probe binary changed during the campaign",
            stage="source",
            code="source_identity_drift",
            slot_ordinal=slot,
        )


def extract_single_record(stdout_path: Path, stderr_path: Path) -> str:
    matches: list[bytes] = []
    prefix = (RECORD_PREFIX + " ").encode("ascii")
    for stream_path in (stdout_path, stderr_path):
        data = stream_path.read_bytes()
        for line in data.splitlines():
            if line.startswith(prefix):
                matches.append(line)
    require(len(matches) == 1,
            f"probe must emit exactly one {RECORD_PREFIX} record; observed {len(matches)}")
    try:
        return matches[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise CampaignError("probe record is not ASCII") from error


@dataclass
class SlotResult:
    ordinal: int
    route: str
    route_sample_ordinal: int
    status: str
    elapsed_ms: int
    record: str | None
    record_fields: dict[str, str] | None
    identity_sha256: str | None
    artifact_lifecycle: str
    failure: str | None

    def to_json(self) -> dict[str, Any]:
        return {
            "ordinal": self.ordinal,
            "route": self.route,
            "route_sample_ordinal": self.route_sample_ordinal,
            "status": self.status,
            "elapsed_ms": self.elapsed_ms,
            "record": self.record,
            "record_fields": self.record_fields,
            "identity_sha256": self.identity_sha256,
            "artifact_lifecycle": self.artifact_lifecycle,
            "failure": self.failure,
        }


def process_group_exists(process_group: int) -> bool:
    require(os.name == "posix" and hasattr(os, "killpg"),
            "campaign process groups require POSIX")
    try:
        os.killpg(process_group, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def campaign_termination_signals() -> tuple[int, ...]:
    return tuple(
        int(getattr(signal, name))
        for name in ("SIGHUP", "SIGINT", "SIGTERM")
        if hasattr(signal, name)
    )


def wait_for_process_group_exit(
    process: subprocess.Popen[Any],
    process_group: int,
    timeout_s: float,
) -> bool:
    deadline = time.monotonic() + timeout_s
    while True:
        process.poll()
        if not process_group_exists(process_group):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def signal_process_group(process_group: int, signal_number: int, label: str) -> None:
    try:
        os.killpg(process_group, signal_number)
    except ProcessLookupError:
        return
    except OSError as error:
        raise CampaignError(
            f"cannot signal {label} process group: {error}",
            stage="process",
            code="child_termination_failure",
        ) from error


def terminate_process(
    process: subprocess.Popen[Any],
    label: str,
    grace_s: float = 5.0,
) -> None:
    require(os.name == "posix" and hasattr(os, "killpg"),
            "campaign child cleanup requires POSIX process groups")
    process_group = process.pid
    if process.poll() is None:
        try:
            require(os.getpgid(process.pid) == process_group,
                    f"{label} does not own a fresh process group")
        except ProcessLookupError:
            process.poll()
    if not process_group_exists(process_group):
        return
    signal_process_group(process_group, signal.SIGTERM, label)
    if wait_for_process_group_exit(process, process_group, grace_s):
        return
    signal_process_group(process_group, signal.SIGKILL, label)
    if wait_for_process_group_exit(process, process_group, grace_s):
        return
    raise CampaignError(
        f"{label} process group survived terminate and kill",
        stage="process",
        code="child_termination_failure",
    )


def wait_for_process(
    process: subprocess.Popen[Any],
    started: float,
    timeout_s: int,
    label: str,
    sleep_function: Any = time.sleep,
) -> bool:
    next_heartbeat = 30
    try:
        while process.poll() is None:
            elapsed = time.monotonic() - started
            if elapsed >= timeout_s:
                terminate_process(process, label)
                return True
            if elapsed >= next_heartbeat:
                print(f"{label}: running for {int(elapsed)}s", file=sys.stderr, flush=True)
                next_heartbeat += 30
            sleep_function(min(1.0, max(0.05, timeout_s - elapsed)))
        return False
    except BaseException as error:
        try:
            terminate_process(process, label)
        except CampaignError as termination_error:
            raise CampaignError(
                f"{label} was interrupted and child cleanup failed: {termination_error}",
                stage="process",
                code="interrupted",
            ) from error
        raise CampaignError(
            f"{label} was interrupted: {type(error).__name__}: {error}",
            stage="process",
            code="interrupted",
        ) from error


def run_process(
    arguments: Sequence[str],
    cwd: Path,
    stdout_path: Path,
    stderr_path: Path,
    timeout_s: int,
    label: str,
    popen_factory: Any = subprocess.Popen,
) -> tuple[int, int, bool]:
    require(os.name == "posix" and hasattr(os, "killpg"),
            "campaign process execution requires POSIX process groups")
    started = time.monotonic()
    process: subprocess.Popen[Any] | None = None
    deferred_signals: list[int] = []
    previous_signal_handlers: list[tuple[int, Any]] = []

    def defer_signal(signal_number: int, _frame: Any) -> None:
        deferred_signals.append(signal_number)

    def restore_signal_handlers() -> None:
        if not previous_signal_handlers:
            return
        for signal_number, previous_handler in reversed(previous_signal_handlers):
            signal.signal(signal_number, previous_handler)
        previous_signal_handlers.clear()

    with stdout_path.open("wb") as stdout_handle, stderr_path.open("wb") as stderr_handle:
        try:
            for signal_number in campaign_termination_signals():
                previous_handler = signal.signal(signal_number, defer_signal)
                previous_signal_handlers.append((signal_number, previous_handler))
            process = popen_factory(
                list(arguments),
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=stdout_handle,
                stderr=stderr_handle,
                shell=False,
                start_new_session=True,
            )
            restore_signal_handlers()
            if deferred_signals:
                raise CampaignError(
                    f"{label} was interrupted by signal {deferred_signals[0]}",
                    stage="process",
                    code="interrupted",
                )
            timed_out = wait_for_process(process, started, timeout_s, label)
            return_code = process.returncode if process.returncode is not None else -1
            if process_group_exists(process.pid):
                terminate_process(process, label)
                raise CampaignError(
                    f"{label} exited while descendant processes remained",
                    stage="process",
                    code="descendant_process_leak",
                )
        except BaseException as error:
            if process is not None:
                try:
                    terminate_process(process, label)
                except CampaignError as termination_error:
                    raise CampaignError(
                        f"{label} failed and child cleanup failed: {termination_error}",
                        stage="process",
                        code="interrupted",
                    ) from error
            if isinstance(error, CampaignError):
                raise
            if process is None:
                raise
            raise CampaignError(
                f"{label} was interrupted: {type(error).__name__}: {error}",
                stage="process",
                code="interrupted",
            ) from error
        finally:
            restore_signal_handlers()
    elapsed_ms = max(0, int((time.monotonic() - started) * 1000))
    return return_code, elapsed_ms, timed_out


def median_fraction(values: Sequence[int]) -> Fraction:
    require(len(values) > 0, "cannot compute the median of an empty sample")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return Fraction(ordered[middle], 1)
    return Fraction(ordered[middle - 1] + ordered[middle], 2)


def ratio_ppm(numerator: Fraction, denominator: Fraction) -> int | None:
    if denominator <= 0:
        return None
    ratio = numerator / denominator * 1_000_000
    return (ratio.numerator + ratio.denominator - 1) // ratio.denominator


def route_stability(slots: Sequence[SlotResult]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "field_count": len(ROUTE_STABILITY_FIELDS),
        "field_order": list(ROUTE_STABILITY_FIELDS),
        "legacy_sha256": None,
        "structured_sha256": None,
        "legacy_stable": False,
        "structured_stable": False,
        "mismatches": [],
    }
    for route in ("legacy", "structured"):
        route_slots = [slot for slot in slots if slot.route == route]
        require(route_slots and all(slot.record_fields is not None for slot in route_slots),
                f"{route} lacks validated route samples")
        reference = route_slots[0].record_fields
        assert reference is not None
        result[f"{route}_sha256"] = canonical_digest(reference, ROUTE_STABILITY_FIELDS)
        for slot in route_slots[1:]:
            assert slot.record_fields is not None
            for field in ROUTE_STABILITY_FIELDS:
                if slot.record_fields[field] != reference[field]:
                    result["mismatches"].append({
                        "route": route,
                        "slot_ordinal": slot.ordinal,
                        "field": field,
                        "expected": reference[field],
                        "actual": slot.record_fields[field],
                    })
        result[f"{route}_stable"] = not any(
            mismatch["route"] == route for mismatch in result["mismatches"]
        )
    return result


def directional_budget(slots: Sequence[SlotResult], stability: dict[str, Any]) -> dict[str, Any]:
    legacy = [slot.record_fields for slot in slots if slot.route == "legacy"]
    structured = [slot.record_fields for slot in slots if slot.route == "structured"]
    require(all(fields is not None for fields in legacy + structured),
            "budget lacks validated route fields")
    legacy_fields = [fields for fields in legacy if fields is not None]
    structured_fields = [fields for fields in structured if fields is not None]

    positive = sum(int(fields["matrix_signed_delta"]) > 0 for fields in structured_fields)
    legacy_wall = median_fraction([int(fields["wall_ms"]) for fields in legacy_fields])
    structured_wall = median_fraction([int(fields["wall_ms"]) for fields in structured_fields])
    wall_ratio = ratio_ppm(structured_wall, legacy_wall)
    wall_pass = wall_ratio is not None and wall_ratio <= 1_200_000

    rss_backends = {fields["process_rss_backend"] for fields in legacy_fields + structured_fields}
    rss_supported = (
        len(rss_backends) == 1
        and not rss_backends.intersection({"unsupported", "unobserved"})
        and all(fields["process_peak_rss_supported"] == "true"
                for fields in legacy_fields + structured_fields)
    )
    peak_ratio: int | None = None
    peak_pass = False
    if rss_supported:
        legacy_peak = median_fraction(
            [int(fields["process_peak_rss_bytes"]) for fields in legacy_fields]
        )
        structured_peak = median_fraction(
            [int(fields["process_peak_rss_bytes"]) for fields in structured_fields]
        )
        peak_ratio = ratio_ppm(structured_peak, legacy_peak)
        peak_pass = peak_ratio is not None and peak_ratio <= 1_600_000

    legacy_nonzeros = median_fraction(
        [int(fields["matrix_nonzeros"]) for fields in legacy_fields]
    )
    structured_nonzeros = median_fraction(
        [int(fields["matrix_nonzeros"]) for fields in structured_fields]
    )
    nonzeros_ratio = ratio_ppm(structured_nonzeros, legacy_nonzeros)
    nonzeros_pass = nonzeros_ratio is not None and nonzeros_ratio <= 30_000_000

    all_pass = (
        stability["legacy_stable"]
        and stability["structured_stable"]
        and positive == len(structured_fields)
        and wall_pass
        and peak_pass
        and nonzeros_pass
    )
    status = "pass" if all_pass else ("unavailable" if not rss_supported else "fail")
    return {
        "status": status,
        "structured_positive_delta_samples": positive,
        "structured_samples": len(structured_fields),
        "wall_ratio_ppm": wall_ratio,
        "wall_limit_ppm": 1_200_000,
        "wall_pass": wall_pass,
        "peak_rss_ratio_ppm": peak_ratio,
        "peak_rss_limit_ppm": 1_600_000,
        "peak_rss_pass": peak_pass,
        "matrix_nonzeros_ratio_ppm": nonzeros_ratio,
        "matrix_nonzeros_limit_ppm": 30_000_000,
        "matrix_nonzeros_pass": nonzeros_pass,
        "promotion": False,
    }


def summary_record(
    *,
    status: str,
    order: Sequence[str],
    slots_completed: int,
    samples_per_route: int,
    identity_sha256: str | None,
    source: SourceIdentity,
    max_batch_workers: int,
    max_local_threads: str,
    timeout_s: int,
    stability: dict[str, Any] | None,
    budget: dict[str, Any] | None,
    artifact_published: bool,
) -> str:
    values: dict[str, str] = {
        "schema_version": "1",
        "status": status,
        "scope": SCOPE,
        "schedule": "abba_baab_prefix_v1",
        "slots_planned": str(len(order)),
        "slots_completed": str(slots_completed),
        "samples_per_route": str(samples_per_route),
        "identity_fields": str(len(IDENTITY_FIELDS)),
        "identity_sha256": identity_sha256 or "na",
        "source_commit": source.commit,
        "source_tree": source.tree,
        "binary_sha256": source.binary_sha256,
        "max_special_q": str(MAX_SPECIAL_Q),
        "max_special_q_batch_workers": str(max_batch_workers),
        "max_local_sieve_threads": max_local_threads,
        "per_slot_timeout_s": str(timeout_s),
        "route_stability": (
            "pass" if stability is not None and stability["legacy_stable"]
            and stability["structured_stable"] else "fail"
        ),
        "structured_positive_delta_samples": (
            str(budget["structured_positive_delta_samples"]) if budget is not None else "na"
        ),
        "wall_ratio_ppm": (
            str(budget["wall_ratio_ppm"]) if budget is not None
            and budget["wall_ratio_ppm"] is not None else "na"
        ),
        "peak_rss_ratio_ppm": (
            str(budget["peak_rss_ratio_ppm"]) if budget is not None
            and budget["peak_rss_ratio_ppm"] is not None else "na"
        ),
        "matrix_nonzeros_ratio_ppm": (
            str(budget["matrix_nonzeros_ratio_ppm"]) if budget is not None
            and budget["matrix_nonzeros_ratio_ppm"] is not None else "na"
        ),
        "directional_budget": budget["status"] if budget is not None else "not_evaluated",
        "artifact_published": "true" if artifact_published else "false",
        "promotion": "false",
    }
    return SUMMARY_PREFIX + " " + " ".join(f"{key}={values[key]}" for key in SUMMARY_FIELDS)


def parse_summary_record(line: str) -> dict[str, str]:
    require(line.isascii() and "\n" not in line and "\r" not in line and "\x00" not in line,
            "campaign summary is not one ASCII line")
    tokens = line.split(" ")
    require(tokens[0] == SUMMARY_PREFIX, "campaign summary prefix mismatch")
    require(len(tokens) == len(SUMMARY_FIELDS) + 1, "campaign summary field count mismatch")
    fields: dict[str, str] = {}
    observed: list[str] = []
    for token in tokens[1:]:
        require(token.count("=") == 1, "campaign summary token is not key=value")
        key, value = token.split("=", 1)
        require(key and value and key not in fields, "campaign summary has invalid fields")
        fields[key] = value
        observed.append(key)
    require(observed == SUMMARY_FIELDS, "campaign summary field set/order mismatch")
    require(fields["schema_version"] == "1" and fields["scope"] == SCOPE,
            "campaign summary identity mismatch")
    require(fields["schedule"] == "abba_baab_prefix_v1", "campaign schedule mismatch")
    require(fields["status"] in {"pass", "fail"}, "campaign summary status mismatch")
    require(fields["promotion"] == "false", "campaign summary promotion mismatch")
    require((fields["status"] == "pass" and fields["artifact_published"] == "true") or
            (fields["status"] == "fail" and fields["artifact_published"] == "false"),
            "campaign summary status/publication mismatch")
    numeric: dict[str, int] = {}
    for key in (
        "slots_planned", "slots_completed", "samples_per_route", "identity_fields",
        "max_special_q", "max_special_q_batch_workers", "per_slot_timeout_s",
    ):
        numeric[key] = parse_uint(fields[key], key)
    require(2 <= numeric["samples_per_route"] <= 9 and
            numeric["slots_planned"] == 2 * numeric["samples_per_route"] and
            numeric["slots_completed"] <= numeric["slots_planned"] and
            numeric["identity_fields"] == len(IDENTITY_FIELDS) and
            numeric["max_special_q"] == MAX_SPECIAL_Q and
            1 <= numeric["max_special_q_batch_workers"] <= 4 and
            0 < numeric["per_slot_timeout_s"] <= UINT32_MAX,
            "campaign summary cardinality/parameter binding is invalid")
    for key in (
        "structured_positive_delta_samples", "wall_ratio_ppm", "peak_rss_ratio_ppm",
        "matrix_nonzeros_ratio_ppm",
    ):
        if fields[key] != "na":
            numeric[key] = parse_uint(fields[key], key)
    require(fields["max_local_sieve_threads"] == "auto" or
            (UINT_RE.fullmatch(fields["max_local_sieve_threads"]) is not None and
             0 < int(fields["max_local_sieve_threads"]) <= UINT32_MAX),
            "campaign summary local-thread field is invalid")
    require(fields["identity_sha256"] == "na" or
            HEX64_RE.fullmatch(fields["identity_sha256"]) is not None,
            "campaign summary identity SHA is invalid")
    require(HEX40_RE.fullmatch(fields["source_commit"]) is not None and
            HEX40_RE.fullmatch(fields["source_tree"]) is not None and
            HEX64_RE.fullmatch(fields["binary_sha256"]) is not None,
            "campaign summary provenance is invalid")
    require(fields["route_stability"] in {"pass", "fail"} and
            fields["directional_budget"] in {
                "pass", "fail", "unavailable", "not_evaluated"
            },
            "campaign summary terminal status is invalid")
    if fields["directional_budget"] == "pass":
        require(fields["structured_positive_delta_samples"] ==
                fields["samples_per_route"] and
                all(fields[key] != "na" for key in (
                    "wall_ratio_ppm", "peak_rss_ratio_ppm",
                    "matrix_nonzeros_ratio_ppm",
                )) and
                numeric["wall_ratio_ppm"] <= 1_200_000 and
                numeric["peak_rss_ratio_ppm"] <= 1_600_000 and
                numeric["matrix_nonzeros_ratio_ppm"] <= 30_000_000,
                "passing campaign budget lacks complete ratios/samples")
    elif fields["structured_positive_delta_samples"] != "na":
        require(int(fields["structured_positive_delta_samples"]) <=
                numeric["samples_per_route"],
                "campaign summary positive-delta count exceeds its sample count")
    if fields["route_stability"] == "pass":
        require(numeric["slots_completed"] == numeric["slots_planned"],
                "stable route summary lacks a complete schedule")
    if fields["status"] == "pass":
        require(numeric["slots_completed"] == numeric["slots_planned"] and
                fields["identity_sha256"] != "na" and
                fields["route_stability"] == "pass" and
                fields["directional_budget"] == "pass",
                "pass summary lacks complete identity/stability/budget evidence")
    return fields


def validate_source_object(source: Any) -> SourceIdentity:
    require(type(source) is dict and set(source) == SOURCE_KEYS,
            "campaign source object is not closed")
    require(all(type(source[key]) is str for key in SOURCE_KEYS),
            "campaign source values are not strings")
    require(HEX40_RE.fullmatch(source["commit"]) is not None and
            HEX40_RE.fullmatch(source["tree"]) is not None and
            HEX64_RE.fullmatch(source["binary_sha256"]) is not None and
            source["build_type"] == "Release" and
            source["host_system"] in {"linux", "darwin"} and
            TOKEN_RE.fullmatch(source["host_machine"]) is not None,
            "campaign source provenance is invalid")
    return SourceIdentity(source["commit"], source["tree"], source["binary_sha256"])


def validate_parameter_object(parameters: Any) -> tuple[int, str, int]:
    require(type(parameters) is dict and set(parameters) == PARAMETER_KEYS,
            "campaign parameters object is not closed")
    batch_workers = parameters["max_special_q_batch_workers"]
    local_threads = parameters["max_local_sieve_threads"]
    timeout_s = parameters["per_slot_timeout_s"]
    require(type(parameters["n"]) is str and parameters["n"] == PROBE_N and
            type(parameters["max_special_q"]) is int and
            parameters["max_special_q"] == MAX_SPECIAL_Q and
            type(batch_workers) is int and 1 <= batch_workers <= 4 and
            type(local_threads) is str and
            (local_threads == "auto" or
             (UINT_RE.fullmatch(local_threads) is not None and 0 < int(local_threads) <= UINT32_MAX)) and
            type(timeout_s) is int and 0 < timeout_s <= UINT32_MAX,
            "campaign parameters are invalid")
    return batch_workers, local_threads, timeout_s


def validate_schedule_object(schedule: Any) -> tuple[int, list[str]]:
    require(type(schedule) is dict and set(schedule) == SCHEDULE_KEYS,
            "campaign schedule object is not closed")
    require(type(schedule["policy"]) is str and
            schedule["policy"] == "abba_baab_prefix_v1",
            "campaign artifact schedule policy mismatch")
    samples = schedule["samples_per_route"]
    require(type(samples) is int and 2 <= samples <= 9,
            "campaign artifact sample count is invalid")
    expected_order = campaign_order(samples)
    require(type(schedule["order"]) is list and schedule["order"] == expected_order,
            "campaign artifact route order mismatch")
    require(type(schedule["slots_planned"]) is int and
            schedule["slots_planned"] == len(expected_order) and
            type(schedule["slots_completed"]) is int and
            0 <= schedule["slots_completed"] <= schedule["slots_planned"],
            "campaign artifact slot counts are invalid")
    started = parse_campaign_timestamp(schedule["started_utc"], "started_utc")
    completed = parse_campaign_timestamp(schedule["completed_utc"], "completed_utc")
    require(completed >= started, "campaign artifact timestamps are reversed")
    return samples, expected_order


def parse_campaign_timestamp(value: Any, field: str) -> datetime:
    require(type(value) is str and UTC_TIMESTAMP_RE.fullmatch(value) is not None,
            f"campaign {field} is not a canonical UTC timestamp")
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError as error:
        raise CampaignError(f"campaign {field} is not a valid UTC timestamp") from error
    require(parsed.strftime("%Y-%m-%dT%H:%M:%SZ") == value,
            f"campaign {field} is not canonical")
    return parsed


def strict_json_equal(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        return False
    if type(left) is dict:
        return (set(left) == set(right) and
                all(strict_json_equal(left[key], right[key]) for key in left))
    if type(left) is list:
        return (len(left) == len(right) and
                all(strict_json_equal(a, b) for a, b in zip(left, right)))
    return bool(left == right)


def validate_slot_object(
    slot: Any,
    index: int,
    expected_route: str,
    expected_sample_ordinal: int,
    max_batch_workers: int,
    max_local_threads: str,
    *,
    allow_failure: bool,
) -> SlotResult:
    require(type(slot) is dict and set(slot) == SLOT_KEYS,
            "campaign slot object is not closed")
    require(type(slot["ordinal"]) is int and slot["ordinal"] == index and
            type(slot["route"]) is str and slot["route"] == expected_route,
            "campaign slot ordinal/order mismatch")
    require(type(slot["route_sample_ordinal"]) is int and
            slot["route_sample_ordinal"] == expected_sample_ordinal,
            "campaign route sample ordinal mismatch")
    require(type(slot["elapsed_ms"]) is int and slot["elapsed_ms"] >= 0,
            "campaign slot elapsed time is invalid")
    require(slot["status"] in {"pass", "fail"}, "campaign slot status is invalid")
    if slot["status"] == "pass":
        require(type(slot["record"]) is str and
                type(slot["record_fields"]) is dict and
                list(slot["record_fields"]) == ROUTE_FIELDS and
                all(type(value) is str for value in slot["record_fields"].values()) and
                type(slot["identity_sha256"]) is str and
                slot["artifact_lifecycle"] == "cleaned" and slot["failure"] is None,
                "passing campaign slot lacks closed evidence")
        parsed = parse_route_record(slot["record"])
        require(strict_json_equal(parsed, slot["record_fields"]),
                "campaign slot record and record_fields differ")
        validate_route_contract(parsed, expected_route, max_batch_workers, max_local_threads)
        digest = canonical_digest(parsed, IDENTITY_FIELDS)
        require(slot["identity_sha256"] == digest,
                "campaign slot identity digest is not canonical")
        return SlotResult(
            index, expected_route, expected_sample_ordinal, "pass", slot["elapsed_ms"],
            slot["record"], parsed, digest, "cleaned", None,
        )

    require(allow_failure and slot["artifact_lifecycle"] == "retained" and
            type(slot["failure"]) is str and slot["failure"] != "" and
            (slot["record"] is None or type(slot["record"]) is str) and
            slot["identity_sha256"] is None,
            "failed campaign slot lacks retained diagnostic evidence")
    if slot["record_fields"] is not None:
        require(type(slot["record_fields"]) is dict and
                list(slot["record_fields"]) == ROUTE_FIELDS and
                all(type(value) is str for value in slot["record_fields"].values()) and
                type(slot["record"]) is str and
                strict_json_equal(parse_route_record(slot["record"]),
                                  slot["record_fields"]),
                "failed campaign slot record fields are not closed raw evidence")
    return SlotResult(
        index, expected_route, expected_sample_ordinal, "fail", slot["elapsed_ms"],
        slot["record"], slot["record_fields"], None, "retained", slot["failure"],
    )


def rebuild_slots(
    slots: Any,
    expected_order: Sequence[str],
    max_batch_workers: int,
    max_local_threads: str,
    *,
    allow_failure: bool,
) -> list[SlotResult]:
    require(type(slots) is list and len(slots) <= len(expected_order),
            "campaign slot collection is invalid")
    observed_samples = {"legacy": 0, "structured": 0}
    rebuilt: list[SlotResult] = []
    for index, slot in enumerate(slots):
        route = expected_order[index]
        observed_samples[route] += 1
        rebuilt.append(validate_slot_object(
            slot, index, route, observed_samples[route], max_batch_workers,
            max_local_threads, allow_failure=allow_failure,
        ))
    return rebuilt


def validate_campaign_artifact(artifact: dict[str, Any]) -> None:
    require(type(artifact) is dict and set(artifact) == ROOT_KEYS,
            "campaign artifact root is not closed")
    require(artifact["artifact_format"] == ARTIFACT_FORMAT and
            type(artifact["artifact_format_version"]) is int and
            artifact["artifact_format_version"] == 1 and
            artifact["record_schema"] == SUMMARY_PREFIX and
            artifact["record_schema_variant"] == ARTIFACT_VARIANT and
            artifact["status"] == "pass",
            "campaign artifact schema identity mismatch")
    require(artifact["claim_boundary"] == CLAIM_BOUNDARY and artifact["promotion"] is False and
            artifact["failure"] is None,
            "campaign artifact crossed its pass-only claim boundary")
    source = validate_source_object(artifact["source"])
    max_batch_workers, max_local_threads, timeout_s = validate_parameter_object(
        artifact["parameters"]
    )
    samples, expected_order = validate_schedule_object(artifact["schedule"])
    require(artifact["schedule"]["slots_completed"] == len(expected_order),
            "pass artifact lacks a complete schedule")
    slots = rebuild_slots(
        artifact["slots"], expected_order, max_batch_workers, max_local_threads,
        allow_failure=False,
    )
    require(len(slots) == len(expected_order), "pass artifact lacks slots")

    rebuilt_identity = identity_object(slots)
    rebuilt_stability = route_stability(slots)
    rebuilt_budget = directional_budget(slots, rebuilt_stability)
    require(strict_json_equal(artifact["identity"], rebuilt_identity),
            "campaign identity object does not match slot evidence")
    require(strict_json_equal(artifact["route_stability"], rebuilt_stability),
            "route stability object does not match slot evidence")
    require(strict_json_equal(artifact["directional_budget"], rebuilt_budget),
            "directional budget does not match slot evidence")
    require(rebuilt_stability["legacy_stable"] and
            rebuilt_stability["structured_stable"] and
            not rebuilt_stability["mismatches"],
            "pass artifact lacks stable routes")
    require(rebuilt_budget["status"] == "pass", "pass artifact lacks a passing budget")

    expected_summary = summary_record(
        status="pass",
        order=expected_order,
        slots_completed=len(slots),
        samples_per_route=samples,
        identity_sha256=rebuilt_identity["sha256"],
        source=source,
        max_batch_workers=max_batch_workers,
        max_local_threads=max_local_threads,
        timeout_s=timeout_s,
        stability=rebuilt_stability,
        budget=rebuilt_budget,
        artifact_published=True,
    )
    require(artifact["summary_record"] == expected_summary,
            "campaign summary does not match rebuilt slot evidence")
    parse_summary_record(artifact["summary_record"])


def validate_failure_diagnostic(diagnostic: dict[str, Any]) -> None:
    require(type(diagnostic) is dict and set(diagnostic) == DIAGNOSTIC_ROOT_KEYS,
            "campaign failure diagnostic root is not closed")
    require(diagnostic["artifact_format"] == DIAGNOSTIC_FORMAT and
            type(diagnostic["artifact_format_version"]) is int and
            diagnostic["artifact_format_version"] == 1 and
            diagnostic["record_schema"] == SUMMARY_PREFIX and
            diagnostic["record_schema_variant"] == ARTIFACT_VARIANT and
            diagnostic["status"] == "fail" and
            diagnostic["claim_boundary"] == CLAIM_BOUNDARY and
            diagnostic["promotion"] is False and
            diagnostic["artifact_published"] is False,
            "campaign failure diagnostic identity is invalid")
    source = validate_source_object(diagnostic["source"])
    max_batch_workers, max_local_threads, timeout_s = validate_parameter_object(
        diagnostic["parameters"]
    )
    samples, expected_order = validate_schedule_object(diagnostic["schedule"])
    require(diagnostic["schedule"]["slots_completed"] == len(diagnostic["slots"]),
            "failure diagnostic completed slot count mismatch")
    slots = rebuild_slots(
        diagnostic["slots"], expected_order, max_batch_workers, max_local_threads,
        allow_failure=True,
    )

    identity = diagnostic["identity"]
    if identity is not None:
        require(all(slot.status == "pass" for slot in slots) and
                strict_json_equal(identity, identity_object(slots)),
                "failure diagnostic identity does not match slot evidence")
    stability = diagnostic["route_stability"]
    if stability is not None:
        require(len(slots) == len(expected_order) and
                all(slot.status == "pass" for slot in slots) and
                strict_json_equal(stability, route_stability(slots)),
                "failure diagnostic stability does not match slot evidence")
    budget = diagnostic["directional_budget"]
    if budget is not None:
        require(stability is not None and
                strict_json_equal(budget, directional_budget(slots, stability)),
                "failure diagnostic budget does not match slot evidence")
    failure = diagnostic["failure"]
    require(type(failure) is dict and set(failure) == FAILURE_KEYS and
            type(failure["stage"]) is str and failure["stage"] != "" and
            (failure["slot_ordinal"] is None or
             (type(failure["slot_ordinal"]) is int and
              0 <= failure["slot_ordinal"] < len(expected_order))) and
            type(failure["code"]) is str and failure["code"] != "" and
            type(failure["message"]) is str and failure["message"] != "" and
            type(failure["diagnostic_directory"]) is str and
            failure["diagnostic_directory"] != "",
            "campaign failure object is invalid")
    failed_slots = [slot for slot in slots if slot.status == "fail"]
    require(len(failed_slots) <= 1 and
            (not failed_slots or slots[-1].status == "fail"),
            "campaign failure diagnostic violates fail-fast slot ordering")
    failure_ordinal = failure["slot_ordinal"]
    if failed_slots:
        require(failure_ordinal == failed_slots[0].ordinal,
                "campaign failure ordinal does not identify its failed slot")
    else:
        require((len(slots) == len(expected_order) and failure_ordinal is None) or
                (len(slots) < len(expected_order) and failure_ordinal == len(slots)),
                "campaign failure ordinal does not identify the next slot")
    expected_summary = summary_record(
        status="fail",
        order=expected_order,
        slots_completed=len(slots),
        samples_per_route=samples,
        identity_sha256=identity["sha256"] if identity is not None else None,
        source=source,
        max_batch_workers=max_batch_workers,
        max_local_threads=max_local_threads,
        timeout_s=timeout_s,
        stability=stability,
        budget=budget,
        artifact_published=False,
    )
    require(diagnostic["summary_record"] == expected_summary,
            "failure summary does not match diagnostic evidence")
    parse_summary_record(diagnostic["summary_record"])


def write_validated_json(
    destination: Path,
    document: dict[str, Any],
    validator: Any,
    description: str,
) -> None:
    validator(document)
    temporary_name: str | None = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
            delete=False,
        ) as handle:
            temporary_name = handle.name
            json.dump(document, handle, ensure_ascii=True, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        loaded = json.loads(Path(temporary_name).read_text(encoding="utf-8"))
        validator(loaded)
        os.replace(temporary_name, destination)
        temporary_name = None
    except CampaignError:
        raise
    except (OSError, json.JSONDecodeError, TypeError, ValueError) as error:
        raise CampaignError(f"cannot publish {description}: {error}",
                            stage="artifact", code="publication_failure") from error
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except FileNotFoundError:
                pass
            except OSError as error:
                raise CampaignError(f"cannot clean temporary {description}: {error}",
                                    stage="artifact", code="temporary_cleanup_failure") from error


def write_atomic_json(destination: Path, artifact: dict[str, Any]) -> None:
    write_validated_json(
        destination, artifact, validate_campaign_artifact, "campaign artifact"
    )


def write_atomic_diagnostic(destination: Path, diagnostic: dict[str, Any]) -> None:
    write_validated_json(
        destination, diagnostic, validate_failure_diagnostic, "campaign failure diagnostic"
    )


def invalidate_artifact(destination: Path) -> None:
    try:
        destination.unlink()
    except FileNotFoundError:
        pass
    except OSError as error:
        raise CampaignError(f"cannot invalidate canonical campaign artifact: {error}",
                            stage="artifact", code="invalidation_failure") from error


def load_campaign_artifact(
    artifact_path: Path,
    expected_summary: str,
) -> dict[str, Any]:
    def closed_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            require(key not in result, f"campaign artifact repeats JSON key {key}")
            result[key] = value
        return result

    try:
        metadata = os.lstat(artifact_path)
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1,
                "canonical campaign artifact is not a single-link regular file")
        require(0 < metadata.st_size <= 16 * 1024 * 1024,
                "canonical campaign artifact size is invalid")
        payload = artifact_path.read_bytes()
        require(len(payload) == metadata.st_size and payload.endswith(b"\n") and
                b"\x00" not in payload and payload.isascii(),
                "canonical campaign artifact byte framing is invalid")
        loaded = json.loads(payload.decode("ascii"), object_pairs_hook=closed_object)
    except CampaignError:
        raise
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read canonical campaign artifact: {error}",
            stage="artifact",
            code="validation_failure",
        ) from error
    require(isinstance(loaded, dict), "canonical campaign artifact root is not an object")
    validate_campaign_artifact(loaded)
    require(loaded["summary_record"] == expected_summary,
            "canonical campaign artifact differs from the outer summary")
    return loaded


def identity_object(slots: Sequence[SlotResult]) -> dict[str, Any]:
    require(slots and slots[0].record_fields is not None, "campaign has no identity source")
    reference = slots[0].record_fields
    assert reference is not None
    digest = canonical_digest(reference, IDENTITY_FIELDS)
    for slot in slots:
        require(slot.record_fields is not None, "campaign slot lacks parsed fields")
        assert slot.record_fields is not None
        for field in IDENTITY_FIELDS:
            require(slot.record_fields[field] == reference[field],
                    f"51-field identity drift at slot {slot.ordinal}: {field}")
        require(slot.identity_sha256 == digest,
                f"51-field identity digest drift at slot {slot.ordinal}")
    return {
        "field_count": len(IDENTITY_FIELDS),
        "field_order": list(IDENTITY_FIELDS),
        "values": {field: reference[field] for field in IDENTITY_FIELDS},
        "sha256": digest,
    }


def make_artifact(
    *,
    source: SourceIdentity,
    max_batch_workers: int,
    max_local_threads: str,
    timeout_s: int,
    samples_per_route: int,
    order: Sequence[str],
    started_utc: str,
    completed_utc: str,
    slots: Sequence[SlotResult],
    identity: dict[str, Any],
    stability: dict[str, Any],
    budget: dict[str, Any],
) -> dict[str, Any]:
    summary = summary_record(
        status="pass",
        order=order,
        slots_completed=len(slots),
        samples_per_route=samples_per_route,
        identity_sha256=identity["sha256"] if identity is not None else None,
        source=source,
        max_batch_workers=max_batch_workers,
        max_local_threads=max_local_threads,
        timeout_s=timeout_s,
        stability=stability,
        budget=budget,
        artifact_published=True,
    )
    return {
        "artifact_format": ARTIFACT_FORMAT,
        "artifact_format_version": 1,
        "record_schema": SUMMARY_PREFIX,
        "record_schema_variant": ARTIFACT_VARIANT,
        "status": "pass",
        "claim_boundary": CLAIM_BOUNDARY,
        "promotion": False,
        "source": {
            "commit": source.commit,
            "tree": source.tree,
            "binary_sha256": source.binary_sha256,
            "build_type": "Release",
            "host_system": platform.system().lower() or "unknown",
            "host_machine": platform.machine().lower() or "unknown",
        },
        "parameters": {
            "n": PROBE_N,
            "max_special_q": MAX_SPECIAL_Q,
            "max_special_q_batch_workers": max_batch_workers,
            "max_local_sieve_threads": max_local_threads,
            "per_slot_timeout_s": timeout_s,
        },
        "schedule": {
            "policy": "abba_baab_prefix_v1",
            "samples_per_route": samples_per_route,
            "slots_planned": len(order),
            "slots_completed": len(slots),
            "order": list(order),
            "started_utc": started_utc,
            "completed_utc": completed_utc,
        },
        "identity": identity,
        "slots": [slot.to_json() for slot in slots],
        "route_stability": stability,
        "directional_budget": budget,
        "failure": None,
        "summary_record": summary,
    }


def make_failure_diagnostic(
    *,
    source: SourceIdentity,
    max_batch_workers: int,
    max_local_threads: str,
    timeout_s: int,
    samples_per_route: int,
    order: Sequence[str],
    started_utc: str,
    completed_utc: str,
    slots: Sequence[SlotResult],
    identity: dict[str, Any] | None,
    stability: dict[str, Any] | None,
    budget: dict[str, Any] | None,
    failure: dict[str, Any],
) -> dict[str, Any]:
    summary = summary_record(
        status="fail",
        order=order,
        slots_completed=len(slots),
        samples_per_route=samples_per_route,
        identity_sha256=identity["sha256"] if identity is not None else None,
        source=source,
        max_batch_workers=max_batch_workers,
        max_local_threads=max_local_threads,
        timeout_s=timeout_s,
        stability=stability,
        budget=budget,
        artifact_published=False,
    )
    return {
        "artifact_format": DIAGNOSTIC_FORMAT,
        "artifact_format_version": 1,
        "record_schema": SUMMARY_PREFIX,
        "record_schema_variant": ARTIFACT_VARIANT,
        "status": "fail",
        "claim_boundary": CLAIM_BOUNDARY,
        "promotion": False,
        "artifact_published": False,
        "source": {
            "commit": source.commit,
            "tree": source.tree,
            "binary_sha256": source.binary_sha256,
            "build_type": "Release",
            "host_system": platform.system().lower() or "unknown",
            "host_machine": platform.machine().lower() or "unknown",
        },
        "parameters": {
            "n": PROBE_N,
            "max_special_q": MAX_SPECIAL_Q,
            "max_special_q_batch_workers": max_batch_workers,
            "max_local_sieve_threads": max_local_threads,
            "per_slot_timeout_s": timeout_s,
        },
        "schedule": {
            "policy": "abba_baab_prefix_v1",
            "samples_per_route": samples_per_route,
            "slots_planned": len(order),
            "slots_completed": len(slots),
            "order": list(order),
            "started_utc": started_utc,
            "completed_utc": completed_utc,
        },
        "identity": identity,
        "slots": [slot.to_json() for slot in slots],
        "route_stability": stability,
        "directional_budget": budget,
        "failure": failure,
        "summary_record": summary,
    }


def failure_object(error: CampaignError, diagnostic_directory: Path | None) -> dict[str, Any]:
    return {
        "stage": error.stage,
        "slot_ordinal": error.slot_ordinal,
        "code": error.code,
        "message": str(error),
        "diagnostic_directory": str(diagnostic_directory) if diagnostic_directory else None,
    }


def execute_campaign(arguments: argparse.Namespace) -> int:
    project_root = arguments.project_root.resolve()
    executable = arguments.executable.resolve()
    artifact_path = lexical_absolute(arguments.artifact)
    require(project_root.is_dir(), "project root is not a directory")
    require(executable.is_file(), "probe executable does not exist")
    require(os.access(executable, os.X_OK), "probe executable is not executable")
    invalidate_artifact(artifact_path)
    require(os.name == "posix" and hasattr(os, "killpg"),
            "50-digit route campaign execution is POSIX-only")
    verify_release_build(executable)

    order = campaign_order(arguments.samples_per_route)
    source = capture_source_identity(project_root, executable)
    artifact_path.parent.mkdir(parents=True, exist_ok=True)
    run_directory = Path(tempfile.mkdtemp(
        prefix=".complete_first_round_abba_v1.run.", dir=artifact_path.parent
    ))
    started_utc = utc_now()
    slots: list[SlotResult] = []
    sample_counts = {"legacy": 0, "structured": 0}
    identity: dict[str, Any] | None = None
    stability: dict[str, Any] | None = None
    budget: dict[str, Any] | None = None
    active_slot_ordinal: int | None = 0

    try:
        for ordinal, route in enumerate(order):
            active_slot_ordinal = ordinal
            verify_source_identity(project_root, executable, source, slot=ordinal)
            sample_counts[route] += 1
            sample_ordinal = sample_counts[route]
            slot_directory = run_directory / f"slot-{ordinal:02d}-{route}-{sample_ordinal}"
            ooc_directory = slot_directory / "ooc"
            try:
                ooc_directory.mkdir(parents=True)
                stdout_path = slot_directory / "stdout.bin"
                stderr_path = slot_directory / "stderr.bin"
                raw_base = ooc_directory / "raw"
                command = build_probe_command(
                    executable,
                    route,
                    arguments.max_batch_workers,
                    arguments.max_local_threads,
                    raw_base,
                )
                label = f"50d campaign slot={ordinal} route={route} sample={sample_ordinal}"
                print(f"{label}: start", file=sys.stderr, flush=True)
            except (CampaignError, OSError) as setup_error:
                raise CampaignError(
                    f"cannot prepare campaign slot {ordinal}: {setup_error}",
                    stage="slot_setup",
                    code="slot_setup_failure",
                    slot_ordinal=ordinal,
                ) from setup_error
            process_started = time.monotonic()
            try:
                return_code, elapsed_ms, timed_out = run_process(
                    command,
                    project_root,
                    stdout_path,
                    stderr_path,
                    arguments.timeout_s,
                    label,
                )
            except CampaignError as process_error:
                elapsed_ms = max(0, int((time.monotonic() - process_started) * 1000))
                record: str | None = None
                try:
                    record = extract_single_record(stdout_path, stderr_path)
                except (CampaignError, OSError):
                    pass
                slots.append(SlotResult(
                    ordinal, route, sample_ordinal, "fail", elapsed_ms, record, None, None,
                    "retained", str(process_error),
                ))
                raise CampaignError(
                    str(process_error),
                    stage=process_error.stage,
                    code=process_error.code,
                    slot_ordinal=ordinal,
                ) from process_error
            if timed_out:
                slot = SlotResult(
                    ordinal, route, sample_ordinal, "fail", elapsed_ms, None, None, None,
                    "retained", "timeout",
                )
                slots.append(slot)
                raise CampaignError(f"{label} timed out after {arguments.timeout_s}s",
                                    stage="process", code="timeout", slot_ordinal=ordinal)
            if return_code != 0:
                record: str | None = None
                try:
                    record = extract_single_record(stdout_path, stderr_path)
                except CampaignError:
                    pass
                slot = SlotResult(
                    ordinal, route, sample_ordinal, "fail", elapsed_ms, record, None, None,
                    "retained", f"exit_code={return_code}",
                )
                slots.append(slot)
                raise CampaignError(f"{label} exited with code {return_code}",
                                    stage="process", code="nonzero_exit", slot_ordinal=ordinal)

            record: str | None = None
            fields: dict[str, str] | None = None
            try:
                record = extract_single_record(stdout_path, stderr_path)
                fields = parse_route_record(record)
                validate_route_contract(
                    fields, route, arguments.max_batch_workers, arguments.max_local_threads
                )
                raw_data = Path(str(raw_base) + ".reldata")
                raw_index = Path(str(raw_base) + ".relidx")
                require(not raw_data.exists() and not raw_data.is_symlink() and
                        not raw_index.exists() and not raw_index.is_symlink(),
                        "successful route retained its raw OOC pair")
                cleanup_successful_probe_directory(ooc_directory, route)
            except (CampaignError, ProbeCleanupError, OSError) as error:
                message = str(error)
                slot = SlotResult(
                    ordinal, route, sample_ordinal, "fail", elapsed_ms,
                    record, fields, None,
                    "retained", message,
                )
                slots.append(slot)
                raise CampaignError(message, stage="route_contract", code="contract_failure",
                                    slot_ordinal=ordinal) from error

            assert fields is not None
            assert record is not None
            digest = canonical_digest(fields, IDENTITY_FIELDS)
            slots.append(SlotResult(
                ordinal, route, sample_ordinal, "pass", elapsed_ms, record, fields, digest,
                "cleaned", None,
            ))
            # A failure after committing this pass belongs to the next slot
            # preflight, unless the schedule is already complete.
            active_slot_ordinal = len(slots) if len(slots) < len(order) else None
            print(f"{label}: pass ({elapsed_ms}ms)", file=sys.stderr, flush=True)

        active_slot_ordinal = None
        verify_source_identity(project_root, executable, source, slot=None)
        identity = identity_object(slots)
        stability = route_stability(slots)
        if stability["mismatches"]:
            raise CampaignError(
                f"route stability drifted in {len(stability['mismatches'])} field comparison(s)",
                stage="stability", code="route_stability_drift",
            )
        budget = directional_budget(slots, stability)
        if budget["status"] != "pass":
            raise CampaignError(
                f"directional budget is {budget['status']}",
                stage="budget", code="directional_budget_failure",
            )

        artifact = make_artifact(
            source=source,
            max_batch_workers=arguments.max_batch_workers,
            max_local_threads=arguments.max_local_threads,
            timeout_s=arguments.timeout_s,
            samples_per_route=arguments.samples_per_route,
            order=order,
            started_utc=started_utc,
            completed_utc=utc_now(),
            slots=slots,
            identity=identity,
            stability=stability,
            budget=budget,
        )
        validate_campaign_artifact(artifact)
        try:
            shutil.rmtree(run_directory)
        except OSError as error:
            raise CampaignError(
                f"cannot clean successful campaign staging directory: {error}",
                stage="cleanup",
                code="staging_cleanup_failure",
            ) from error
        require(not run_directory.exists(),
                "successful campaign staging directory survived cleanup")
        write_atomic_json(artifact_path, artifact)
        print(artifact["summary_record"])
        return 0
    except (CampaignError, OSError, KeyboardInterrupt) as caught:
        if isinstance(caught, CampaignError):
            error = caught
            if (error.slot_ordinal is None and active_slot_ordinal is not None and
                    len(slots) < len(order)):
                error = CampaignError(
                    str(error),
                    stage=error.stage,
                    code=error.code,
                    slot_ordinal=active_slot_ordinal,
                )
        elif isinstance(caught, KeyboardInterrupt):
            error = CampaignError(
                "campaign interrupted by SIGINT",
                stage="process",
                code="interrupted",
                slot_ordinal=active_slot_ordinal,
            )
        else:
            error = CampaignError(
                f"campaign filesystem/process operation failed: {caught}",
                stage="campaign",
                code="os_error",
                slot_ordinal=active_slot_ordinal,
            )
        try:
            invalidate_artifact(artifact_path)
        except CampaignError as invalidation_error:
            print(f"campaign canonical invalidation failed: {invalidation_error}",
                  file=sys.stderr)
            return 1
        try:
            if identity is None and slots and all(slot.record_fields is not None for slot in slots):
                identity = identity_object(slots)
        except CampaignError:
            identity = None
        diagnostic = make_failure_diagnostic(
            source=source,
            max_batch_workers=arguments.max_batch_workers,
            max_local_threads=arguments.max_local_threads,
            timeout_s=arguments.timeout_s,
            samples_per_route=arguments.samples_per_route,
            order=order,
            started_utc=started_utc,
            completed_utc=utc_now(),
            slots=slots,
            identity=identity,
            stability=stability,
            budget=budget,
            failure=failure_object(error, run_directory),
        )
        try:
            run_directory.mkdir(parents=True, exist_ok=True)
            write_atomic_diagnostic(run_directory / "diagnostic.json", diagnostic)
        except CampaignError as publication_error:
            print(f"campaign diagnostic publication failed: {publication_error}", file=sys.stderr)
        except OSError as publication_error:
            print(f"campaign diagnostic publication failed: {publication_error}", file=sys.stderr)
        print(diagnostic["summary_record"])
        print(f"campaign failed: {error}; diagnostics retained at {run_directory}",
              file=sys.stderr)
        return 1


def replace_field(record: str, key: str, value: str) -> str:
    tokens = record.split(" ")
    matches = [index for index, token in enumerate(tokens) if token.startswith(f"{key}=")]
    require(len(matches) == 1, f"self-test cannot uniquely replace {key}")
    tokens[matches[0]] = f"{key}={value}"
    return " ".join(tokens)


def prepare_campaign_fixture(record: str, route: str) -> str:
    record = record.replace("GNFS_EXPERIMENT_FIXTURE_V2", RECORD_PREFIX, 1)
    replacements = {
        "max_special_q": str(MAX_SPECIAL_Q),
        "special_q_processed": "4",
        "special_q_batch_worker_limit": "4",
        "special_q_batch_peak_workers": "4",
        "special_q_batch_count": "1",
        "special_q_batch_peak_size": "4",
        "local_sieve_thread_budget": "4",
        "special_q_batch_peak_assigned_threads": "4",
        "special_q_worker_peak_sieve_threads": "1",
        "candidates_total": "8",
        "candidate_batch_peak_workers": "2",
        "candidate_batch_total_chunks": "2",
        "candidate_batch_peak_chunks": "2",
        "candidate_batch_peak_candidates": "8",
        "candidate_batch_rss_sample_candidates": "8",
        "rational_fb_columns": "3",
        "algebraic_fb_columns": "2",
        "base_factor_columns": "5",
        "initial_raw_target": "50",
        "first_round_complete": "true",
        "sieve_stop_reason": "effective_column_excess",
        "raw_rows": "100",
        "raw_duplicates": "0",
        "input_lp_columns": "10",
        "input_lp_w1": "2",
        "input_lp_w2": "3",
        "input_lp_w3": "1",
        "input_lp_w4plus": "4",
        "wall_ms": "100" if route == "legacy" else "110",
        "output_rows": "10" if route == "legacy" else "20",
        "output_lp_columns": "4" if route == "legacy" else "8",
        "output_digest_low": "101" if route == "legacy" else "201",
        "output_digest_high": "102" if route == "legacy" else "202",
        "matrix_rows": "10" if route == "legacy" else "20",
        "matrix_cols": "12" if route == "legacy" else "10",
        "matrix_nonzeros": "100" if route == "legacy" else "2900",
        "matrix_signed_delta": "-2" if route == "legacy" else "10",
        "process_rss_backend": "darwin_getrusage",
        "process_current_rss_supported": "true",
        "process_peak_rss_supported": "true",
        "candidate_generation_s": "0.25",
        "candidate_cofactor_s": "0.5",
    }
    if route == "structured":
        replacements.update({
            "structured_commits": "1",
            "structured_emitted_rows": "21",
            "structured_stop": "budget_limit",
            "incidence_shards": "2",
            "incidence_requested_workers": "4",
            "incidence_peak_workers": "2",
            "structured_filter_records": "1",
            "structured_matrix_records": "1",
            "output_pair_observed": "true",
            "output_pair_retained_by_matrix": "true",
            "output_pair_removed": "true",
            "output_lease_removed": "true",
        })
    rss_value = "100" if route == "legacy" else "150"
    for key in CURRENT_RSS_FIELDS + PEAK_RSS_FIELDS + CANDIDATE_RSS_FIELDS:
        replacements[key] = rss_value
    for key, value in replacements.items():
        record = replace_field(record, key, value)
    return record


def expect_rejected(callable_: Any, description: str) -> None:
    try:
        callable_()
    except CampaignError:
        return
    raise CampaignError(f"self-test accepted {description}")


def self_test_process_lifecycle(root: Path) -> None:
    child_stdout = root / "child-stdout.bin"
    child_stderr = root / "child-stderr.bin"
    exit_code, _, timed_out = run_process(
        [sys.executable, "-c", "raise SystemExit(7)"],
        root,
        child_stdout,
        child_stderr,
        2,
        "campaign self-test nonzero child",
    )
    require(exit_code == 7 and not timed_out, "nonzero child status was not preserved")
    exit_code, _, timed_out = run_process(
        [sys.executable, "-c", "import time; time.sleep(2)"],
        root,
        child_stdout,
        child_stderr,
        1,
        "campaign self-test timeout child",
    )
    require(timed_out and exit_code != 0, "timeout child group was not terminated")

    original_signal_handlers = {
        signal_number: signal.getsignal(signal_number)
        for signal_number in campaign_termination_signals()
    }
    for injected_signal in campaign_termination_signals():
        spawned_process: subprocess.Popen[Any] | None = None

        def interrupting_popen(*popen_arguments: Any, **popen_keywords: Any) -> Any:
            nonlocal spawned_process
            spawned_process = subprocess.Popen(*popen_arguments, **popen_keywords)
            os.kill(os.getpid(), injected_signal)
            return spawned_process

        try:
            try:
                run_process(
                    [sys.executable, "-c", "import time; time.sleep(30)"],
                    root,
                    child_stdout,
                    child_stderr,
                    10,
                    f"campaign self-test deferred signal {injected_signal}",
                    popen_factory=interrupting_popen,
                )
            except CampaignError as error:
                require(error.stage == "process" and error.code == "interrupted",
                        f"signal {injected_signal} did not map to process/interrupted")
            else:
                raise CampaignError(
                    f"self-test accepted deferred signal {injected_signal}"
                )
        finally:
            if spawned_process is not None and process_group_exists(spawned_process.pid):
                terminate_process(
                    spawned_process,
                    f"self-test deferred signal {injected_signal} cleanup",
                )
        require(spawned_process is not None,
                f"signal {injected_signal} fixture did not spawn its child")
        require(spawned_process.poll() is not None and
                not process_group_exists(spawned_process.pid),
                f"signal {injected_signal} left its process group running")
        for signal_number, original_handler in original_signal_handlers.items():
            require(signal.getsignal(signal_number) == original_handler,
                    f"signal {signal_number} handler was not restored exactly")

    kill_fallback_marker = root / "kill-fallback.ready"
    kill_fallback_code = (
        "import pathlib,signal,sys,time;"
        "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
        "pathlib.Path(sys.argv[1]).write_text('ready',encoding='ascii');"
        "time.sleep(30)"
    )
    kill_fallback_child = subprocess.Popen(
        [sys.executable, "-c", kill_fallback_code, str(kill_fallback_marker)],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        kill_fallback_deadline = time.monotonic() + 2
        while (not kill_fallback_marker.exists() and
               time.monotonic() < kill_fallback_deadline):
            time.sleep(0.01)
        require(kill_fallback_marker.is_file(), "SIGKILL fallback child was not ready")
        terminate_process(
            kill_fallback_child,
            "campaign self-test SIGKILL fallback child",
            grace_s=0.05,
        )
    finally:
        if process_group_exists(kill_fallback_child.pid):
            terminate_process(
                kill_fallback_child,
                "campaign self-test SIGKILL fallback cleanup",
                grace_s=0.05,
            )
    require(kill_fallback_child.poll() is not None and
            not process_group_exists(kill_fallback_child.pid),
            "SIGTERM-ignoring child survived SIGKILL fallback")

    leaked_descendant_path = root / "leaked-descendant.pid"
    leaked_leader_code = (
        "import os,pathlib,subprocess,sys;"
        "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']);"
        "pathlib.Path(sys.argv[1]).write_text(f'{os.getpid()} {child.pid}',encoding='ascii')"
    )
    try:
        run_process(
            [sys.executable, "-c", leaked_leader_code, str(leaked_descendant_path)],
            root,
            child_stdout,
            child_stderr,
            5,
            "campaign self-test exited leader with descendant",
        )
    except CampaignError as error:
        require(error.stage == "process" and error.code == "descendant_process_leak",
                "exited leader did not report descendant_process_leak")
    else:
        raise CampaignError("self-test accepted an exited leader with a live descendant")
    require(leaked_descendant_path.is_file(),
            "exited-leader descendant PID fixture was not published")
    leaked_group, leaked_child = (
        int(value) for value in leaked_descendant_path.read_text(encoding="ascii").split()
    )
    require(not process_group_exists(leaked_group),
            "exited leader left its descendant process group running")
    try:
        os.kill(leaked_child, 0)
    except ProcessLookupError:
        pass
    else:
        raise CampaignError("exited leader left its descendant process running")

    for exception, description in (
        (KeyboardInterrupt(), "KeyboardInterrupt"),
        (RuntimeError("synthetic poll failure"), "poll exception"),
    ):
        def interrupt_wait(_delay: float, raised: BaseException = exception) -> None:
            raise raised

        interrupted_child = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            cwd=root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            try:
                wait_for_process(
                    interrupted_child,
                    time.monotonic(),
                    10,
                    f"campaign self-test {description}",
                    interrupt_wait,
                )
            except CampaignError as error:
                require(error.stage == "process" and error.code == "interrupted",
                        f"{description} did not map to process/interrupted")
            else:
                raise CampaignError(f"self-test accepted {description}")
        finally:
            if process_group_exists(interrupted_child.pid):
                terminate_process(interrupted_child, f"self-test {description} cleanup")
        require(interrupted_child.poll() is not None and
                not process_group_exists(interrupted_child.pid),
                f"{description} left its process group running")

    grandchild_pid_path = root / "grandchild.pid"
    leader_code = (
        "import pathlib,subprocess,sys,time;"
        "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']);"
        "pathlib.Path(sys.argv[1]).write_text(str(child.pid),encoding='ascii');"
        "time.sleep(30)"
    )
    leader = subprocess.Popen(
        [sys.executable, "-c", leader_code, str(grandchild_pid_path)],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    try:
        deadline = time.monotonic() + 2
        while (not grandchild_pid_path.exists() or
               grandchild_pid_path.stat().st_size == 0) and time.monotonic() < deadline:
            time.sleep(0.01)
        require(grandchild_pid_path.is_file() and grandchild_pid_path.stat().st_size > 0,
                "grandchild PID fixture was not published")
        grandchild_pid = int(grandchild_pid_path.read_text(encoding="ascii"))

        def interrupt_grandchild_wait(_delay: float) -> None:
            raise KeyboardInterrupt()

        try:
            wait_for_process(
                leader,
                time.monotonic(),
                10,
                "campaign self-test descendant group",
                interrupt_grandchild_wait,
            )
        except CampaignError as error:
            require(error.stage == "process" and error.code == "interrupted",
                    "descendant-group interruption status mismatch")
        else:
            raise CampaignError("descendant-group interruption was accepted")
        require(not process_group_exists(leader.pid),
                "interrupted descendant process group survived cleanup")
        try:
            os.kill(grandchild_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise CampaignError("interrupted grandchild process survived group cleanup")
    finally:
        if process_group_exists(leader.pid):
            terminate_process(leader, "self-test descendant group cleanup")


def self_test(arguments: argparse.Namespace) -> int:
    structured = prepare_campaign_fixture(arguments.structured_record, "structured")
    legacy = prepare_campaign_fixture(arguments.legacy_record, "legacy")
    structured_fields = parse_route_record(structured)
    legacy_fields = parse_route_record(legacy)
    validate_route_contract(structured_fields, "structured", 4, "auto")
    validate_route_contract(legacy_fields, "legacy", 4, "auto")
    require(int(structured_fields["structured_emitted_rows"]) >
            int(structured_fields["output_rows"]),
            "self-test lacks the legal emitted-rows-greater-than-output case")
    explicit_clamped = replace_field(
        structured, "max_local_sieve_threads_requested", "8"
    )
    validate_route_contract(parse_route_record(explicit_clamped), "structured", 4, "8")
    invalid_explicit_budget = explicit_clamped
    for key, value in (
        ("local_sieve_thread_budget", "9"),
        ("special_q_batch_peak_assigned_threads", "9"),
        ("special_q_worker_peak_sieve_threads", "3"),
    ):
        invalid_explicit_budget = replace_field(invalid_explicit_budget, key, value)
    expect_rejected(
        lambda: validate_route_contract(
            parse_route_record(invalid_explicit_budget), "structured", 4, "8"
        ),
        "effective local-thread budget exceeding its explicit request",
    )
    require(len(ROUTE_FIELDS) == 109, "route source schema is not 109 fields")
    require(len(IDENTITY_FIELDS) == 51, "campaign identity schema is not 51 fields")
    require(len(ROUTE_STABILITY_FIELDS) == 13, "route stability schema is not 13 fields")
    require(len(SUMMARY_FIELDS) == 24, "campaign summary schema is not 24 fields")
    schedule_cycle = [
        "legacy", "structured", "structured", "legacy",
        "structured", "legacy", "legacy", "structured",
    ]
    for sample_count in range(2, 10):
        schedule = campaign_order(sample_count)
        expected_schedule = [
            schedule_cycle[index % len(schedule_cycle)]
            for index in range(sample_count * 2)
        ]
        require(len(schedule) == sample_count * 2 and
                schedule.count("legacy") == sample_count and
                schedule.count("structured") == sample_count and
                schedule == expected_schedule,
                f"N={sample_count} ABBA/BAAB prefix schedule mismatch")
    expect_rejected(lambda: campaign_order(1), "samples_per_route below 2")
    expect_rejected(lambda: campaign_order(10), "samples_per_route above 9")
    auto_command = build_probe_command(
        Path("probe"), "structured", 4, "auto", Path("slot/ooc/raw")
    )
    require(auto_command.count("--max-special-q-batch-workers") == 1 and
            "--max-local-sieve-threads" not in auto_command,
            "auto probe command option multiplicity mismatch")
    explicit_command = build_probe_command(
        Path("probe"), "legacy", 3, "2", Path("slot/ooc/raw")
    )
    require(explicit_command.count("--max-special-q-batch-workers") == 1 and
            explicit_command.count("--max-local-sieve-threads") == 1 and
            explicit_command[explicit_command.index("--max-local-sieve-threads") + 1] == "2",
            "explicit probe command option multiplicity mismatch")

    route_tokens = structured.split(" ")
    expect_rejected(lambda: parse_route_record(" ".join(route_tokens[:-1])),
                    "a missing route field")
    duplicate_route = route_tokens.copy()
    duplicate_route[-1] = duplicate_route[1]
    expect_rejected(lambda: parse_route_record(" ".join(duplicate_route)),
                    "a duplicate route field")
    unknown_route = route_tokens.copy()
    unknown_route[-1] = "unknown_field=none"
    expect_rejected(lambda: parse_route_record(" ".join(unknown_route)),
                    "an unknown route field")
    reordered_route = route_tokens.copy()
    reordered_route[1], reordered_route[2] = reordered_route[2], reordered_route[1]
    expect_rejected(lambda: parse_route_record(" ".join(reordered_route)),
                    "reordered route fields")
    oversized_float = replace_field(structured, "candidate_generation_s", "1e309")
    expect_rejected(lambda: parse_route_record(oversized_float),
                    "a non-binary64 finite timing")

    def expect_route_mutation_rejected(
        source_record: str,
        route: str,
        replacements: dict[str, str],
        description: str,
    ) -> None:
        mutated = source_record
        for key, value in replacements.items():
            mutated = replace_field(mutated, key, value)
        expect_rejected(
            lambda: validate_route_contract(
                parse_route_record(mutated), route, 4, "auto"
            ),
            description,
        )

    for source_record, route in ((legacy, "legacy"), (structured, "structured")):
        expect_route_mutation_rejected(
            source_record, route, {"candidate_chunk_size": "255"},
            f"cross-route-consistent candidate chunk-size drift ({route})",
        )
        expect_route_mutation_rejected(
            source_record, route, {"raw_duplicates": "1"},
            f"cross-route-consistent raw duplicates ({route})",
        )
        expect_route_mutation_rejected(
            source_record, route, {"input_lp_w4plus": "5"},
            f"cross-route-consistent LP histogram drift ({route})",
        )
        expect_route_mutation_rejected(
            source_record, route, {"special_q_batch_peak_workers": "3"},
            f"cross-route-consistent special-Q topology drift ({route})",
        )
        expect_route_mutation_rejected(
            source_record, route, {"candidate_batch_peak_workers": "1"},
            f"cross-route-consistent candidate topology drift ({route})",
        )
    expect_route_mutation_rejected(
        structured,
        "structured",
        {"structured_stop": "not_started"},
        "structured reducer reported not_started",
    )
    expect_route_mutation_rejected(
        legacy,
        "legacy",
        {"matrix_rows": "11", "matrix_signed_delta": "-1"},
        "legacy matrix rows exceeding reduction output",
    )
    expect_route_mutation_rejected(
        structured,
        "structured",
        {"matrix_rows": "21", "matrix_signed_delta": "11"},
        "structured matrix rows exceeding reduction output",
    )

    source = SourceIdentity("1" * 40, "2" * 40, "3" * 64)
    records = [legacy, structured, structured, legacy]
    slots: list[SlotResult] = []
    route_counts = {"legacy": 0, "structured": 0}
    for ordinal, (route, record) in enumerate(zip(campaign_order(2), records)):
        route_counts[route] += 1
        fields = parse_route_record(record)
        slots.append(SlotResult(
            ordinal, route, route_counts[route], "pass", 1, record, fields,
            canonical_digest(fields, IDENTITY_FIELDS), "cleaned", None,
        ))
    identity = identity_object(slots)
    stability = route_stability(slots)
    budget = directional_budget(slots, stability)
    require(stability["legacy_stable"] and stability["structured_stable"],
            "valid fixture routes are unstable")
    require(budget["status"] == "pass", "valid fixture budget did not pass")
    artifact = make_artifact(
        source=source, max_batch_workers=4, max_local_threads="auto",
        timeout_s=7200, samples_per_route=2, order=campaign_order(2),
        started_utc="2026-01-01T00:00:00Z", completed_utc="2026-01-01T00:00:01Z",
        slots=slots, identity=identity, stability=stability, budget=budget,
    )
    if artifact["source"]["host_system"] not in {"linux", "darwin"}:
        artifact["source"]["host_system"] = "linux"
    validate_campaign_artifact(artifact)

    missing = dict(artifact)
    missing.pop("promotion")
    expect_rejected(lambda: validate_campaign_artifact(missing), "missing root field")
    unknown = dict(artifact)
    unknown["unknown"] = 1
    expect_rejected(lambda: validate_campaign_artifact(unknown), "unknown root field")
    wrong_order = copy.deepcopy(artifact)
    wrong_order["schedule"]["order"][0] = "structured"
    expect_rejected(lambda: validate_campaign_artifact(wrong_order), "wrong route order")
    wrong_ordinal = copy.deepcopy(artifact)
    wrong_ordinal["slots"][1]["route_sample_ordinal"] = 2
    expect_rejected(lambda: validate_campaign_artifact(wrong_ordinal),
                    "wrong route sample ordinal")
    for path, description in (
        (("artifact_format_version",), "boolean root format version"),
        (("schedule", "slots_planned"), "boolean planned-slot count"),
        (("schedule", "slots_completed"), "boolean completed-slot count"),
        (("slots", 0, "ordinal"), "boolean slot ordinal"),
        (("slots", 0, "route_sample_ordinal"), "boolean route sample ordinal"),
        (("slots", 0, "elapsed_ms"), "boolean slot elapsed time"),
        (("route_stability", "legacy_stable"), "integer route-stability boolean"),
        (("directional_budget", "wall_pass"), "integer budget boolean"),
    ):
        mutated_type = copy.deepcopy(artifact)
        target: Any = mutated_type
        for component in path[:-1]:
            target = target[component]
        original = target[path[-1]]
        target[path[-1]] = (True if type(original) is int else 1)
        if path[-1] == "ordinal":
            target[path[-1]] = False
        expect_rejected(
            lambda document=mutated_type: validate_campaign_artifact(document),
            description,
        )
    unsupported_host = copy.deepcopy(artifact)
    unsupported_host["source"]["host_system"] = "windows"
    expect_rejected(lambda: validate_campaign_artifact(unsupported_host),
                    "unsupported campaign host provenance")
    malformed_timestamp = copy.deepcopy(artifact)
    malformed_timestamp["schedule"]["started_utc"] = "garbageZ"
    expect_rejected(lambda: validate_campaign_artifact(malformed_timestamp),
                    "malformed campaign timestamp")
    reversed_timestamp = copy.deepcopy(artifact)
    reversed_timestamp["schedule"]["started_utc"] = "2026-01-01T00:00:02Z"
    expect_rejected(lambda: validate_campaign_artifact(reversed_timestamp),
                    "reversed campaign timestamps")
    record_field_mismatch = copy.deepcopy(artifact)
    record_field_mismatch["slots"][0]["record"] = replace_field(
        record_field_mismatch["slots"][0]["record"], "wall_ms", "101"
    )
    expect_rejected(lambda: validate_campaign_artifact(record_field_mismatch),
                    "record/record_fields mismatch")
    wrong_digest = copy.deepcopy(artifact)
    wrong_digest["slots"][0]["identity_sha256"] = "0" * 64
    expect_rejected(lambda: validate_campaign_artifact(wrong_digest),
                    "mutated per-slot identity digest")
    wrong_identity = copy.deepcopy(artifact)
    wrong_identity["identity"]["values"]["raw_rows"] = "999"
    expect_rejected(lambda: validate_campaign_artifact(wrong_identity),
                    "mutated rebuilt identity")
    wrong_stability = copy.deepcopy(artifact)
    wrong_stability["route_stability"]["legacy_sha256"] = "0" * 64
    expect_rejected(lambda: validate_campaign_artifact(wrong_stability),
                    "mutated rebuilt route stability")
    wrong_budget = copy.deepcopy(artifact)
    wrong_budget["directional_budget"]["wall_ratio_ppm"] += 1
    expect_rejected(lambda: validate_campaign_artifact(wrong_budget),
                    "mutated rebuilt directional budget")
    wrong_summary = copy.deepcopy(artifact)
    wrong_summary["summary_record"] = replace_field(
        wrong_summary["summary_record"], "wall_ratio_ppm", "1"
    )
    expect_rejected(lambda: validate_campaign_artifact(wrong_summary),
                    "mutated rebuilt summary")
    reordered_summary = artifact["summary_record"].split(" ")
    reordered_summary[1], reordered_summary[2] = reordered_summary[2], reordered_summary[1]
    expect_rejected(lambda: parse_summary_record(" ".join(reordered_summary)),
                    "reordered summary fields")
    pass_claiming_unpublished = replace_field(
        artifact["summary_record"], "artifact_published", "false"
    )
    expect_rejected(lambda: parse_summary_record(pass_claiming_unpublished),
                    "pass summary claiming no canonical artifact")
    for field, value, description in (
        ("slots_completed", "0", "pass summary without all scheduled slots"),
        ("identity_sha256", "na", "pass summary without identity"),
        ("route_stability", "fail", "pass summary without stable routes"),
        ("directional_budget", "not_evaluated", "pass summary without a budget"),
        ("max_local_sieve_threads", "4294967296",
         "pass summary with an oversized local-thread request"),
        ("per_slot_timeout_s", "4294967296",
         "pass summary with an oversized slot timeout"),
    ):
        mutated_summary = replace_field(artifact["summary_record"], field, value)
        expect_rejected(lambda record=mutated_summary: parse_summary_record(record),
                        description)
    for field, value, description in (
        ("wall_ratio_ppm", "1200001", "pass summary above wall budget"),
        ("peak_rss_ratio_ppm", "1600001", "pass summary above RSS budget"),
        ("matrix_nonzeros_ratio_ppm", "30000001",
         "pass summary above matrix-nonzeros budget"),
    ):
        mutated_summary = replace_field(artifact["summary_record"], field, value)
        expect_rejected(lambda record=mutated_summary: parse_summary_record(record),
                        description)

    drifted_slots = [SlotResult(**slot.__dict__) for slot in slots]
    assert drifted_slots[1].record_fields is not None
    drifted_slots[1].record_fields = dict(drifted_slots[1].record_fields)
    drifted_slots[1].record_fields["candidate_batch_rss_sample_candidates"] = "1"
    expect_rejected(lambda: identity_object(drifted_slots), "51-field identity drift")

    unstable_slots = [SlotResult(**slot.__dict__) for slot in slots]
    assert unstable_slots[2].record_fields is not None
    unstable_slots[2].record_fields = dict(unstable_slots[2].record_fields)
    unstable_slots[2].record_fields["matrix_nonzeros"] = "2901"
    unstable = route_stability(unstable_slots)
    require(not unstable["structured_stable"] and len(unstable["mismatches"]) == 1,
            "route stability drift was not isolated")
    require(legacy_fields["output_digest_low"] != structured_fields["output_digest_low"],
            "self-test did not preserve allowed cross-route output differences")

    over_budget_slots = [SlotResult(**slot.__dict__) for slot in slots]
    for slot in over_budget_slots:
        assert slot.record_fields is not None
        slot.record_fields = dict(slot.record_fields)
        if slot.route == "structured":
            slot.record_fields["wall_ms"] = "121"
    require(directional_budget(over_budget_slots, stability)["status"] == "fail",
            "wall budget regression did not fail")

    unsupported_slots = [SlotResult(**slot.__dict__) for slot in slots]
    for slot in unsupported_slots:
        assert slot.record_fields is not None
        slot.record_fields = dict(slot.record_fields)
        slot.record_fields["process_rss_backend"] = "unsupported"
        slot.record_fields["process_peak_rss_supported"] = "false"
        slot.record_fields["process_peak_rss_bytes"] = "na"
    require(directional_budget(unsupported_slots, stability)["status"] == "unavailable",
            "unsupported RSS did not fail closed")

    with tempfile.TemporaryDirectory(prefix="gnfs-50d-campaign-selftest-") as temp_text:
        root = Path(temp_text)
        stdout_path = root / "stdout.bin"
        stderr_path = root / "stderr.bin"
        stdout_path.write_bytes((structured + "\n").encode("ascii"))
        stderr_path.write_bytes(b"")
        require(extract_single_record(stdout_path, stderr_path) == structured,
                "raw-byte record extraction changed the fixture")
        stdout_path.write_bytes(b"")
        expect_rejected(lambda: extract_single_record(stdout_path, stderr_path),
                        "a missing raw-byte record")
        stdout_path.write_bytes((structured + "\n").encode("ascii"))
        stderr_path.write_bytes((structured + "\n").encode("ascii"))
        expect_rejected(lambda: extract_single_record(stdout_path, stderr_path),
                        "multiple raw-byte records")

        if os.name == "posix" and hasattr(os, "killpg"):
            self_test_process_lifecycle(root)

        binary = root / "probe"
        binary.write_bytes(b"first")
        cache_path = root / "CMakeCache.txt"
        cache_path.write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        verify_release_build(binary)
        cache_path.write_text("CMAKE_BUILD_TYPE:STRING=Debug\n", encoding="utf-8")
        expect_rejected(lambda: verify_release_build(binary),
                        "a non-Release probe build cache")
        cache_path.write_text("CMAKE_BUILD_TYPE:STRING=Release\n", encoding="utf-8")
        first_hash = sha256_file(binary)
        binary.write_bytes(b"second")
        require(sha256_file(binary) != first_hash, "binary replacement did not change SHA-256")

        symlink_anchor = root / "artifact-anchor.txt"
        symlink_anchor.write_bytes(b"sentinel")
        symlink_artifact = root / "artifact-link.json"
        symlink_artifact.symlink_to(symlink_anchor)
        expect_rejected(
            lambda: load_campaign_artifact(
                lexical_absolute(symlink_artifact), artifact["summary_record"]
            ),
            "a canonical artifact symlink",
        )
        invalidate_artifact(lexical_absolute(symlink_artifact))
        require(not os.path.lexists(symlink_artifact) and
                symlink_anchor.read_bytes() == b"sentinel",
                "artifact invalidation followed a final symlink")

        destination = root / "campaign.json"
        destination.write_text("stale pass", encoding="utf-8")
        invalidate_artifact(destination)
        require(not destination.exists(), "stale campaign artifact survived invalidation")
        write_atomic_json(destination, artifact)
        load_campaign_artifact(destination, artifact["summary_record"])
        expect_rejected(
            lambda: load_campaign_artifact(
                destination,
                replace_field(artifact["summary_record"], "wall_ratio_ppm", "1"),
            ),
            "canonical artifact with a different outer summary",
        )
        require(not list(root.glob(".campaign.json.*.tmp")),
                "atomic publication retained a temporary file")
        blocker = root / "not-a-directory"
        blocker.write_text("block", encoding="utf-8")
        expect_rejected(lambda: write_atomic_json(blocker / "campaign.json", artifact),
                        "publication through a non-directory parent")

        diagnostic_directory = root / "retained-diagnostic"
        failed_slot_fields = parse_route_record(structured)
        failed_slot = SlotResult(
            ordinal=1,
            route="structured",
            route_sample_ordinal=1,
            status="fail",
            elapsed_ms=1,
            record=structured,
            record_fields=failed_slot_fields,
            identity_sha256=None,
            artifact_lifecycle="retained",
            failure="synthetic timeout",
        )
        failed_diagnostic = make_failure_diagnostic(
            source=source, max_batch_workers=4,
            max_local_threads="auto", timeout_s=7200, samples_per_route=2,
            order=campaign_order(2), started_utc="2026-01-01T00:00:00Z",
            completed_utc="2026-01-01T00:00:01Z",
            slots=[slots[0], failed_slot], identity=None,
            stability=None, budget=None,
            failure={
                "stage": "process", "slot_ordinal": 1, "code": "timeout",
                "message": "synthetic timeout",
                "diagnostic_directory": str(diagnostic_directory),
            },
        )
        if failed_diagnostic["source"]["host_system"] not in {"linux", "darwin"}:
            failed_diagnostic["source"]["host_system"] = "linux"
        validate_failure_diagnostic(failed_diagnostic)
        next_slot_failure = make_failure_diagnostic(
            source=source, max_batch_workers=4,
            max_local_threads="auto", timeout_s=7200, samples_per_route=2,
            order=campaign_order(2), started_utc="2026-01-01T00:00:00Z",
            completed_utc="2026-01-01T00:00:01Z", slots=slots[:1], identity=None,
            stability=None, budget=None,
            failure={
                "stage": "source", "slot_ordinal": 1,
                "code": "source_identity_drift",
                "message": "synthetic next-slot preflight failure",
                "diagnostic_directory": str(diagnostic_directory),
            },
        )
        if next_slot_failure["source"]["host_system"] not in {"linux", "darwin"}:
            next_slot_failure["source"]["host_system"] = "linux"
        validate_failure_diagnostic(next_slot_failure)
        partial_prefix_without_ordinal = copy.deepcopy(next_slot_failure)
        partial_prefix_without_ordinal["failure"]["slot_ordinal"] = None
        expect_rejected(
            lambda: validate_failure_diagnostic(partial_prefix_without_ordinal),
            "partial all-pass prefix without its next-slot ordinal",
        )
        complete_post_run_failure = make_failure_diagnostic(
            source=source, max_batch_workers=4,
            max_local_threads="auto", timeout_s=7200, samples_per_route=2,
            order=campaign_order(2), started_utc="2026-01-01T00:00:00Z",
            completed_utc="2026-01-01T00:00:01Z", slots=slots, identity=None,
            stability=None, budget=None,
            failure={
                "stage": "source", "slot_ordinal": None,
                "code": "source_identity_drift",
                "message": "synthetic post-run source failure",
                "diagnostic_directory": str(diagnostic_directory),
            },
        )
        if complete_post_run_failure["source"]["host_system"] not in {"linux", "darwin"}:
            complete_post_run_failure["source"]["host_system"] = "linux"
        validate_failure_diagnostic(complete_post_run_failure)
        mismatched_failure_ordinal = copy.deepcopy(failed_diagnostic)
        mismatched_failure_ordinal["failure"]["slot_ordinal"] = 0
        expect_rejected(
            lambda: validate_failure_diagnostic(mismatched_failure_ordinal),
            "failure ordinal bound to a different slot",
        )
        pass_after_failure = make_failure_diagnostic(
            source=source, max_batch_workers=4,
            max_local_threads="auto", timeout_s=7200, samples_per_route=2,
            order=campaign_order(2), started_utc="2026-01-01T00:00:00Z",
            completed_utc="2026-01-01T00:00:01Z",
            slots=[slots[0], failed_slot, slots[2]], identity=None,
            stability=None, budget=None,
            failure={
                "stage": "process", "slot_ordinal": 1, "code": "timeout",
                "message": "synthetic timeout",
                "diagnostic_directory": str(diagnostic_directory),
            },
        )
        if pass_after_failure["source"]["host_system"] not in {"linux", "darwin"}:
            pass_after_failure["source"]["host_system"] = "linux"
        expect_rejected(
            lambda: validate_failure_diagnostic(pass_after_failure),
            "passing slot after a failed slot",
        )
        second_failed_slot = SlotResult(
            ordinal=2,
            route="structured",
            route_sample_ordinal=2,
            status="fail",
            elapsed_ms=1,
            record=structured,
            record_fields=failed_slot_fields,
            identity_sha256=None,
            artifact_lifecycle="retained",
            failure="second synthetic timeout",
        )
        multiple_failures = make_failure_diagnostic(
            source=source, max_batch_workers=4,
            max_local_threads="auto", timeout_s=7200, samples_per_route=2,
            order=campaign_order(2), started_utc="2026-01-01T00:00:00Z",
            completed_utc="2026-01-01T00:00:01Z",
            slots=[slots[0], failed_slot, second_failed_slot], identity=None,
            stability=None, budget=None,
            failure={
                "stage": "process", "slot_ordinal": 2, "code": "timeout",
                "message": "second synthetic timeout",
                "diagnostic_directory": str(diagnostic_directory),
            },
        )
        if multiple_failures["source"]["host_system"] not in {"linux", "darwin"}:
            multiple_failures["source"]["host_system"] = "linux"
        expect_rejected(
            lambda: validate_failure_diagnostic(multiple_failures),
            "multiple failed campaign slots",
        )
        diagnostic_bool_version = copy.deepcopy(failed_diagnostic)
        diagnostic_bool_version["artifact_format_version"] = True
        expect_rejected(lambda: validate_failure_diagnostic(diagnostic_bool_version),
                        "boolean diagnostic format version")
        diagnostic_bool_slot = copy.deepcopy(failed_diagnostic)
        diagnostic_bool_slot["slots"][0]["elapsed_ms"] = True
        expect_rejected(lambda: validate_failure_diagnostic(diagnostic_bool_slot),
                        "boolean diagnostic slot elapsed time")
        diagnostic_unknown_record_field = copy.deepcopy(failed_diagnostic)
        assert diagnostic_unknown_record_field["slots"][1]["status"] == "fail"
        assert diagnostic_unknown_record_field["slots"][1]["record_fields"] is not None
        diagnostic_unknown_record_field["slots"][1]["record_fields"]["unknown"] = "1"
        expect_rejected(
            lambda: validate_failure_diagnostic(diagnostic_unknown_record_field),
            "unknown diagnostic record field",
        )
        diagnostic_record_mismatch = copy.deepcopy(failed_diagnostic)
        diagnostic_record_mismatch["slots"][1]["record"] = replace_field(
            diagnostic_record_mismatch["slots"][1]["record"], "wall_ms", "101"
        )
        expect_rejected(
            lambda: validate_failure_diagnostic(diagnostic_record_mismatch),
            "diagnostic record/record_fields mismatch",
        )
        failure_summary = parse_summary_record(failed_diagnostic["summary_record"])
        require(failure_summary["status"] == "fail" and
                failure_summary["artifact_published"] == "false",
                "failure summary claimed canonical publication")
        fail_claiming_published = replace_field(
            failed_diagnostic["summary_record"], "artifact_published", "true"
        )
        expect_rejected(lambda: parse_summary_record(fail_claiming_published),
                        "fail summary claiming canonical publication")
        stale_canonical = root / "failure-canonical.json"
        stale_canonical.write_text("stale pass", encoding="utf-8")
        invalidate_artifact(stale_canonical)
        diagnostic_directory.mkdir()
        write_atomic_diagnostic(diagnostic_directory / "diagnostic.json", failed_diagnostic)
        require(not stale_canonical.exists(),
                "failure diagnostic path published a canonical artifact")

        invalid_destination = root / "invalid-canonical.json"
        mutated_for_publication = copy.deepcopy(artifact)
        mutated_for_publication["directional_budget"]["wall_ratio_ppm"] += 1
        expect_rejected(
            lambda: write_atomic_json(invalid_destination, mutated_for_publication),
            "invalid artifact before atomic replacement",
        )
        require(not invalid_destination.exists(),
                "invalid temporary artifact replaced the canonical destination")

        second_validation_destination = root / "second-validation-canonical.json"
        validator_calls = [0]

        def reject_serialized_document(_document: dict[str, Any]) -> None:
            validator_calls[0] += 1
            if validator_calls[0] == 2:
                raise CampaignError("synthetic serialized-document rejection")

        expect_rejected(
            lambda: write_validated_json(
                second_validation_destination,
                {"payload": "valid-in-memory"},
                reject_serialized_document,
                "second-validation fixture",
            ),
            "serialized temporary document rejected before replace",
        )
        require(validator_calls[0] == 2 and
                not second_validation_destination.exists() and
                not list(root.glob(".second-validation-canonical.json.*.tmp")),
                "pre-replace serialized validation was not fail-closed")

    print("50-digit route campaign closed-schema self-test: PASS")
    return 0


def parse_positive_uint(text: str) -> int:
    if UINT_RE.fullmatch(text) is None or int(text) == 0 or int(text) > UINT32_MAX:
        raise argparse.ArgumentTypeError("value must be a canonical uint32 greater than zero")
    return int(text)


def parse_samples(text: str) -> int:
    value = parse_positive_uint(text)
    if not 2 <= value <= 9:
        raise argparse.ArgumentTypeError("samples-per-route must be in 2..9")
    return value


def parse_batch_workers(text: str) -> int:
    value = parse_positive_uint(text)
    if not 1 <= value <= 4:
        raise argparse.ArgumentTypeError("max-batch-workers must be in 1..4")
    return value


def parse_local_threads(text: str) -> str:
    if text == "auto":
        return text
    parse_positive_uint(text)
    return text


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    run = subparsers.add_parser("run", help="run one complete-first-round campaign")
    run.add_argument("--project-root", type=Path, required=True)
    run.add_argument("--executable", type=Path, required=True)
    run.add_argument("--artifact", type=Path, required=True)
    run.add_argument("--samples-per-route", type=parse_samples, default=2)
    run.add_argument("--max-batch-workers", type=parse_batch_workers, default=4)
    run.add_argument("--max-local-threads", type=parse_local_threads, default="auto")
    run.add_argument("--timeout-s", type=parse_positive_uint, default=7200)

    invalidate = subparsers.add_parser("invalidate", help="invalidate one canonical artifact")
    invalidate.add_argument("--artifact", type=Path, required=True)

    check = subparsers.add_parser("validate-summary", help="validate one V1 summary record")
    check.add_argument("--record", required=True)

    validate_artifact_parser = subparsers.add_parser(
        "validate-artifact", help="re-read and validate one canonical pass artifact"
    )
    validate_artifact_parser.add_argument("--artifact", type=Path, required=True)
    validate_artifact_parser.add_argument("--summary-record", required=True)

    self_check = subparsers.add_parser("self-test", help="run synthetic contract checks")
    self_check.add_argument("--structured-record", required=True)
    self_check.add_argument("--legacy-record", required=True)
    return parser


def handle_termination_signal(signal_number: int, _frame: Any) -> None:
    raise CampaignError(
        f"campaign interrupted by signal {signal_number}",
        stage="process",
        code="interrupted",
    )


def main() -> NoReturn:
    parser = build_parser()
    arguments = parser.parse_args()
    previous_signal_handlers: list[tuple[int, Any]] = []
    try:
        if arguments.command == "run":
            for signal_number in campaign_termination_signals():
                previous_handler = signal.getsignal(signal_number)
                previous_signal_handlers.append((signal_number, previous_handler))
                signal.signal(signal_number, handle_termination_signal)
        if arguments.command == "run":
            status = execute_campaign(arguments)
        elif arguments.command == "invalidate":
            invalidate_artifact(lexical_absolute(arguments.artifact))
            status = 0
        elif arguments.command == "validate-summary":
            parse_summary_record(arguments.record)
            status = 0
        elif arguments.command == "validate-artifact":
            load_campaign_artifact(
                lexical_absolute(arguments.artifact), arguments.summary_record
            )
            status = 0
        else:
            status = self_test(arguments)
    except CampaignError as error:
        print(f"50-digit route campaign error: {error}", file=sys.stderr)
        status = 1
    except KeyboardInterrupt:
        print("50-digit route campaign error: interrupted by SIGINT", file=sys.stderr)
        status = 1
    finally:
        for signal_number, previous_handler in reversed(previous_signal_handlers):
            signal.signal(signal_number, previous_handler)
    raise SystemExit(status)


if __name__ == "__main__":
    main()
