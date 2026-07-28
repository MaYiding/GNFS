#!/usr/bin/env python3
"""Validate the closed GNFS environment inventory for distributed-sieve work identity."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


SOURCE_ROOTS = (
    "include/gnfs/sieve",
    "include/gnfs/cofactor",
    "src/sieve",
    "src/cofactor",
)
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".ipp",
    ".mm",
}

SEMANTIC_FLAGS = (
    "GNFS_LATTICE_LLL",
    "GNFS_LATTICE_SKEW",
    "GNFS_ADAPTIVE_LATTICE",
    "GNFS_ADAPTIVE_LATTICE_THRESHOLD",
    "GNFS_ADAPTIVE_LATTICE_MAX_RETRIES",
    "GNFS_ADAPTIVE_LATTICE_SEED",
    "GNFS_SURVIVAL_FILTER",
    "GNFS_SURVIVAL_THRESHOLD",
    "GNFS_COFACTOR_BRENT",
    "GNFS_ECM_BRENT_SUYAMA",
    "GNFS_ECM_BS_DEGREE",
    "GNFS_ECM_SIGMA_POOL_SIZE",
    "GNFS_ECM_CURVE_POOL",
)

CONSERVATIVE_FLAGS = (
    "GNFS_ECM_BATCH_INV",
    "GNFS_COFACTOR_BATCH_SIZE",
    "GNFS_BRENT_POLLARD_RHO_THREADS",
    "GNFS_ECM_B1_CACHE_SIZE",
    "GNFS_ECM_STAGE1_PARALLEL_THREADS",
    "GNFS_ECM_STAGE2_PARALLEL",
    "GNFS_COFACTOR_RESULT_CACHE_SIZE",
    "GNFS_TRIAL_DIV_SIMD",
    "GNFS_LATTICE_BASIS_PARALLEL_THREADS",
    "GNFS_LATTICE_COORDS_SIMD",
    "GNFS_SIEVE_APPLY_TILE_THREADS",
    "GNFS_BUCKET_PREFETCH",
    "GNFS_SIEVE_ECORE_THREADS",
    "GNFS_SIEVE_NO_TINY_SIMD",
    "GNFS_SIEVE_NORM_TILE_BITS",
    "GNFS_SIEVE_REGION_TILE_BITS",
    "GNFS_SIEVE_SATURATED_SUB_SIMD",
    "GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD",
)

DIAGNOSTIC_FLAGS = ("GNFS_COFACTOR_TIMING_ENABLE",)

DISTRIBUTED_FLAGS = (
    "GNFS_DISTRIBUTED_SIEVE_WORKERS",
    "GNFS_DISTRIBUTED_SIEVE_BASE_PATH",
    "GNFS_DISTRIBUTED_SIEVE_SQ_PER_WORKER",
)

CATEGORY_FLAGS = {
    "semantic": SEMANTIC_FLAGS,
    "conservative": CONSERVATIVE_FLAGS,
    "diagnostic": DIAGNOSTIC_FLAGS,
    "distributed": DISTRIBUTED_FLAGS,
}

CANONICAL_EXECUTION_POLICY_FLAGS = SEMANTIC_FLAGS + CONSERVATIVE_FLAGS
EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS = (
    CANONICAL_EXECUTION_POLICY_FLAGS + DIAGNOSTIC_FLAGS
)
EXECUTION_POLICY_ENVIRONMENT_ADAPTER = (
    "src/sieve/distributed_sieve_execution_policy_environment.cpp"
)
EXECUTION_POLICY_ENVIRONMENT_CAPTURE = (
    "capture_distributed_sieve_execution_policy_environment_v1"
)
EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER = "std::thread::hardware_concurrency"

# These implementation units are the environment-free side of the planned
# freeze boundary. Legacy parsing remains outside them.
DURABLE_ENVIRONMENT_FREE_FILES = {
    "include/gnfs/sieve/distributed_sieve_protocol.hpp",
    "include/gnfs/sieve/distributed_sieve_resume.hpp",
    "src/sieve/distributed_sieve_execution_policy.cpp",
    "src/sieve/distributed_sieve_execution_policy_internal.hpp",
    "src/sieve/distributed_sieve_bound_work.cpp",
    "src/sieve/distributed_sieve_bound_work_internal.hpp",
    "src/sieve/distributed_sieve_cofactor_runtime_config.cpp",
    "src/sieve/distributed_sieve_cofactor_runtime_config_internal.hpp",
    "src/sieve/distributed_sieve_lattice_runtime_config.cpp",
    "src/sieve/distributed_sieve_lattice_runtime_config_internal.hpp",
    "src/sieve/distributed_sieve_protocol.cpp",
    "src/sieve/distributed_sieve_resume.cpp",
    "src/sieve/distributed_sieve_seed_v2.cpp",
    "src/sieve/distributed_sieve_work_identity_codec.cpp",
    "src/sieve/distributed_sieve_work_identity_codec_internal.hpp",
    "src/sieve/distributed_sieve_work_package_codec.cpp",
    "src/sieve/distributed_sieve_work_package_codec_internal.hpp",
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "src/sieve/distributed_sieve_worker_work_package_file_ops_internal.hpp",
    "src/sieve/distributed_sieve_worker_process.cpp",
    "src/sieve/distributed_sieve_worker_process_internal.hpp",
    "include/gnfs/sieve/distributed_sieve_seed_v2.hpp",
}

WORKER_PROCESS_TRANSPORT_FILE = "src/sieve/distributed_sieve_worker_process.cpp"
WORKER_PROCESS_TRANSPORT_FILES = {
    WORKER_PROCESS_TRANSPORT_FILE,
    "src/sieve/distributed_sieve_worker_process_internal.hpp",
}
WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerProcessFixedCapabilitySourcesV1",
    "spawn_distributed_sieve_worker_process_batch_with_capabilities",
)
WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_process.cpp",
    "src/sieve/distributed_sieve_worker_process_internal.hpp",
    "tests/test_distributed_sieve_worker_process.cpp",
}
WORKER_PROCESS_LEGACY_FILE = "src/sieve/distributed_sieve.cpp"
WORKER_PROCESS_POLICY_PREFIXES = (
    "include/gnfs/sieve/",
    "src/sieve/",
)
WORKER_PROCESS_DIRECT_CALL_ALLOWLIST = {
    "posix_spawn": {WORKER_PROCESS_TRANSPORT_FILE},
    "fork": {WORKER_PROCESS_LEGACY_FILE},
    "waitpid": {
        WORKER_PROCESS_LEGACY_FILE,
        WORKER_PROCESS_TRANSPORT_FILE,
    },
}
WORKER_PROCESS_REQUIRED_DIRECT_CALLS = {
    (WORKER_PROCESS_TRANSPORT_FILE, "posix_spawn"): 1,
    (WORKER_PROCESS_TRANSPORT_FILE, "waitpid"): 1,
    (WORKER_PROCESS_LEGACY_FILE, "fork"): 1,
    (WORKER_PROCESS_LEGACY_FILE, "waitpid"): 1,
}
WORKER_PROCESS_FORBIDDEN_PROCESS_IDENTIFIERS = (
    "_Fork",
    "vfork",
    "posix_spawnp",
    "waitid",
    "wait3",
    "wait4",
)
WORKER_PROCESS_TRANSPORT_FORBIDDEN_IDENTIFIERS = ("environ",)

# Code-token bans close indirect ambient-policy entrances that do not contain
# getenv/random_device themselves. The runtime mapper is deliberately a pure
# projection: it must not construct a legacy runtime object or invoke a basis
# helper whose overload could re-read process or host state.
DURABLE_FORBIDDEN_IDENTIFIERS = (
    "from_env",
)
DURABLE_FORBIDDEN_CALLS = (
    "hardware_concurrency",
)
DURABLE_PURE_RUNTIME_MAPPER_FILES = {
    "include/gnfs/sieve/distributed_sieve_seed_v2.hpp",
    "src/sieve/distributed_sieve_bound_work.cpp",
    "src/sieve/distributed_sieve_bound_work_internal.hpp",
    "src/sieve/distributed_sieve_cofactor_runtime_config.cpp",
    "src/sieve/distributed_sieve_cofactor_runtime_config_internal.hpp",
    "src/sieve/distributed_sieve_lattice_runtime_config.cpp",
    "src/sieve/distributed_sieve_lattice_runtime_config_internal.hpp",
    "src/sieve/distributed_sieve_seed_v2.cpp",
}
DURABLE_BOUND_WORK_FILES = {
    "src/sieve/distributed_sieve_bound_work.cpp",
    "src/sieve/distributed_sieve_bound_work_internal.hpp",
}
DURABLE_BOUND_WORK_FORBIDDEN_IDENTIFIERS = (
    "DistributedSieveWaveStore",
    "DistributedSieveWorkerAttemptStartReceipt",
    "OOCCleanupTransaction",
    "OOCPrivateLease",
    "OOCRelationWriter",
    "RelationCollector",
    "RelationSink",
    "run_distributed_sieve",
    "process_id",
    "getpid",
    "fork",
    "open",
    "mkdir",
    "rename",
    "unlink",
    "posix_spawn",
    "ThreadPool",
    "async",
    "filesystem",
    "ifstream",
    "ofstream",
    "fstream",
    "fopen",
    "opendir",
    "steady_clock",
    "system_clock",
    "high_resolution_clock",
)
BOUND_WORK_SCAN_EXCLUDED_TOP_LEVEL_PREFIXES = (
    ".",
    "build",
    "cmake-build",
    "xcode-build",
)
BOUND_WORK_USE_SITE_IDENTIFIERS = (
    "DistributedSieveBoundWorkV1",
    "DistributedSieveBoundWorkResultV1",
    "bind_distributed_sieve_work_v1",
)
BOUND_WORK_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_bound_work.cpp",
    "src/sieve/distributed_sieve_bound_work_internal.hpp",
    "tests/test_distributed_sieve_execution_policy.cpp",
}
WORK_PACKAGE_CARRIER_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerWorkPackageFileV1",
    "create_distributed_sieve_worker_work_package_file_v1",
)
WORK_PACKAGE_CARRIER_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
}
LEGACY_PIPELINE_FILE = "src/api/pipeline.cpp"
LEGACY_PIPELINE_DURABLE_FORBIDDEN_IDENTIFIERS = (
    "DistributedSieveWorkIdentityV1",
    "DistributedSieveBoundWorkV1",
    "bind_distributed_sieve_work_v1",
    "distributed_sieve_bound_work_internal",
)
DURABLE_PURE_RUNTIME_MAPPER_FORBIDDEN_IDENTIFIERS = (
    "AdaptiveBasisManager",
    "LatticeSieve",
    "compute_lattice_basis",
    "compute_lattice_basis_with_skewness",
    "lattice_reduction_method_from_env",
    "lattice_skew_enabled_from_env",
    "parallel_lattice_basis_reduce",
    "brent_pollard_enabled",
    "classify_cofactor",
    "classify_cofactor_seeded_v1",
    "classify_cofactor_seeded_with_brent_v1",
    "quick_factor",
    "survival_filter_enabled",
    "apply_brent_suyama_env",
)
DURABLE_PURE_RUNTIME_MAPPER_FORBIDDEN_CALLS = (
    "lattice_basis_parallel_threads",
    "survival_threshold",
)


@dataclass(frozen=True)
class LegacyDynamicRead:
    relative: str
    normalized_argument: str
    display_name: str


LEGACY_TEST_ONLY_READS = (
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_FAIL_HANDOFF_PENDING_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_CORRUPT_REPORT_ATTEMPT_<chunk_id>",
    ),
    LegacyDynamicRead(
        relative="src/sieve/distributed_sieve.cpp",
        normalized_argument=(
            '("GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_"'
            "+std::to_string(chunk_id)).c_str()"
        ),
        display_name="GNFS_DISTRIBUTED_SIEVE_CORRUPT_RECEIPT_ATTEMPT_<chunk_id>",
    ),
)


@dataclass(frozen=True)
class LegacyExecutionPolicyLiteralRead:
    name: str
    relative: str


# Every canonical/diagnostic setting has exactly one legacy parser while M2e
# remains dormant. Keep this edge set closed by both flag and file: a known
# flag appearing in another file is not equivalent, and a duplicate read in
# the expected file is still a post-capture reread risk.
LEGACY_EXECUTION_POLICY_LITERAL_READS = (
    LegacyExecutionPolicyLiteralRead(
        "GNFS_LATTICE_LLL", "include/gnfs/sieve/lattice_basis.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_LATTICE_SKEW", "include/gnfs/sieve/lattice_basis.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ADAPTIVE_LATTICE", "include/gnfs/sieve/adaptive_lattice.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ADAPTIVE_LATTICE_THRESHOLD",
        "include/gnfs/sieve/adaptive_lattice.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ADAPTIVE_LATTICE_MAX_RETRIES",
        "include/gnfs/sieve/adaptive_lattice.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ADAPTIVE_LATTICE_SEED", "include/gnfs/sieve/adaptive_lattice.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SURVIVAL_FILTER", "include/gnfs/cofactor/survival_predictor.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SURVIVAL_THRESHOLD", "include/gnfs/cofactor/survival_predictor.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_COFACTOR_BRENT", "include/gnfs/cofactor/smooth_check.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_BRENT_SUYAMA", "include/gnfs/cofactor/ecm.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_BS_DEGREE", "include/gnfs/cofactor/ecm.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_SIGMA_POOL_SIZE", "include/gnfs/cofactor/sigma_seed_pool.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_CURVE_POOL", "include/gnfs/cofactor/ecm_curve_pool.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_BATCH_INV", "include/gnfs/cofactor/batch_inversion.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_COFACTOR_BATCH_SIZE", "include/gnfs/cofactor/batch_trial.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_BRENT_POLLARD_RHO_THREADS",
        "include/gnfs/cofactor/brent_pollard_rho_parallel.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_B1_CACHE_SIZE", "include/gnfs/cofactor/ecm_prime_cache.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_STAGE1_PARALLEL_THREADS",
        "include/gnfs/cofactor/ecm_stage1_parallel.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_ECM_STAGE2_PARALLEL",
        "include/gnfs/cofactor/ecm_stage2_parallel.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_COFACTOR_RESULT_CACHE_SIZE",
        "include/gnfs/cofactor/result_cache.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_TRIAL_DIV_SIMD", "include/gnfs/cofactor/trial_div_simd.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_LATTICE_BASIS_PARALLEL_THREADS",
        "include/gnfs/sieve/lattice_basis_parallel.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_LATTICE_COORDS_SIMD", "include/gnfs/sieve/lattice_coords_simd.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_APPLY_TILE_THREADS",
        "include/gnfs/sieve/apply_tile_parallel.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_BUCKET_PREFETCH", "include/gnfs/sieve/bucket_prefetch.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_ECORE_THREADS", "include/gnfs/sieve/ecore_qos.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_NO_TINY_SIMD", "include/gnfs/sieve/lattice_sieve.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_NORM_TILE_BITS", "include/gnfs/sieve/norm_tile.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_REGION_TILE_BITS", "include/gnfs/sieve/region_tile.hpp"
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_SATURATED_SUB_SIMD",
        "include/gnfs/sieve/saturated_sub_simd.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD",
        "include/gnfs/sieve/threshold_scan_simd.hpp",
    ),
    LegacyExecutionPolicyLiteralRead(
        "GNFS_COFACTOR_TIMING_ENABLE", "include/gnfs/cofactor/stage_timing.hpp"
    ),
)

LEGACY_EXECUTION_POLICY_LITERAL_READ_BY_FLAG = {
    entry.name: entry for entry in LEGACY_EXECUTION_POLICY_LITERAL_READS
}

# Existing nondeterministic legacy code stays visible and count-closed while the
# durable execution path moves to explicit deterministic random sources.
LEGACY_RANDOM_DEVICE_USES = {
    "include/gnfs/cofactor/ecm.hpp": 3,
}

LITERAL_ARGUMENT = re.compile(r'\s*"(GNFS_[A-Z0-9_]+)"\s*\Z')


@dataclass(frozen=True, order=True)
class GetenvCall:
    line: int
    argument: str


@dataclass(frozen=True, order=True)
class ClassifiedRead:
    category: str
    name: str
    relative: str
    line: int


@dataclass(frozen=True, order=True)
class CodeIdentifierUse:
    line: int
    offset: int


def _skip_quoted(text: str, start: int, quote: str) -> int:
    cursor = start + 1
    while cursor < len(text):
        if text[cursor] == "\\":
            cursor += 2
            continue
        cursor += 1
        if text[cursor - 1] == quote:
            break
    return cursor


def _skip_raw_string(text: str, start: int) -> int | None:
    if start + 1 >= len(text) or text[start] != "R" or text[start + 1] != '"':
        return None
    delimiter_end = text.find("(", start + 2, min(len(text), start + 19))
    if delimiter_end < 0:
        return None
    delimiter = text[start + 2 : delimiter_end]
    if any(char.isspace() or char in "\\()" for char in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    end = text.find(terminator, delimiter_end + 1)
    return len(text) if end < 0 else end + len(terminator)


def _skip_non_code(text: str, cursor: int) -> int | None:
    raw_end = _skip_raw_string(text, cursor)
    if raw_end is not None:
        return raw_end
    if text[cursor] in {'"', "'"}:
        return _skip_quoted(text, cursor, text[cursor])
    if text.startswith("//", cursor):
        newline = text.find("\n", cursor + 2)
        return len(text) if newline < 0 else newline
    if text.startswith("/*", cursor):
        end = text.find("*/", cursor + 2)
        return len(text) if end < 0 else end + 2
    return None


def _matching_parenthesis(text: str, opening: int) -> int | None:
    depth = 1
    cursor = opening + 1
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text[cursor] == "(":
            depth += 1
        elif text[cursor] == ")":
            depth -= 1
            if depth == 0:
                return cursor
        cursor += 1
    return None


def _skip_call_trivia(text: str, cursor: int) -> int:
    while cursor < len(text):
        if text[cursor].isspace():
            cursor += 1
            continue
        if text.startswith("//", cursor) or text.startswith("/*", cursor):
            skipped = _skip_non_code(text, cursor)
            if skipped is None:
                return cursor
            cursor = skipped
            continue
        return cursor
    return cursor


def find_code_identifier_uses(text: str, identifier: str) -> list[CodeIdentifierUse]:
    uses: list[CodeIdentifierUse] = []
    cursor = 0
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text.startswith(identifier, cursor):
            before = text[cursor - 1] if cursor > 0 else ""
            after_index = cursor + len(identifier)
            after = text[after_index] if after_index < len(text) else ""
            if not ((before.isalnum() or before == "_") or (after.isalnum() or after == "_")):
                uses.append(
                    CodeIdentifierUse(
                        line=text.count("\n", 0, cursor) + 1,
                        offset=cursor,
                    )
                )
                cursor = after_index
                continue
        cursor += 1
    return uses


def find_non_call_identifier_uses(
    text: str, identifier: str
) -> list[CodeIdentifierUse]:
    result: list[CodeIdentifierUse] = []
    for use in find_code_identifier_uses(text, identifier):
        opening = _skip_call_trivia(text, use.offset + len(identifier))
        if opening >= len(text) or text[opening] != "(":
            result.append(use)
    return result


def find_call_identifier_uses(
    text: str, identifier: str
) -> list[CodeIdentifierUse]:
    result: list[CodeIdentifierUse] = []
    for use in find_code_identifier_uses(text, identifier):
        opening = _skip_call_trivia(text, use.offset + len(identifier))
        if opening < len(text) and text[opening] == "(":
            result.append(use)
    return result


def _matching_brace(text: str, opening: int) -> int | None:
    depth = 1
    cursor = opening + 1
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return cursor
        cursor += 1
    return None


def find_function_body(
    text: str, function_name: str
) -> tuple[str | None, int, list[tuple[int, str]]]:
    errors: list[tuple[int, str]] = []
    uses = find_code_identifier_uses(text, function_name)
    if len(uses) != 1:
        errors.append(
            (
                1,
                f"expected exactly one {function_name} definition, found {len(uses)} identifiers",
            )
        )
        return None, 0, errors

    use = uses[0]
    opening = _skip_call_trivia(text, use.offset + len(function_name))
    if opening >= len(text) or text[opening] != "(":
        errors.append((use.line, f"{function_name} is not followed by a parameter list"))
        return None, 0, errors
    closing = _matching_parenthesis(text, opening)
    if closing is None:
        errors.append((use.line, f"{function_name} has an unterminated parameter list"))
        return None, 0, errors

    cursor = closing + 1
    body_opening: int | None = None
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text[cursor].isspace():
            cursor += 1
            continue
        if text[cursor] == ";":
            errors.append((use.line, f"{function_name} is only declared, not defined"))
            return None, 0, errors
        if text[cursor] == "{":
            body_opening = cursor
            break
        cursor += 1
    if body_opening is None:
        errors.append((use.line, f"{function_name} has no function body"))
        return None, 0, errors

    body_closing = _matching_brace(text, body_opening)
    if body_closing is None:
        errors.append((use.line, f"{function_name} has an unterminated function body"))
        return None, 0, errors
    body_line_offset = text.count("\n", 0, body_opening + 1)
    return text[body_opening + 1 : body_closing], body_line_offset, errors


def find_getenv_calls(text: str) -> tuple[list[GetenvCall], list[tuple[int, str]]]:
    calls: list[GetenvCall] = []
    errors: list[tuple[int, str]] = []
    cursor = 0
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text.startswith("getenv", cursor):
            before = text[cursor - 1] if cursor > 0 else ""
            after_index = cursor + len("getenv")
            after = text[after_index] if after_index < len(text) else ""
            if (before.isalnum() or before == "_") or (after.isalnum() or after == "_"):
                cursor += 1
                continue
            opening = _skip_call_trivia(text, after_index)
            if opening >= len(text) or text[opening] != "(":
                cursor += len("getenv")
                continue
            line = text.count("\n", 0, cursor) + 1
            closing = _matching_parenthesis(text, opening)
            if closing is None:
                errors.append((line, "unterminated getenv call"))
                break
            calls.append(GetenvCall(line=line, argument=text[opening + 1 : closing]))
            cursor = closing + 1
            continue
        cursor += 1
    return calls, errors


def normalize_dynamic_argument(argument: str) -> str:
    return re.sub(r"\s+", "", argument)


def build_flag_categories() -> tuple[dict[str, str], list[str]]:
    result: dict[str, str] = {}
    errors: list[str] = []
    for category, flags in CATEGORY_FLAGS.items():
        for flag in flags:
            previous = result.get(flag)
            if previous is not None:
                errors.append(f"{flag} is classified as both {previous} and {category}")
            result[flag] = category
    return result, errors


class Checks:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.errors: list[str] = []
        self.reads: list[ClassifiedRead] = []
        self.legacy_counts = {entry: 0 for entry in LEGACY_TEST_ONLY_READS}
        self.legacy_execution_policy_literal_counts = {
            entry: 0 for entry in LEGACY_EXECUTION_POLICY_LITERAL_READS
        }
        self.adapter_counts = {
            flag: 0 for flag in EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS
        }
        self.adapter_seen = False
        self.random_device_counts = {
            relative: 0 for relative in LEGACY_RANDOM_DEVICE_USES
        }
        self.worker_process_call_counts = {
            key: 0 for key in WORKER_PROCESS_REQUIRED_DIRECT_CALLS
        }

    def fail(self, relative: str, line: int, message: str) -> None:
        self.errors.append(f"{relative}:{line}: {message}")

    def source_files(self) -> list[tuple[str, Path]]:
        files: list[tuple[str, Path]] = []
        for relative_root in SOURCE_ROOTS:
            directory = self.root / relative_root
            if not directory.is_dir():
                self.fail(relative_root, 1, "source inventory directory is missing")
                continue
            for path in directory.rglob("*"):
                if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                    relative = path.relative_to(self.root).as_posix()
                    files.append((relative, path))
        return sorted(files)

    def bound_work_source_files(self) -> list[tuple[str, Path]]:
        files: list[tuple[str, Path]] = []
        for path in self.root.rglob("*"):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relative_path = path.relative_to(self.root)
            if not relative_path.parts:
                continue
            top_level = relative_path.parts[0]
            if any(
                top_level.startswith(prefix)
                for prefix in BOUND_WORK_SCAN_EXCLUDED_TOP_LEVEL_PREFIXES
            ):
                continue
            files.append((relative_path.as_posix(), path))
        return sorted(files)

    def validate_getenv_identifier_uses(
        self, relative: str, text: str, calls: list[GetenvCall]
    ) -> None:
        getenv_uses = find_code_identifier_uses(text, "getenv")
        non_call_getenv_uses = find_non_call_identifier_uses(text, "getenv")
        for use in non_call_getenv_uses:
            self.fail(
                relative,
                use.line,
                "getenv identifier must be used only as a direct call; "
                "aliases and function-pointer references are forbidden",
            )
        direct_getenv_use_count = len(getenv_uses) - len(non_call_getenv_uses)
        if direct_getenv_use_count != len(calls):
            self.fail(
                relative,
                1,
                f"getenv identifier/direct-call parser mismatch: "
                f"{direct_getenv_use_count} direct identifiers, {len(calls)} parsed calls",
            )

    def validate_durable_ambient_api_uses(
        self, relative: str, text: str
    ) -> None:
        if relative not in DURABLE_ENVIRONMENT_FREE_FILES:
            return

        for identifier in DURABLE_FORBIDDEN_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"durable implementation must not use ambient API {identifier}",
                )
        for identifier in DURABLE_FORBIDDEN_CALLS:
            for use in find_call_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"durable implementation must not call ambient API {identifier}",
                )

        if relative in DURABLE_BOUND_WORK_FILES:
            for identifier in DURABLE_BOUND_WORK_FORBIDDEN_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        f"bound-work mapper must not use runtime/side-effect API {identifier}",
                    )

        if relative not in DURABLE_PURE_RUNTIME_MAPPER_FILES:
            return
        for identifier in DURABLE_PURE_RUNTIME_MAPPER_FORBIDDEN_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"pure runtime mapper must not use legacy runtime API {identifier}",
                )
        for identifier in DURABLE_PURE_RUNTIME_MAPPER_FORBIDDEN_CALLS:
            for use in find_call_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"pure runtime mapper must not call legacy runtime API {identifier}",
                )

    def validate_worker_process_transport_boundary(
        self, relative: str, text: str
    ) -> None:
        if not relative.startswith(WORKER_PROCESS_POLICY_PREFIXES):
            return

        for identifier, allowed_files in WORKER_PROCESS_DIRECT_CALL_ALLOWLIST.items():
            uses = find_code_identifier_uses(text, identifier)
            calls = find_call_identifier_uses(text, identifier)
            if relative not in allowed_files:
                for use in uses:
                    self.fail(
                        relative,
                        use.line,
                        f"production {identifier} authority belongs only to "
                        f"{', '.join(sorted(allowed_files))}",
                    )
                continue
            count_key = (relative, identifier)
            if count_key in self.worker_process_call_counts:
                self.worker_process_call_counts[count_key] += len(calls)
            if len(uses) != len(calls):
                non_call_offsets = {
                    use.offset
                    for use in find_non_call_identifier_uses(text, identifier)
                }
                for use in uses:
                    if use.offset in non_call_offsets:
                        self.fail(
                            relative,
                            use.line,
                            f"{identifier} authority must be used only as a direct call",
                        )

        for identifier in WORKER_PROCESS_FORBIDDEN_PROCESS_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"sieve process policy forbids alternate API {identifier}",
                )

        if relative not in WORKER_PROCESS_TRANSPORT_FILES:
            return
        for identifier in WORKER_PROCESS_TRANSPORT_FORBIDDEN_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"self-exec process transport must not use {identifier}",
                )

    def validate_legacy_pipeline_boundary(self, text: str) -> None:
        for identifier in LEGACY_PIPELINE_DURABLE_FORBIDDEN_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    LEGACY_PIPELINE_FILE,
                    use.line,
                    f"legacy Pipeline must not self-mint durable work via {identifier}",
                )

    def validate_bound_work_use_site(self, relative: str, text: str) -> None:
        bound_uses: list[CodeIdentifierUse] = []
        for identifier in BOUND_WORK_USE_SITE_IDENTIFIERS:
            uses = find_code_identifier_uses(text, identifier)
            bound_uses.extend(uses)
            if relative not in BOUND_WORK_USE_SITE_ALLOWLIST:
                for use in uses:
                    self.fail(
                        relative,
                        use.line,
                        f"bound-work use site is not receipt-gated/allowlisted: {identifier}",
                    )
        if bound_uses:
            for use in find_code_identifier_uses(text, "run_distributed_sieve"):
                self.fail(
                    relative,
                    use.line,
                    "bound-work projection must not coexist with the legacy seeded runner",
                )

    def validate_work_package_carrier_use_site(
        self, relative: str, text: str
    ) -> None:
        if relative in WORK_PACKAGE_CARRIER_USE_SITE_ALLOWLIST:
            return
        for identifier in WORK_PACKAGE_CARRIER_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "anonymous work-package carrier authority is not "
                    f"receipt-gated/allowlisted: {identifier}",
                )

    def validate_worker_process_fixed_capability_use_site(
        self, relative: str, text: str
    ) -> None:
        if relative in WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_ALLOWLIST:
            return
        for identifier in WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "fixed-capability worker-process API use site is not "
                    f"allowlisted: {identifier}",
                )

    def classify(self, relative: str, call: GetenvCall, categories: dict[str, str]) -> None:
        if relative == EXECUTION_POLICY_ENVIRONMENT_ADAPTER:
            literal = LITERAL_ARGUMENT.fullmatch(call.argument)
            if literal is None:
                rendered = " ".join(call.argument.split())
                if len(rendered) > 160:
                    rendered = rendered[:157] + "..."
                self.fail(
                    relative,
                    call.line,
                    "execution-policy environment adapter requires a literal GNFS flag, "
                    f"found {rendered!r}",
                )
                return
            flag = literal.group(1)
            if flag not in self.adapter_counts:
                self.fail(
                    relative,
                    call.line,
                    f"execution-policy environment adapter must not read {flag}",
                )
                return
            self.adapter_counts[flag] += 1
            self.reads.append(
                ClassifiedRead(
                    category=categories[flag],
                    name=flag,
                    relative=relative,
                    line=call.line,
                )
            )
            return

        if relative in DURABLE_ENVIRONMENT_FREE_FILES:
            self.fail(
                relative,
                call.line,
                "durable protocol/execution-policy implementation must not read process environment",
            )
            return

        literal = LITERAL_ARGUMENT.fullmatch(call.argument)
        if literal is not None:
            flag = literal.group(1)
            category = categories.get(flag)
            if category is None:
                self.fail(relative, call.line, f"unclassified GNFS environment read {flag}")
                return
            if flag in EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS:
                expected = LEGACY_EXECUTION_POLICY_LITERAL_READ_BY_FLAG.get(flag)
                if expected is None:
                    self.fail(
                        relative,
                        call.line,
                        f"legacy execution-policy literal allowlist is missing {flag}",
                    )
                    return
                if relative != expected.relative:
                    self.fail(
                        relative,
                        call.line,
                        f"legacy execution-policy literal read {flag} is only allowed in "
                        f"{expected.relative}",
                    )
                    return
                self.legacy_execution_policy_literal_counts[expected] += 1
                count = self.legacy_execution_policy_literal_counts[expected]
                if count > 1:
                    self.fail(
                        relative,
                        call.line,
                        f"legacy execution-policy literal read {flag} must occur exactly once "
                        f"in {relative}, found at least {count}",
                    )
            self.reads.append(
                ClassifiedRead(category=category, name=flag, relative=relative, line=call.line)
            )
            return

        normalized = normalize_dynamic_argument(call.argument)
        matches = [
            entry
            for entry in LEGACY_TEST_ONLY_READS
            if entry.relative == relative and entry.normalized_argument == normalized
        ]
        if len(matches) == 1:
            entry = matches[0]
            self.legacy_counts[entry] += 1
            self.reads.append(
                ClassifiedRead(
                    category="legacy-test-only",
                    name=entry.display_name,
                    relative=relative,
                    line=call.line,
                )
            )
            return

        rendered = " ".join(call.argument.split())
        if len(rendered) > 160:
            rendered = rendered[:157] + "..."
        self.fail(
            relative,
            call.line,
            f"dynamic or non-GNFS getenv read is not in the exact legacy-test-only allowlist: "
            f"{rendered!r}",
        )

    def validate_legacy_execution_policy_literal_counts(self) -> None:
        for entry, count in self.legacy_execution_policy_literal_counts.items():
            if count != 1:
                self.fail(
                    entry.relative,
                    1,
                    f"legacy execution-policy literal allowlist expects exactly one "
                    f"{entry.name} read in this file, found {count}",
                )

    def classify_random_device(self, relative: str, use: CodeIdentifierUse) -> None:
        if relative in DURABLE_ENVIRONMENT_FREE_FILES:
            self.fail(
                relative,
                use.line,
                "durable protocol/execution-policy implementation must not use random_device",
            )
            return
        expected = LEGACY_RANDOM_DEVICE_USES.get(relative)
        if expected is None:
            self.fail(
                relative,
                use.line,
                "random_device use is not in the exact legacy allowlist",
            )
            return
        self.random_device_counts[relative] += 1
        if self.random_device_counts[relative] > expected:
            self.fail(
                relative,
                use.line,
                f"legacy random_device allowlist expects {expected} uses in this file",
            )

    def validate_environment_adapter(
        self, text: str, all_calls: list[GetenvCall]
    ) -> None:
        self.adapter_seen = True
        body, body_line_offset, body_errors = find_function_body(
            text, EXECUTION_POLICY_ENVIRONMENT_CAPTURE
        )
        for line, error in body_errors:
            self.fail(EXECUTION_POLICY_ENVIRONMENT_ADAPTER, line, error)
        if body is None:
            return

        all_host_uses = find_code_identifier_uses(
            text, EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
        )
        body_host_uses = find_code_identifier_uses(
            body, EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
        )
        body_host_non_call_uses = find_non_call_identifier_uses(
            body, EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
        )
        if len(body_host_uses) != 1:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                f"{EXECUTION_POLICY_ENVIRONMENT_CAPTURE} must contain exactly one "
                f"{EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER} identifier, "
                f"found {len(body_host_uses)}",
            )
        if body_host_non_call_uses:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                body_host_non_call_uses[0].line + body_line_offset,
                f"{EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER} must be used as a "
                f"direct call inside {EXECUTION_POLICY_ENVIRONMENT_CAPTURE}",
            )
        if len(all_host_uses) != len(body_host_uses):
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                f"all {EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER} identifiers must be "
                f"inside {EXECUTION_POLICY_ENVIRONMENT_CAPTURE}",
            )

        body_calls, parse_errors = find_getenv_calls(body)
        adjusted_body_calls = [
            GetenvCall(line=call.line + body_line_offset, argument=call.argument)
            for call in body_calls
        ]
        for line, error in parse_errors:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                line + body_line_offset,
                error,
            )
        if adjusted_body_calls != all_calls:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                f"all getenv reads must be inside {EXECUTION_POLICY_ENVIRONMENT_CAPTURE}",
            )

        observed: list[str] = []
        for call in adjusted_body_calls:
            literal = LITERAL_ARGUMENT.fullmatch(call.argument)
            if literal is None:
                self.fail(
                    EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                    call.line,
                    "descriptor capture getenv argument is not a literal GNFS flag",
                )
                continue
            observed.append(literal.group(1))

        expected = list(EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS)
        for flag in expected:
            count = observed.count(flag)
            if count != 1:
                self.fail(
                    EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                    1,
                    f"descriptor capture expects exactly one {flag} read, found {count}",
                )
        unexpected = sorted(set(observed) - set(expected))
        for flag in unexpected:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                f"descriptor capture contains unexpected flag {flag}",
            )
        if len(observed) == len(expected) and set(observed) == set(expected) and observed != expected:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                "descriptor capture flags are not in canonical key order followed by diagnostic",
            )

    def run(self) -> list[str]:
        categories, category_errors = build_flag_categories()
        for error in category_errors:
            self.fail("scripts/check_distributed_sieve_policy.py", 1, error)
        allowlisted_flags = tuple(
            entry.name for entry in LEGACY_EXECUTION_POLICY_LITERAL_READS
        )
        if allowlisted_flags != EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS:
            self.fail(
                "scripts/check_distributed_sieve_policy.py",
                1,
                "legacy execution-policy literal allowlist must contain the exact "
                "31 canonical flags followed by the diagnostic flag",
            )

        for relative, path in self.source_files():
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                self.fail(relative, 1, f"cannot read source: {exc}")
                continue
            calls, parse_errors = find_getenv_calls(text)
            for line, error in parse_errors:
                self.fail(relative, line, error)
            self.validate_getenv_identifier_uses(relative, text, calls)
            self.validate_durable_ambient_api_uses(relative, text)
            self.validate_worker_process_transport_boundary(relative, text)
            if relative == EXECUTION_POLICY_ENVIRONMENT_ADAPTER:
                self.validate_environment_adapter(text, calls)
            for call in calls:
                self.classify(relative, call, categories)
            for use in find_code_identifier_uses(text, "random_device"):
                self.classify_random_device(relative, use)

        try:
            pipeline_text = (self.root / LEGACY_PIPELINE_FILE).read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            self.fail(LEGACY_PIPELINE_FILE, 1, f"cannot read source: {exc}")
        else:
            self.validate_legacy_pipeline_boundary(pipeline_text)

        for relative, path in self.bound_work_source_files():
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                self.fail(relative, 1, f"cannot read bound-work use site: {exc}")
                continue
            self.validate_bound_work_use_site(relative, text)
            self.validate_work_package_carrier_use_site(relative, text)
            self.validate_worker_process_fixed_capability_use_site(relative, text)

        for entry, count in self.legacy_counts.items():
            if count != 1:
                self.fail(
                    entry.relative,
                    1,
                    f"legacy-test-only allowlist expects exactly one "
                    f"{entry.display_name} read, found {count}",
                )
        self.validate_legacy_execution_policy_literal_counts()

        if not self.adapter_seen:
            self.fail(
                EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                1,
                "execution-policy environment adapter is missing from the source inventory",
            )
        for flag, count in self.adapter_counts.items():
            if count != 1:
                self.fail(
                    EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
                    1,
                    f"environment adapter expects exactly one literal {flag} read, found {count}",
                )

        for relative, expected in LEGACY_RANDOM_DEVICE_USES.items():
            count = self.random_device_counts[relative]
            if count != expected:
                self.fail(
                    relative,
                    1,
                    f"legacy random_device allowlist expects exactly {expected} uses, found {count}",
                )

        for (relative, identifier), expected in WORKER_PROCESS_REQUIRED_DIRECT_CALLS.items():
            count = self.worker_process_call_counts[(relative, identifier)]
            if count != expected:
                self.fail(
                    relative,
                    1,
                    f"process policy requires exactly "
                    f"{expected} direct {identifier} call, found {count}",
                )

        observed_flags = {
            read.name for read in self.reads if read.category != "legacy-test-only"
        }
        for flag, category in categories.items():
            if flag not in observed_flags:
                self.fail(
                    "scripts/check_distributed_sieve_policy.py",
                    1,
                    f"classified {category} setting {flag} has no source getenv read",
                )

        self.errors.sort()
        self.reads.sort()
        return self.errors


def run_self_test() -> list[str]:
    errors: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    snippet = r'''
// getenv("GNFS_NOT_A_REAL_READ")
const char* text = "getenv(\"GNFS_NOT_A_REAL_READ\")";
const char* semantic = std::getenv("GNFS_LATTICE_LLL");
const char* conservative = std::getenv /* inventory trivia */ ("GNFS_ECM_BATCH_INV");
const char* injected = std::getenv(
    ("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_" + std::to_string(chunk_id)).c_str());
'''
    calls, parse_errors = find_getenv_calls(snippet)
    expect(not parse_errors, f"unexpected parser errors: {parse_errors}")
    expect(len(calls) == 3, f"expected three real getenv calls, found {len(calls)}")
    if len(calls) == 3:
        literal = LITERAL_ARGUMENT.fullmatch(calls[0].argument)
        expect(
            literal is not None and literal.group(1) == "GNFS_LATTICE_LLL",
            "literal GNFS flag extraction failed",
        )
        conservative = LITERAL_ARGUMENT.fullmatch(calls[1].argument)
        expect(
            conservative is not None and conservative.group(1) == "GNFS_ECM_BATCH_INV",
            "comment-separated GNFS flag extraction failed",
        )
        expect(
            normalize_dynamic_argument(calls[2].argument)
            == LEGACY_TEST_ONLY_READS[0].normalized_argument,
            "exact dynamic test-injection normalization failed",
        )
        expect(
            calls[0].line == 4 and calls[1].line == 5 and calls[2].line == 6,
            "getenv line tracking failed",
        )
    direct_identifier_checks = Checks(Path("."))
    direct_identifier_checks.validate_getenv_identifier_uses(
        "src/sieve/example.cpp", snippet, calls
    )
    expect(
        not direct_identifier_checks.errors,
        f"direct getenv calls were rejected: {direct_identifier_checks.errors}",
    )

    alias_snippet = r'''
auto getter = std::getenv;
const char* value = getter("GNFS_LATTICE_LLL");
const char* direct = std::getenv("GNFS_LATTICE_LLL");
'''
    alias_calls, alias_parse_errors = find_getenv_calls(alias_snippet)
    alias_checks = Checks(Path("."))
    alias_checks.validate_getenv_identifier_uses(
        "src/sieve/example.cpp", alias_snippet, alias_calls
    )
    expect(
        not alias_parse_errors
        and len(alias_calls) == 1
        and alias_checks.errors
        == [
            "src/sieve/example.cpp:2: getenv identifier must be used only as a "
            "direct call; aliases and function-pointer references are forbidden"
        ],
        f"getenv alias/function-pointer reference was not rejected: "
        f"{alias_checks.errors}",
    )

    transport_checks = Checks(Path("."))
    transport_checks.validate_worker_process_transport_boundary(
        WORKER_PROCESS_TRANSPORT_FILE,
        r'''
// posix_spawn(); waitpid(); fork(); environ;
const char* text = "posix_spawn(); waitpid(); fork(); environ;";
const auto spawn_result = ::posix_spawn(&child, path, &actions, &attributes, argv, envp);
const auto observed = ::waitpid(child, &status, 0);
''',
    )
    expect(
        not transport_checks.errors
        and transport_checks.worker_process_call_counts[
            (WORKER_PROCESS_TRANSPORT_FILE, "posix_spawn")
        ]
        == 1
        and transport_checks.worker_process_call_counts[
            (WORKER_PROCESS_TRANSPORT_FILE, "waitpid")
        ]
        == 1,
        "source-private posix_spawn/waitpid direct calls were not counted exactly once",
    )

    leaked_transport_checks = Checks(Path("."))
    leaked_transport_checks.validate_worker_process_transport_boundary(
        "src/sieve/other.cpp",
        "const auto spawn_result = ::posix_spawn("
        "&child, path, &actions, &attributes, argv, envp);\n",
    )
    expect(
        leaked_transport_checks.errors
        == [
            "src/sieve/other.cpp:1: production posix_spawn authority belongs only to "
            "src/sieve/distributed_sieve_worker_process.cpp"
        ],
        "posix_spawn authority outside the source-private transport was not rejected",
    )

    aliased_transport_checks = Checks(Path("."))
    aliased_transport_checks.validate_worker_process_transport_boundary(
        WORKER_PROCESS_TRANSPORT_FILE,
        "const auto waiter = ::waitpid;\n",
    )
    expect(
        aliased_transport_checks.errors
        == [
            "src/sieve/distributed_sieve_worker_process.cpp:1: "
            "waitpid authority must be used only as a direct call"
        ],
        "waitpid function-pointer alias inside the transport was not rejected",
    )

    forbidden_transport_checks = Checks(Path("."))
    forbidden_transport_checks.validate_worker_process_transport_boundary(
        WORKER_PROCESS_TRANSPORT_FILE,
        "const auto child = ::fork();\n"
        "char** inherited_environment = environ;\n",
    )
    expect(
        forbidden_transport_checks.errors
        == [
            "src/sieve/distributed_sieve_worker_process.cpp:1: "
            "production fork authority belongs only to "
            "src/sieve/distributed_sieve.cpp",
            "src/sieve/distributed_sieve_worker_process.cpp:2: "
            "self-exec process transport must not use environ",
        ],
        "raw fork or inherited environment inside the transport was not rejected",
    )

    alternate_process_checks = Checks(Path("."))
    alternate_process_checks.validate_worker_process_transport_boundary(
        "include/gnfs/sieve/other.hpp",
        "const auto a = ::_Fork();\n"
        "const auto b = ::vfork();\n"
        "const auto c = ::posix_spawnp(&pid, name, actions, attrs, argv, envp);\n"
        "const auto d = ::waitid(P_PID, pid, &status, WEXITED);\n"
        "const auto e = ::wait3(&status, 0, nullptr);\n"
        "const auto f = ::wait4(pid, &status, 0, nullptr);\n",
    )
    expect(
        len(alternate_process_checks.errors) == 6
        and all(
            "sieve process policy forbids alternate API" in error
            for error in alternate_process_checks.errors
        ),
        "alternate spawn/fork/wait APIs or public sieve-header uses were not rejected",
    )

    header_spawn_checks = Checks(Path("."))
    header_spawn_checks.validate_worker_process_transport_boundary(
        "include/gnfs/sieve/other.hpp",
        "const auto spawned = ::posix_spawn(&pid, path, actions, attrs, argv, envp);\n",
    )
    expect(
        header_spawn_checks.errors
        == [
            "include/gnfs/sieve/other.hpp:1: production posix_spawn authority "
            "belongs only to src/sieve/distributed_sieve_worker_process.cpp"
        ],
        "public sieve-header posix_spawn authority was not rejected",
    )

    categories, category_errors = build_flag_categories()
    expect(not category_errors, f"category table is not disjoint: {category_errors}")
    expect(
        len(CANONICAL_EXECUTION_POLICY_FLAGS) == 31
        and len(set(CANONICAL_EXECUTION_POLICY_FLAGS)) == 31,
        "canonical execution-policy flag inventory is not exactly 31 unique flags",
    )
    expect(
        len(EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS) == 32
        and len(set(EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS)) == 32
        and EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS[-1]
        == "GNFS_COFACTOR_TIMING_ENABLE",
        "environment descriptor inventory is not 31 canonical flags plus one diagnostic",
    )
    expect(
        tuple(entry.name for entry in LEGACY_EXECUTION_POLICY_LITERAL_READS)
        == EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS
        and len(LEGACY_EXECUTION_POLICY_LITERAL_READ_BY_FLAG) == 32,
        "legacy execution-policy literal allowlist is not the exact 31+1 inventory",
    )
    expect(
        categories.get("GNFS_COFACTOR_TIMING_ENABLE") == "diagnostic",
        "diagnostic classification failed",
    )
    expect(
        normalize_dynamic_argument(
            '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"+std::to_string(other_chunk)).c_str()'
        )
        != LEGACY_TEST_ONLY_READS[0].normalized_argument,
        "legacy dynamic allowlist is not expression-exact",
    )

    unknown_checks = Checks(Path("."))
    unknown_checks.classify(
        "src/sieve/example.cpp",
        GetenvCall(line=17, argument='"GNFS_UNKNOWN_POLICY_FLAG"'),
        categories,
    )
    expect(
        unknown_checks.errors
        == [
            "src/sieve/example.cpp:17: "
            "unclassified GNFS environment read GNFS_UNKNOWN_POLICY_FLAG"
        ],
        "unknown GNFS flags do not fail with a source location",
    )

    legacy_checks = Checks(Path("."))
    legacy_entry = LEGACY_TEST_ONLY_READS[0]
    legacy_checks.classify(
        legacy_entry.relative,
        GetenvCall(line=23, argument=legacy_entry.normalized_argument),
        categories,
    )
    expect(
        legacy_checks.legacy_counts[legacy_entry] == 1
        and legacy_checks.reads
        == [
            ClassifiedRead(
                category="legacy-test-only",
                name=legacy_entry.display_name,
                relative=legacy_entry.relative,
                line=23,
            )
        ],
        "exact legacy dynamic read was not classified",
    )
    legacy_checks.classify(
        legacy_entry.relative,
        GetenvCall(
            line=29,
            argument=(
                '("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_"'
                "+std::to_string(other_chunk)).c_str()"
            ),
        ),
        categories,
    )
    expect(
        len(legacy_checks.errors) == 1
        and legacy_checks.errors[0].startswith(f"{legacy_entry.relative}:29:"),
        "altered legacy dynamic read does not fail with a source location",
    )

    closed_literal_checks = Checks(Path("."))
    for index, entry in enumerate(LEGACY_EXECUTION_POLICY_LITERAL_READS):
        closed_literal_checks.classify(
            entry.relative,
            GetenvCall(line=100 + index, argument=f'"{entry.name}"'),
            categories,
        )
    closed_literal_checks.validate_legacy_execution_policy_literal_counts()
    expect(
        not closed_literal_checks.errors
        and all(
            count == 1
            for count in closed_literal_checks.legacy_execution_policy_literal_counts.values()
        ),
        f"valid closed legacy literal reads were rejected: "
        f"{closed_literal_checks.errors}",
    )

    duplicate_literal_checks = Checks(Path("."))
    for index, entry in enumerate(LEGACY_EXECUTION_POLICY_LITERAL_READS):
        duplicate_literal_checks.classify(
            entry.relative,
            GetenvCall(line=150 + index, argument=f'"{entry.name}"'),
            categories,
        )
    duplicate_literal_entry = LEGACY_EXECUTION_POLICY_LITERAL_READS[0]
    duplicate_literal_checks.classify(
        duplicate_literal_entry.relative,
        GetenvCall(line=190, argument=f'"{duplicate_literal_entry.name}"'),
        categories,
    )
    duplicate_literal_checks.validate_legacy_execution_policy_literal_counts()
    expect(
        any("found at least 2" in error for error in duplicate_literal_checks.errors)
        and any("found 2" in error for error in duplicate_literal_checks.errors),
        "duplicate legacy execution-policy literal read was not rejected",
    )

    missing_literal_checks = Checks(Path("."))
    for index, entry in enumerate(LEGACY_EXECUTION_POLICY_LITERAL_READS[:-1]):
        missing_literal_checks.classify(
            entry.relative,
            GetenvCall(line=200 + index, argument=f'"{entry.name}"'),
            categories,
        )
    missing_literal_checks.validate_legacy_execution_policy_literal_counts()
    missing_literal_entry = LEGACY_EXECUTION_POLICY_LITERAL_READS[-1]
    expect(
        len(missing_literal_checks.errors) == 1
        and missing_literal_entry.name in missing_literal_checks.errors[0]
        and "found 0" in missing_literal_checks.errors[0],
        f"missing legacy execution-policy literal read was not rejected exactly: "
        f"{missing_literal_checks.errors}",
    )

    wrong_file_literal_checks = Checks(Path("."))
    wrong_file_entry = LEGACY_EXECUTION_POLICY_LITERAL_READS[0]
    for index, entry in enumerate(LEGACY_EXECUTION_POLICY_LITERAL_READS[1:]):
        wrong_file_literal_checks.classify(
            entry.relative,
            GetenvCall(line=250 + index, argument=f'"{entry.name}"'),
            categories,
        )
    wrong_file_literal_checks.classify(
        "src/sieve/example.cpp",
        GetenvCall(line=299, argument=f'"{wrong_file_entry.name}"'),
        categories,
    )
    wrong_file_literal_checks.validate_legacy_execution_policy_literal_counts()
    expect(
        any(
            "is only allowed in" in error
            and wrong_file_entry.relative in error
            for error in wrong_file_literal_checks.errors
        )
        and any(
            wrong_file_entry.name in error and "found 0" in error
            for error in wrong_file_literal_checks.errors
        ),
        f"wrong-file legacy execution-policy literal read was not rejected: "
        f"{wrong_file_literal_checks.errors}",
    )

    identifier_snippet = r'''
// std::random_device ignored_comment;
const char* ignored_text = "random_device";
std::random_device actual;
int random_device_suffix = 0;
'''
    identifier_uses = find_code_identifier_uses(identifier_snippet, "random_device")
    expect(
        len(identifier_uses) == 1 and identifier_uses[0].line == 4,
        f"random_device code-token scan failed: {identifier_uses}",
    )

    durable_checks = Checks(Path("."))
    durable_relative = sorted(DURABLE_ENVIRONMENT_FREE_FILES)[0]
    durable_checks.classify(
        durable_relative,
        GetenvCall(line=31, argument='"GNFS_LATTICE_LLL"'),
        categories,
    )
    expect(
        durable_checks.errors
        == [
            f"{durable_relative}:31: "
            "durable protocol/execution-policy implementation must not read process environment"
        ],
        "durable-path environment ban is not enforced",
    )
    durable_checks.classify_random_device(
        durable_relative, CodeIdentifierUse(line=37, offset=0)
    )
    expect(
        durable_checks.errors[-1]
        == f"{durable_relative}:37: "
        "durable protocol/execution-policy implementation must not use random_device",
        "durable-path random_device ban is not enforced",
    )

    mapper_relative = sorted(DURABLE_PURE_RUNTIME_MAPPER_FILES)[0]
    mapper_api_snippet = r'''
// LatticeSieve and thread::hardware_concurrency() are ignored here.
const char* ignored = "AdaptiveLatticeConfig{}.from_env()";
config.lattice_basis_parallel_threads = policy.lattice_basis_parallel_threads;
using std::thread;
auto host = thread::hardware_concurrency();
auto config = AdaptiveLatticeConfig{}.from_env();
LatticeSieve sieve(context, factor_base);
auto basis = compute_lattice_basis_with_skewness(sq, skewness);
auto threads = lattice_basis_parallel_threads();
auto ambient_brent = brent_pollard_enabled();
auto ambient_threshold = survival_threshold();
auto legacy_classification = classify_cofactor(cofactor, bound);
auto direct_ecm = ECM::quick_factor(cofactor);
auto seeded_classification = classify_cofactor_seeded_v1(
    cofactor, bound, false, 0, coordinates, side, provider);
auto seeded_brent_classification = classify_cofactor_seeded_with_brent_v1(
    cofactor, bound, false, 0, coordinates, side, provider, 0, false, 0);
'''
    mapper_api_checks = Checks(Path("."))
    mapper_api_checks.validate_durable_ambient_api_uses(
        mapper_relative, mapper_api_snippet
    )
    expect(
        len(mapper_api_checks.errors) == 11
        and any(
            "ambient API hardware_concurrency" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "ambient API from_env" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API LatticeSieve" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API compute_lattice_basis_with_skewness" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "must not call legacy runtime API lattice_basis_parallel_threads"
            in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API quick_factor" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API classify_cofactor_seeded_v1" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API classify_cofactor_seeded_with_brent_v1" in error
            for error in mapper_api_checks.errors
        ),
        f"durable runtime-mapper ambient API bans are not closed: "
        f"{mapper_api_checks.errors}",
    )

    provider_api_checks = Checks(Path("."))
    provider_api_checks.validate_durable_ambient_api_uses(
        "src/sieve/distributed_sieve_seed_v2.cpp",
        "auto enabled = brent_pollard_enabled();",
    )
    expect(
        len(provider_api_checks.errors) == 1
        and "legacy runtime API brent_pollard_enabled"
        in provider_api_checks.errors[0],
        "distributed seed-provider indirect ambient API ban is not enforced",
    )

    bound_work_checks = Checks(Path("."))
    bound_work_relative = "src/sieve/distributed_sieve_bound_work.cpp"
    bound_work_checks.validate_durable_ambient_api_uses(
        bound_work_relative,
        r'''
DistributedSieveWaveStore store;
DistributedSieveWorkerAttemptStartReceipt receipt;
RelationCollector collector;
auto pid = process_id();
auto rows = run_distributed_sieve(config);
auto fork_pointer = &fork;
auto descriptor = open(path, flags);
std::ofstream output(path);
auto now = std::chrono::steady_clock::now();
''',
    )
    expect(
        len(bound_work_checks.errors) == 9
        and any(
            "runtime/side-effect API DistributedSieveWaveStore" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API DistributedSieveWorkerAttemptStartReceipt" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API RelationCollector" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API process_id" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API run_distributed_sieve" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API fork" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API open" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API ofstream" in error
            for error in bound_work_checks.errors
        )
        and any(
            "runtime/side-effect API steady_clock" in error
            for error in bound_work_checks.errors
        ),
        f"bound-work authority/process bans are not closed: "
        f"{bound_work_checks.errors}",
    )

    use_site_checks = Checks(Path("."))
    use_site_checks.validate_bound_work_use_site(
        "src/sieve/untrusted_wrapper.mm",
        r'''
auto bound = bind_distributed_sieve_work_v1(identity, frozen, context, factor_base);
auto rows = run_distributed_sieve(config, context, factor_base, bound.sieve_parameters);
''',
    )
    expect(
        len(use_site_checks.errors) == 2
        and any(
            "use site is not receipt-gated/allowlisted" in error
            for error in use_site_checks.errors
        )
        and any(
            "must not coexist with the legacy seeded runner" in error
            for error in use_site_checks.errors
        ),
        f"bound-work repo-wide use-site gate is not enforced: "
        f"{use_site_checks.errors}",
    )
    expect(
        ".mm" in SOURCE_SUFFIXES,
        "Objective-C++ sources are missing from the repo-wide bound-work scan",
    )
    repository_inventory = Checks(
        Path(__file__).resolve().parents[1]
    ).bound_work_source_files()
    inventory_top_levels = {
        Path(relative).parts[0] for relative, _ in repository_inventory
    }
    expect(
        {"bench", "include", "src", "tests"} <= inventory_top_levels
        and any(relative == "src/linalg/metal_spmv.mm" for relative, _ in repository_inventory),
        "repo-wide bound-work scan misses a current source-bearing top level or Objective-C++ file",
    )
    allowed_use_site_checks = Checks(Path("."))
    allowed_use_site_checks.validate_bound_work_use_site(
        "tests/test_distributed_sieve_execution_policy.cpp",
        "auto bound = bind_distributed_sieve_work_v1(identity, frozen, context, factor_base);",
    )
    expect(
        not allowed_use_site_checks.errors,
        f"allowlisted bound-work test use was rejected: "
        f"{allowed_use_site_checks.errors}",
    )

    carrier_use_site_checks = Checks(Path("."))
    carrier_use_site_checks.validate_work_package_carrier_use_site(
        "src/sieve/untrusted_launcher.cpp",
        r'''
DistributedSieveWorkerWorkPackageFileV1* token = nullptr;
auto result = create_distributed_sieve_worker_work_package_file_v1(request, identity);
''',
    )
    expect(
        len(carrier_use_site_checks.errors) == 2
        and all(
            "carrier authority is not receipt-gated/allowlisted" in error
            for error in carrier_use_site_checks.errors
        ),
        "anonymous work-package carrier use-site gate is not enforced",
    )
    allowed_carrier_checks = Checks(Path("."))
    allowed_carrier_checks.validate_work_package_carrier_use_site(
        "tests/test_distributed_sieve_worker_work_package_file.cpp",
        "auto result = "
        "create_distributed_sieve_worker_work_package_file_v1(request, identity);",
    )
    expect(
        not allowed_carrier_checks.errors,
        f"allowlisted work-package carrier test use was rejected: "
        f"{allowed_carrier_checks.errors}",
    )

    fixed_capability_use_site_checks = Checks(Path("."))
    fixed_capability_use_site_checks.validate_worker_process_fixed_capability_use_site(
        "src/sieve/untrusted_launcher.cpp",
        r'''
DistributedSieveWorkerProcessFixedCapabilitySourcesV1 sources;
auto result = spawn_distributed_sieve_worker_process_batch_with_capabilities(
    request, sources);
''',
    )
    expect(
        len(fixed_capability_use_site_checks.errors) == 2
        and all(
            "fixed-capability worker-process API use site is not allowlisted" in error
            for error in fixed_capability_use_site_checks.errors
        ),
        "fixed-capability worker-process repo-wide use-site gate is not enforced",
    )
    allowed_fixed_capability_snippet = r'''
DistributedSieveWorkerProcessFixedCapabilitySourcesV1 sources;
auto result = spawn_distributed_sieve_worker_process_batch_with_capabilities(
    request, sources);
'''
    for relative in sorted(WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_ALLOWLIST):
        allowed_fixed_capability_checks = Checks(Path("."))
        allowed_fixed_capability_checks.validate_worker_process_fixed_capability_use_site(
            relative, allowed_fixed_capability_snippet
        )
        expect(
            not allowed_fixed_capability_checks.errors,
            f"allowlisted fixed-capability worker-process use was rejected in "
            f"{relative}: {allowed_fixed_capability_checks.errors}",
        )

    pipeline_checks = Checks(Path("."))
    pipeline_checks.validate_legacy_pipeline_boundary(
        r'''
// bind_distributed_sieve_work_v1 is ignored in comments.
const char* ignored = "DistributedSieveWorkIdentityV1";
DistributedSieveWorkIdentityV1 identity;
auto bound = bind_distributed_sieve_work_v1(identity, frozen, context, factor_base);
'''
    )
    expect(
        len(pipeline_checks.errors) == 2
        and any(
            "DistributedSieveWorkIdentityV1" in error
            for error in pipeline_checks.errors
        )
        and any(
            "bind_distributed_sieve_work_v1" in error
            for error in pipeline_checks.errors
        ),
        f"legacy Pipeline durable self-mint ban is not enforced: "
        f"{pipeline_checks.errors}",
    )

    legacy_random_checks = Checks(Path("."))
    legacy_random_relative, legacy_random_expected = next(
        iter(LEGACY_RANDOM_DEVICE_USES.items())
    )
    for index in range(legacy_random_expected):
        legacy_random_checks.classify_random_device(
            legacy_random_relative, CodeIdentifierUse(line=50 + index, offset=index)
        )
    expect(
        not legacy_random_checks.errors
        and legacy_random_checks.random_device_counts[legacy_random_relative]
        == legacy_random_expected,
        "legacy random_device uses were not preserved by an exact count allowlist",
    )
    legacy_random_checks.classify_random_device(
        legacy_random_relative, CodeIdentifierUse(line=60, offset=99)
    )
    expect(
        len(legacy_random_checks.errors) == 1
        and legacy_random_checks.errors[0].startswith(
            f"{legacy_random_relative}:60: legacy random_device allowlist"
        ),
        "additional legacy-file random_device use was not rejected",
    )
    unknown_random_checks = Checks(Path("."))
    unknown_random_checks.classify_random_device(
        "src/sieve/example.cpp", CodeIdentifierUse(line=41, offset=0)
    )
    expect(
        unknown_random_checks.errors
        == [
            "src/sieve/example.cpp:41: "
            "random_device use is not in the exact legacy allowlist"
        ],
        "new random_device use outside the legacy allowlist was not rejected",
    )

    adapter_lines = [
        (
            "auto "
            + EXECUTION_POLICY_ENVIRONMENT_CAPTURE
            + "() noexcept {"
        ),
        *[
            f'  owned_environment_value(std::getenv("{flag}"));'
            for flag in EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS
        ],
        (
            "  const auto host = "
            + EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
            + "();"
        ),
        "}",
    ]
    adapter_source = "\n".join(adapter_lines)
    adapter_calls, adapter_parse_errors = find_getenv_calls(adapter_source)
    expect(
        not adapter_parse_errors and len(adapter_calls) == 32,
        f"synthetic adapter getenv parsing failed: {adapter_parse_errors}",
    )
    adapter_checks = Checks(Path("."))
    adapter_checks.validate_environment_adapter(adapter_source, adapter_calls)
    for call in adapter_calls:
        adapter_checks.classify(
            EXECUTION_POLICY_ENVIRONMENT_ADAPTER, call, categories
        )
    expect(
        not adapter_checks.errors
        and all(count == 1 for count in adapter_checks.adapter_counts.values()),
        f"valid 31+1 descriptor capture was rejected: {adapter_checks.errors}",
    )

    duplicate_lines = list(adapter_lines)
    duplicate_lines[-3] = (
        '  owned_environment_value(std::getenv("'
        + EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS[0]
        + '"));'
    )
    duplicate_source = "\n".join(duplicate_lines)
    duplicate_calls, duplicate_parse_errors = find_getenv_calls(duplicate_source)
    duplicate_checks = Checks(Path("."))
    duplicate_checks.validate_environment_adapter(duplicate_source, duplicate_calls)
    expect(
        not duplicate_parse_errors
        and any("found 2" in error for error in duplicate_checks.errors)
        and any("found 0" in error for error in duplicate_checks.errors),
        "duplicate/missing descriptor capture flags were not rejected",
    )

    reordered_lines = list(adapter_lines)
    reordered_lines[1], reordered_lines[2] = reordered_lines[2], reordered_lines[1]
    reordered_source = "\n".join(reordered_lines)
    reordered_calls, reordered_parse_errors = find_getenv_calls(reordered_source)
    reordered_checks = Checks(Path("."))
    reordered_checks.validate_environment_adapter(reordered_source, reordered_calls)
    expect(
        not reordered_parse_errors
        and any("not in canonical key order" in error for error in reordered_checks.errors),
        "out-of-order descriptor capture flags were not rejected",
    )

    missing_host_lines = [
        line
        for line in adapter_lines
        if EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER not in line
    ]
    missing_host_source = "\n".join(missing_host_lines)
    missing_host_calls, missing_host_parse_errors = find_getenv_calls(
        missing_host_source
    )
    missing_host_checks = Checks(Path("."))
    missing_host_checks.validate_environment_adapter(
        missing_host_source, missing_host_calls
    )
    expect(
        not missing_host_parse_errors
        and any(
            "must contain exactly one" in error and "found 0" in error
            for error in missing_host_checks.errors
        ),
        "missing production-capture hardware_concurrency identifier was not rejected",
    )

    duplicate_host_lines = list(adapter_lines)
    duplicate_host_lines.insert(
        -1,
        "  const auto host_again = "
        + EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
        + "();",
    )
    duplicate_host_source = "\n".join(duplicate_host_lines)
    duplicate_host_calls, duplicate_host_parse_errors = find_getenv_calls(
        duplicate_host_source
    )
    duplicate_host_checks = Checks(Path("."))
    duplicate_host_checks.validate_environment_adapter(
        duplicate_host_source, duplicate_host_calls
    )
    expect(
        not duplicate_host_parse_errors
        and any(
            "must contain exactly one" in error and "found 2" in error
            for error in duplicate_host_checks.errors
        ),
        "duplicate production-capture hardware_concurrency identifier was not rejected",
    )

    aliased_host_lines = [
        (
            "  const auto host_reader = "
            + EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
            + "; const auto host = host_reader();"
            if EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER in line
            else line
        )
        for line in adapter_lines
    ]
    aliased_host_source = "\n".join(aliased_host_lines)
    aliased_host_calls, aliased_host_parse_errors = find_getenv_calls(
        aliased_host_source
    )
    aliased_host_checks = Checks(Path("."))
    aliased_host_checks.validate_environment_adapter(
        aliased_host_source, aliased_host_calls
    )
    expect(
        not aliased_host_parse_errors
        and any(
            "must be used as a direct call" in error
            for error in aliased_host_checks.errors
        ),
        "aliased production-capture hardware_concurrency identifier was not rejected",
    )

    outside_host_source = (
        adapter_source
        + "\nconst auto outside_host = "
        + EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER
        + "();\n"
    )
    outside_host_calls, outside_host_parse_errors = find_getenv_calls(
        outside_host_source
    )
    outside_host_checks = Checks(Path("."))
    outside_host_checks.validate_environment_adapter(
        outside_host_source, outside_host_calls
    )
    expect(
        not outside_host_parse_errors
        and any(
            "identifiers must be inside" in error
            for error in outside_host_checks.errors
        ),
        "adapter-file hardware_concurrency identifier outside capture was not rejected",
    )

    dynamic_adapter_checks = Checks(Path("."))
    dynamic_adapter_checks.classify(
        EXECUTION_POLICY_ENVIRONMENT_ADAPTER,
        GetenvCall(line=71, argument="descriptor.environment_name"),
        categories,
    )
    expect(
        dynamic_adapter_checks.errors
        == [
            f"{EXECUTION_POLICY_ENVIRONMENT_ADAPTER}:71: "
            "execution-policy environment adapter requires a literal GNFS flag, "
            "found 'descriptor.environment_name'"
        ],
        "dynamic environment-adapter getenv argument was not rejected",
    )
    return errors


def print_inventory(reads: list[ClassifiedRead]) -> None:
    for category in (*CATEGORY_FLAGS.keys(), "legacy-test-only"):
        category_reads = [read for read in reads if read.category == category]
        names = sorted({read.name for read in category_reads})
        print(f"[INFO] {category}: {len(names)} settings, {len(category_reads)} reads")
        for read in category_reads:
            print(f"  {read.name} -> {read.relative}:{read.line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quiet", action="store_true", help="print only failures")
    parser.add_argument("--root", type=Path, help="repository root (defaults to this script's parent)")
    parser.add_argument("--self-test", action="store_true", help="run parser/table self-tests first")
    args = parser.parse_args()

    if args.self_test:
        self_test_errors = run_self_test()
        if self_test_errors:
            for error in self_test_errors:
                print(
                    f"[FAIL] scripts/check_distributed_sieve_policy.py:1: self-test: {error}",
                    file=sys.stderr,
                )
            return 1
        if not args.quiet:
            print("[PASS] distributed-sieve policy checker self-test")

    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    checks = Checks(root)
    errors = checks.run()
    if errors:
        for error in errors:
            print(f"[FAIL] {error}", file=sys.stderr)
        return 1
    if not args.quiet:
        print_inventory(checks.reads)
        print("[PASS] distributed-sieve GNFS environment inventory is closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
