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

WORKER_EXECUTOR_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_worker_execution.cpp"
)
WORKER_EXECUTOR_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_worker_execution_internal.hpp"
)
WORKER_EXECUTOR_TEST_FILE = "tests/test_distributed_sieve_worker_execution.cpp"
WORKER_EXECUTOR_RUNTIME_FILES = {
    "src/sieve/distributed_sieve_worker_runtime.cpp",
    "src/sieve/distributed_sieve_worker_runtime_internal.hpp",
}
WORKER_EXECUTOR_CHUNK_FILES = {
    "src/sieve/distributed_sieve_worker_chunk.cpp",
    "src/sieve/distributed_sieve_worker_chunk_internal.hpp",
}
WORKER_EXECUTOR_CAPABILITY_USE_SITE_FILES = {
    WORKER_EXECUTOR_IMPLEMENTATION_FILE,
    WORKER_EXECUTOR_INTERFACE_FILE,
    WORKER_EXECUTOR_TEST_FILE,
}
WORKER_EXECUTOR_DURABLE_FILES = (
    WORKER_EXECUTOR_RUNTIME_FILES
    | WORKER_EXECUTOR_CHUNK_FILES
    | {
        WORKER_EXECUTOR_IMPLEMENTATION_FILE,
        WORKER_EXECUTOR_INTERFACE_FILE,
    }
)
WORKER_EXECUTOR_BOUND_WORK_USE_SITE_FILES = (
    WORKER_EXECUTOR_RUNTIME_FILES | WORKER_EXECUTOR_CHUNK_FILES
)

MERGE_COORDINATOR_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_merge_coordinator.cpp"
)
MERGE_COORDINATOR_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_merge_coordinator.hpp"
)
MERGE_COORDINATOR_TEST_FILE = "tests/test_distributed_sieve_resume.cpp"
MERGE_COORDINATOR_PRODUCTION_FILES = {
    MERGE_COORDINATOR_IMPLEMENTATION_FILE,
    MERGE_COORDINATOR_INTERFACE_FILE,
}
MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_merge_writer_authority.cpp"
)
MERGE_WRITER_AUTHORITY_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_merge_writer_authority_internal.hpp"
)
MERGE_WRITER_AUTHORITY_TEST_FILE = (
    "tests/test_distributed_sieve_merge_writer_authority.cpp"
)
MERGE_WRITER_AUTHORITY_PRODUCTION_FILES = {
    MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
}
MERGE_WRITER_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_merge_writer_internal.cpp"
)
MERGE_WRITER_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_merge_writer_internal.hpp"
)
MERGE_WRITER_EXACT_APPEND_BATCH_IDENTIFIER = "begin_exact_append_batch"
MERGE_WRITER_EXACT_APPEND_BATCH_ALLOWLIST = {
    "include/gnfs/relation/ooc_relation_store.hpp",
    MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
}
MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS = {
    "DistributedSieveMergeWriterTestHooksV1": {
        MERGE_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
    },
    "stream_distributed_sieve_merge_inputs_v1_with_hooks": {
        MERGE_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "after_output_write": {
        MERGE_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_TEST_FILE,
    },
    "DistributedSieveMergeWriterAdoptionTestHooksV1": {
        MERGE_COORDINATOR_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_TEST_FILE,
    },
    "consume_distributed_sieve_merge_generation_v1_with_hooks": {
        MERGE_COORDINATOR_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_TEST_FILE,
    },
    "DistributedSieveMergePreparedPublicationTestHooksV1": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_TEST_FILE,
    },
    "publish_distributed_sieve_merge_prepared_v1_with_hooks": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
        MERGE_WRITER_AUTHORITY_TEST_FILE,
    },
}

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
    MERGE_COORDINATOR_IMPLEMENTATION_FILE,
    MERGE_COORDINATOR_INTERFACE_FILE,
    "src/sieve/distributed_sieve_merge_writer_internal.cpp",
    "src/sieve/distributed_sieve_merge_writer_internal.hpp",
    "src/sieve/distributed_sieve_merge_writer_codec_internal.cpp",
    "src/sieve/distributed_sieve_merge_writer_codec_internal.hpp",
    MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
    "src/sieve/distributed_sieve_wave_store.cpp",
    "src/sieve/distributed_sieve_wave_store_internal.hpp",
    "src/sieve/distributed_sieve_worker_coordinator.cpp",
    "src/sieve/distributed_sieve_worker_coordinator_internal.hpp",
    "src/sieve/distributed_sieve_worker_entry.cpp",
    "src/sieve/distributed_sieve_worker_entry_internal.hpp",
    "src/sieve/distributed_sieve_worker_writer.cpp",
    "src/sieve/distributed_sieve_worker_writer_internal.hpp",
    "src/sieve/distributed_sieve_worker_launcher_fwd_internal.hpp",
    "src/sieve/distributed_sieve_worker_launcher_internal.hpp",
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "src/sieve/distributed_sieve_worker_work_package_file_ops_internal.hpp",
    "src/sieve/distributed_sieve_worker_process.cpp",
    "src/sieve/distributed_sieve_worker_process_internal.hpp",
    "include/gnfs/sieve/distributed_sieve_seed_v2.hpp",
} | WORKER_EXECUTOR_DURABLE_FILES

WORKER_COORDINATOR_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_worker_coordinator.cpp"
)
WORKER_COORDINATOR_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_worker_coordinator_internal.hpp"
)
WORKER_COORDINATOR_TEST_FILE = "tests/test_distributed_sieve_resume.cpp"
WORKER_COORDINATOR_PRODUCTION_FILES = {
    WORKER_COORDINATOR_IMPLEMENTATION_FILE,
    WORKER_COORDINATOR_INTERFACE_FILE,
}
WORKER_COORDINATOR_USE_SITE_ALLOWLIST = (
    WORKER_COORDINATOR_PRODUCTION_FILES
    | MERGE_COORDINATOR_PRODUCTION_FILES
    | MERGE_WRITER_AUTHORITY_PRODUCTION_FILES
    | {WORKER_COORDINATOR_TEST_FILE, MERGE_WRITER_AUTHORITY_TEST_FILE}
)
WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_wave_store.cpp"
)
WORKER_ATTEMPT_TERMINAL_TRANSITION_FUNCTION = (
    "claim_worker_attempt_private_lease_root"
)
WORKER_ATTEMPT_TERMINAL_TRANSITION_HELPER = (
    "exact_worker_attempt_terminal_transition"
)
WORKER_ATTEMPT_TERMINAL_REFRESH_IDENTIFIER = (
    "refresh_exact_terminal_transition"
)
WORKER_ATTEMPT_TERMINAL_HELPER_CALL_FRAGMENT = (
    "exact_worker_attempt_terminal_transition("
    "before,observed,state_->manifest,*claim.worker_attempt_names_,"
    "chunk_id,attempt_ordinal,target_index)"
)
WORKER_ATTEMPT_TERMINAL_REFRESH_GUARD_FRAGMENT = (
    "if(terminal_transition_refreshed||"
    "expectation!=AttemptBaseLockExpectation::present"
)
WORKER_ATTEMPT_TERMINAL_REFRESH_SET_FRAGMENT = (
    "terminal_transition_refreshed=true"
)
WORKER_ATTEMPT_IMMEDIATE_REFRESH_FRAGMENT = (
    "refresh_exact_terminal_transition(immediately_before)"
)
WORKER_ATTEMPT_TARGET_CREATE_FRAGMENT = (
    "target=DistributedSievePrivateLeaseBaseLockAt::create_new_locked("
)
WORKER_ATTEMPT_TARGET_OPEN_FRAGMENT = (
    "target=DistributedSievePrivateLeaseBaseLockAt::open_existing_locked("
)
WORKER_ATTEMPT_HELD_REFRESH_FRAGMENT = (
    "refresh_exact_terminal_transition(held_target)"
)
WORKER_ATTEMPT_HELD_CAPTURE_FRAGMENT = (
    "autoheld_target=capture_manifest_bound_inventory_witness("
    "state_->root_fd,state_->manifest,state_->absolute_root,"
    "state_->creator_process_id,held_inventory())"
)
WORKER_ATTEMPT_HELD_CONFIRM_CAPTURE_FRAGMENT = (
    "autoheld_target_confirmed=capture_manifest_bound_inventory_witness("
    "state_->root_fd,state_->manifest,state_->absolute_root,"
    "state_->creator_process_id,held_inventory())"
)
WORKER_ATTEMPT_HELD_CONFIRM_MATCH_FRAGMENT = (
    "!held_target_confirmed||!expected_successor_matches(held_target_confirmed)"
)
WORKER_ATTEMPT_AFTER_TARGET_HOOK_FRAGMENT = (
    "hooks.after_target_lock_acquired,hooks.context,state_->creator_process_id"
)
WORKER_ATTEMPT_TARGET_TRANSFER_FRAGMENT = (
    "claim.base_lock_at_=std::move(target)"
)
WORKER_COORDINATOR_USE_SITE_IDENTIFIERS = (
    "distributed_sieve_worker_coordinator_detail",
    "DistributedSieveWorkerCoordinationDispositionV1",
    "DistributedSieveWorkerCoordinatorPhaseV1",
    "DistributedSieveWorkerCoordinatorStatusV1",
    "DistributedSieveWorkerCoordinatorDiagnosticV1",
    "DistributedSieveWorkerCoordinatorTestHooksV1",
    "DistributedSieveWorkerCoordinatorRequestV1",
    "DistributedSieveWorkerCoordinatedChunkV1",
    "DistributedSieveWorkerCoordinatorResultV1",
    "coordinate_missing_distributed_sieve_workers_v1",
)
WORKER_COORDINATOR_COMPOSITION_FUNCTION = (
    "coordinate_missing_distributed_sieve_workers_v1"
)
WORKER_COORDINATOR_BOUND_WORK_IDENTIFIER = "bind_distributed_sieve_work_v1"
WORKER_COORDINATOR_SEALED_LAUNCHER_IDENTIFIER = (
    "launch_worker_process_batch_v1"
)
WORKER_COORDINATOR_SEALED_LAUNCHER_FRAGMENT = (
    "result.store->launch_worker_process_batch_v1("
)
WORKER_COORDINATOR_ATTEMPT_OPEN_IDENTIFIER = (
    "open_worker_attempt_private_lease_root"
)
WORKER_COORDINATOR_ATTEMPT_OPEN_FRAGMENT = (
    "result.store->open_worker_attempt_private_lease_root("
    "initial_attempt.chunk_id,initial_attempt.attempt_ordinal)"
)
WORKER_COORDINATOR_ATTEMPT_RECONCILE_IDENTIFIER = (
    "reconcile_worker_attempt_started"
)
WORKER_COORDINATOR_ATTEMPT_RECONCILE_FRAGMENT = (
    "resume::reconcile_worker_attempt_started(std::move(opened))"
)
WORKER_COORDINATOR_TERMINAL_FAILURE_PUBLISH_IDENTIFIER = (
    "publish_chunk_terminal_failure_v1"
)
WORKER_COORDINATOR_TERMINAL_FAILURE_PUBLISH_FRAGMENT = (
    "resume::publish_chunk_terminal_failure_v1("
    "std::move(*reconciled.terminal_failure_admission))"
)
WORKER_COORDINATOR_EXPECTED_ADOPTION_IDENTIFIER = (
    "adopt_expected_worker_handoff_v1"
)
WORKER_COORDINATOR_ORDINARY_ADOPTION_IDENTIFIER = (
    "adopt_worker_handoff_v1"
)
WORKER_COORDINATOR_EXPECTED_ADOPTION_FRAGMENT = (
    "result.store->adopt_expected_worker_handoff_v1("
    "*expected_adopted_witnesses[index])"
)
WORKER_COORDINATOR_TERMINAL_BINDING_FRAGMENT = (
    "constauto&terminal=*reconciled.terminal_handoff"
)
WORKER_COORDINATOR_TERMINAL_WITNESS_STORE_FRAGMENT = (
    "expected_adopted_witnesses[manifest_slot]=terminal"
)
WORKER_COORDINATOR_TERMINAL_ADOPTION_DISPATCH_FRAGMENT = (
    "autoadoption=expected_adopted_witnesses[index].has_value()"
    "?result.store->adopt_expected_worker_handoff_v1("
    "*expected_adopted_witnesses[index])"
    ":result.store->adopt_worker_handoff_v1(manifest.chunks[index].chunk_id)"
)
ALTERNATE_PROCESS_EXECUTION_IDENTIFIERS = (
    "system",
    "popen",
    "_popen",
    "execl",
    "execle",
    "execlp",
    "execv",
    "execve",
    "execveat",
    "execvp",
    "fexecve",
    "_execl",
    "_execle",
    "_execlp",
    "_execlpe",
    "_execv",
    "_execve",
    "_execvp",
    "_execvpe",
    "_spawnl",
    "_spawnle",
    "_spawnlp",
    "_spawnlpe",
    "_spawnv",
    "_spawnve",
    "_spawnvp",
    "_spawnvpe",
    "clone",
    "clone3",
    "forkpty",
    "CreateProcess",
    "CreateProcessA",
    "CreateProcessW",
    "ShellExecute",
    "ShellExecuteA",
    "ShellExecuteW",
    "WinExec",
)
WORKER_COORDINATOR_FORBIDDEN_IDENTIFIERS = {
    "_Fork",
    "vfork",
    "fork",
    "posix_spawn",
    "posix_spawnp",
    "waitpid",
    "waitid",
    "wait3",
    "wait4",
    "spawn_distributed_sieve_worker_process_batch_with_capabilities",
    "recover_worker_attempt_private_lease",
    "ChunkTerminalFailureV1",
    "publish_at",
    "run_distributed_sieve",
    "DistributedSieveWorkerResult",
} | set(ALTERNATE_PROCESS_EXECUTION_IDENTIFIERS)
WORKER_COORDINATOR_FORBIDDEN_IDENTIFIER_FRAGMENTS = (
    "cleanup",
    "unlink",
)
WORKER_COORDINATOR_AUTHORITY_FREE_CLEANUP_FACTS = {
    "cleanup_intent_absent",
}
WORKER_COORDINATOR_LEGACY_PUBLIC_HEADER = "gnfs/sieve/distributed_sieve.hpp"
PUBLIC_SIEVE_HEADER_PREFIX = "include/gnfs/sieve/"

MERGE_COORDINATOR_USE_SITE_ALLOWLIST = (
    MERGE_COORDINATOR_PRODUCTION_FILES
    | MERGE_WRITER_AUTHORITY_PRODUCTION_FILES
    | {MERGE_COORDINATOR_TEST_FILE, MERGE_WRITER_AUTHORITY_TEST_FILE}
)
MERGE_COORDINATOR_USE_SITE_IDENTIFIERS = (
    "distributed_sieve_merge_coordinator_detail",
    "DistributedSieveMergeGenerationAdmissionV1",
    "begin_or_resume_distributed_sieve_merge_generation_v1",
)
MERGE_COORDINATOR_COMPOSITION_FUNCTION = (
    "begin_or_resume_distributed_sieve_merge_generation_v1"
)
MERGE_COORDINATOR_ADMISSION_IDENTIFIER = (
    "DistributedSieveMergeGenerationAdmissionV1"
)
MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER = (
    "DistributedSieveWorkerCoordinatorResultV1"
)
MERGE_COORDINATOR_WORKER_RESULT_PARAMETER = "worker_result"
MERGE_COORDINATOR_RESERVE_IDENTIFIER = (
    "reserve_distributed_sieve_merge_generation_v1"
)
MERGE_COORDINATOR_PUBLISH_IDENTIFIER = "publish_merge_started_v1"
MERGE_GENERATION_AUTHORITY_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_wave_store.cpp",
    "src/sieve/distributed_sieve_wave_store_internal.hpp",
    MERGE_COORDINATOR_IMPLEMENTATION_FILE,
    MERGE_COORDINATOR_TEST_FILE,
}
MERGE_GENERATION_AUTHORITY_USE_SITE_IDENTIFIERS = (
    MERGE_COORDINATOR_RESERVE_IDENTIFIER,
    MERGE_COORDINATOR_PUBLISH_IDENTIFIER,
)
MERGE_COORDINATOR_FORBIDDEN_EXACT_IDENTIFIERS = {
    "filesystem",
    "path",
    "u8path",
    "unlink",
    "unlinkat",
    "remove",
    "remove_all",
    "rmdir",
}
MERGE_COORDINATOR_FORBIDDEN_IDENTIFIER_SEGMENTS = {
    "cleanup",
    "deletion",
    "unlink",
    "remove",
    "removal",
}
MERGE_COORDINATOR_RECORD_IDENTIFIER = "MergeStartedV1"

WORKER_LAUNCHER_IMPLEMENTATION_FILE = "src/sieve/distributed_sieve_wave_store.cpp"
WORKER_LAUNCHER_INTERFACE_FILES = {
    "src/sieve/distributed_sieve_wave_store_internal.hpp",
    "src/sieve/distributed_sieve_worker_launcher_fwd_internal.hpp",
    "src/sieve/distributed_sieve_worker_launcher_internal.hpp",
}
WORKER_LAUNCHER_TEST_FILES = {
    "tests/test_distributed_sieve_resume.cpp",
    "tests/test_distributed_sieve_worker_entry.cpp",
    "tests/test_distributed_sieve_worker_writer_authority.cpp",
}
WORKER_LAUNCHER_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerLaunchSlotV1",
    "DistributedSieveWorkerLauncherTestHooksV1",
    "DistributedSieveWorkerLaunchRequestV1",
    "DistributedSieveLaunchedWorkerAttemptV1",
    "DistributedSieveWorkerLaunchPhaseV1",
    "DistributedSieveWorkerLaunchDispositionV1",
    "DistributedSieveWorkerLaunchDiagnosticV1",
    "DistributedSieveWorkerLaunchChildResultV1",
    "DistributedSieveWorkerLaunchBatchResultV1",
    "launch_worker_process_batch_v1",
)
WORKER_LAUNCHER_USE_SITE_ALLOWLIST = (
    WORKER_LAUNCHER_INTERFACE_FILES
    | {WORKER_LAUNCHER_IMPLEMENTATION_FILE}
    | WORKER_COORDINATOR_PRODUCTION_FILES
    | WORKER_LAUNCHER_TEST_FILES
)
WORKER_LAUNCHER_COMPOSITION_FUNCTION = "launch_worker_process_batch_v1"
WORKER_LAUNCHER_COMPOSITION_USE_COUNTS = {
    "bind_distributed_sieve_work_v1": 1,
    "DistributedSieveWorkerWorkPackageFileV1": 1,
    "create_distributed_sieve_worker_work_package_file_v1": 1,
    "DistributedSieveWorkerProcessFixedCapabilitySourcesV1": 1,
    "spawn_distributed_sieve_worker_process_batch_with_capabilities": 1,
    "retained_reader_": 3,
}
WORKER_LAUNCHER_COMPOSITION_DIRECT_CALL_IDENTIFIERS = {
    "bind_distributed_sieve_work_v1",
    "create_distributed_sieve_worker_work_package_file_v1",
    "spawn_distributed_sieve_worker_process_batch_with_capabilities",
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
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
}
WORKER_ENTRY_IMPLEMENTATION_FILE = "src/sieve/distributed_sieve_worker_entry.cpp"
WORKER_ENTRY_INTERFACE_FILE = "src/sieve/distributed_sieve_worker_entry_internal.hpp"
WORKER_ENTRY_TEST_FILE = "tests/test_distributed_sieve_worker_entry.cpp"
WORKER_WRITER_IMPLEMENTATION_FILE = "src/sieve/distributed_sieve_worker_writer.cpp"
WORKER_WRITER_INTERFACE_FILE = "src/sieve/distributed_sieve_worker_writer_internal.hpp"
WORKER_WRITER_TEST_FILE = "tests/test_distributed_sieve_worker_writer_authority.cpp"
WORKER_ENTRY_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerEntryV1",
    "DistributedSieveWorkerEntryAdoptionResultV1",
    "DistributedSieveWorkerEntryTestHooksV1",
    "adopt_distributed_sieve_worker_entry_v1",
    "adopt_distributed_sieve_worker_entry_v1_with_hooks",
)
WORKER_ENTRY_USE_SITE_ALLOWLIST = {
    WORKER_ENTRY_IMPLEMENTATION_FILE,
    WORKER_ENTRY_INTERFACE_FILE,
    WORKER_ENTRY_TEST_FILE,
    WORKER_WRITER_IMPLEMENTATION_FILE,
    WORKER_WRITER_INTERFACE_FILE,
    WORKER_WRITER_TEST_FILE,
    WORKER_COORDINATOR_TEST_FILE,
} | WORKER_EXECUTOR_CAPABILITY_USE_SITE_FILES
WORKER_WRITER_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerCompletionFactsV1",
    "DistributedSieveWorkerWriterAuthorityV1",
    "DistributedSieveWorkerWriterAdoptionResultV1",
    "DistributedSieveWorkerHandoffTestHooksV1",
    "DistributedSieveWorkerWriterTestHooksV1",
    "DistributedSieveWorkerWriterRollbackV1",
    "consume_distributed_sieve_worker_writer_v1",
    "consume_distributed_sieve_worker_writer_v1_with_hooks",
    "finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks",
    "finalize_and_publish_handoff",
)
WORKER_WRITER_AUTHORITY_IDENTIFIER = "DistributedSieveWorkerWriterAuthorityV1"
WORKER_WRITER_AUTHORITY_EXCEPTION_FILE = (
    "include/gnfs/relation/ooc_relation_store.hpp"
)
WORKER_WRITER_USE_SITE_ALLOWLIST = {
    WORKER_ENTRY_IMPLEMENTATION_FILE,
    WORKER_ENTRY_INTERFACE_FILE,
    WORKER_WRITER_IMPLEMENTATION_FILE,
    WORKER_WRITER_INTERFACE_FILE,
    WORKER_WRITER_TEST_FILE,
} | WORKER_EXECUTOR_CAPABILITY_USE_SITE_FILES
WORKER_EXECUTOR_COMPOSITION_FUNCTION = (
    "execute_distributed_sieve_worker_entry_v1"
)
WORKER_EXECUTOR_COMPOSITION_CALL_ORDER = (
    "rehydrate_distributed_sieve_worker_runtime_v1",
    "prepare_distributed_sieve_worker_chunk_v1",
    "consume_distributed_sieve_worker_writer_v1",
    "finalize_and_publish_handoff",
)
WORKER_EXECUTOR_COMPOSITION_USE_COUNTS = {
    identifier: 1 for identifier in WORKER_EXECUTOR_COMPOSITION_CALL_ORDER
}
WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_IDENTIFIERS = (
    "RelationCollector",
    "finalize_and_publish_handoff_impl",
    "finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks",
    "finalize_and_publish_private_handoff",
    "finalize_and_publish_private_handoff_built",
    "getenv",
    "hardware_concurrency",
)
WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_PREFIXES = ("run_distributed_sieve",)
WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_FRAGMENTS = ("cleanup",)
WORKER_WRITER_IDENTIFIER_EXCEPTIONS = {
    WORKER_WRITER_AUTHORITY_IDENTIFIER: {
        WORKER_WRITER_AUTHORITY_EXCEPTION_FILE,
    },
}
WORKER_WRITER_BRIDGE_IDENTIFIERS = (
    "DistributedSieveWorkerWriterLifetimeGuardV1",
    "OOCInheritedP8WriterMintV1",
    "OOCExactFreshConstructionFailure",
    "OOCExactFreshRollbackDisposition",
    "AdoptInheritedOpenFileDescription",
    "ExactPrivateDirectoryBinding",
    "ExactPrivateDirectoryConstructionToken",
    "discard_and_close_post_fork_child_noexcept",
    "discard_inherited_post_fork_child_noexcept",
)
WORKER_WRITER_BRIDGE_ALLOWLIST = {
    "include/gnfs/relation/ooc_cleanup_transaction.hpp",
    "include/gnfs/relation/ooc_relation_store.hpp",
    "include/gnfs/util/native_binary_update_file.hpp",
    WORKER_ENTRY_IMPLEMENTATION_FILE,
    WORKER_WRITER_IMPLEMENTATION_FILE,
    WORKER_WRITER_INTERFACE_FILE,
}
WORKER_WRITER_BRIDGE_IDENTIFIER_EXCEPTIONS = {
    "OOCExactFreshConstructionFailure": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "AdoptInheritedOpenFileDescription": {
        "src/relation/ooc_private_handoff_adoption.cpp",
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "ExactPrivateDirectoryBinding": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "ExactPrivateDirectoryConstructionToken": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "discard_inherited_post_fork_child_noexcept": {
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
}
BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE = (
    "src/relation/ooc_private_handoff_adoption.cpp"
)
BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE = (
    "src/relation/ooc_private_handoff_adoption_internal.hpp"
)
BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE = (
    "src/sieve/distributed_sieve_wave_store.cpp"
)
BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER = (
    "include/gnfs/relation/ooc_cleanup_transaction.hpp"
)
BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_ALLOWLISTS = {
    "OOCPrivateHandoffBorrowedBaseLockV1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE,
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE,
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER,
    },
    "adopt_private_handoff_with_borrowed_base_lock_v1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE,
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE,
    },
    "OOCPrivateHandoffAdoptionBuilderV1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER,
    },
}
BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_USE_COUNTS = {
    "OOCPrivateHandoffBorrowedBaseLockV1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 7,
        BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE: 14,
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE: 1,
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER: 2,
    },
    "adopt_private_handoff_with_borrowed_base_lock_v1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 1,
        BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE: 2,
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE: 1,
    },
    "OOCPrivateHandoffAdoptionBuilderV1": {
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 3,
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER: 3,
    },
}
BORROWED_BASE_LOCK_CONSUME_FUNCTION = (
    "OOCPrivateHandoffBorrowedBaseLockV1::consume"
)
BORROWED_BASE_LOCK_ADOPTION_FUNCTION = (
    "adopt_private_handoff_with_borrowed_base_lock_v1"
)
BORROWED_BASE_LOCK_CONSTRUCTION_CHAIN_FRAGMENT = (
    "std::stringretained_leaf(lock_leaf_);"
    "intduplicated=-1;"
    "do{duplicated=::fcntl(lock_descriptor_,F_DUPFD_CLOEXEC,0);}"
    "while(duplicated<0&&errno==EINTR);"
    "if(duplicated<0){"
    "fail(OOCCleanupStatus::IoFailure,OOCCleanupStage::None,posix_error(errno));}"
    "try{"
    "autoadopted=std::unique_ptr<BaseLock>(newBaseLock("
    "paths.lock_path,duplicated,static_cast<int>(parent.native_handle()),"
    "std::move(retained_leaf),held_parent_identity,lock_identity_,"
    "BaseLock::AdoptInheritedOpenFileDescription{}));"
    "duplicated=-1;"
    "returnstd::shared_ptr<BaseLock>(std::move(adopted));"
    "}catch(...){"
    "if(duplicated>=0){(void)::close(duplicated);}"
    "throw;}"
)
BORROWED_BASE_LOCK_ADOPTION_BODY = (
    "returnadopt_private_handoff_impl("
    "base_path,hooks,true,"
    "[&](constOOCCleanupPaths&paths,AdoptionParentDirectoryHandle&parent){"
    "returnborrowed.consume(paths,parent);},nullptr,nullptr);"
)
BORROWED_BASE_LOCK_RELEASE_FUNCTION = "release_noexcept"
BORROWED_BASE_LOCK_RELEASE_BODY = (
    "#ifdef_WIN32"
    "if(handle_!=INVALID_HANDLE_VALUE){"
    "(void)::CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}"
    "#else"
    "if(descriptor_>=0){(void)::close(descriptor_);descriptor_=-1;}"
    "#endif"
)
BORROWED_BASE_LOCK_RETAINED_FLOCK_FRAGMENT = (
    "do{retained_result=::flock(descriptor,LOCK_EX|LOCK_NB);}"
    "while(retained_result!=0&&errno==EINTR);"
    "if(retained_result!=0){"
    "fail(OOCCleanupStatus::NamespaceConflict,OOCCleanupStage::None,"
    "posix_error(errno));}"
)
BORROWED_BASE_LOCK_WAVE_MINT_FUNCTION = (
    "DistributedSievePrivateLeaseBaseLockAt::adopt_exact_private_handoff"
)
BORROWED_BASE_LOCK_WAVE_MINT_BODY = (
    "constboolowned=owned_by_current_process();"
    "returnprivate_lease::adopt_private_handoff_with_borrowed_base_lock_v1("
    "base_path,private_lease::OOCPrivateHandoffBorrowedBaseLockV1("
    "owned?root_fd_:-1,owned?lock_fd_:-1,leaf_,relation_identity(identity_),"
    "owned?creator_process_id_:0));"
)
BORROWED_BASE_LOCK_TOKEN_CLASS_BODY = (
    "public:"
    "OOCPrivateHandoffBorrowedBaseLockV1()=delete;"
    "OOCPrivateHandoffBorrowedBaseLockV1("
    "constOOCPrivateHandoffBorrowedBaseLockV1&)=delete;"
    "OOCPrivateHandoffBorrowedBaseLockV1&operator=("
    "constOOCPrivateHandoffBorrowedBaseLockV1&)=delete;"
    "OOCPrivateHandoffBorrowedBaseLockV1("
    "OOCPrivateHandoffBorrowedBaseLockV1&&other)noexcept;"
    "OOCPrivateHandoffBorrowedBaseLockV1&operator=("
    "OOCPrivateHandoffBorrowedBaseLockV1&&)=delete;"
    "~OOCPrivateHandoffBorrowedBaseLockV1()=default;"
    "private:"
    "OOCPrivateHandoffBorrowedBaseLockV1("
    "intparent_descriptor,intlock_descriptor,std::string_viewlock_leaf,"
    "std::array<std::uint64_t,3>lock_identity,"
    "std::uint64_tcreator_process_id)noexcept;"
    "[[nodiscard]]std::shared_ptr<BaseLock>consume("
    "constOOCCleanupPaths&paths,AdoptionParentDirectoryHandle&parent);"
    "intparent_descriptor_=-1;"
    "intlock_descriptor_=-1;"
    "std::string_viewlock_leaf_;"
    "std::array<std::uint64_t,3>lock_identity_{};"
    "std::uint64_tcreator_process_id_=0;"
    "boolconsumed_=false;"
    "friendclass::gnfs::sieve::distributed_sieve_resume_detail::"
    "DistributedSievePrivateLeaseBaseLockAt;"
    "friendOOCPrivateHandoffAdoptionResult"
    "adopt_private_handoff_with_borrowed_base_lock_v1("
    "conststd::filesystem::path&base_path,"
    "OOCPrivateHandoffBorrowedBaseLockV1&&borrowed,"
    "OOCPrivateHandoffAdoptionTestHookshooks)noexcept;"
)
CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE = (
    "src/relation/ooc_private_cleanup_union.cpp"
)
CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE = (
    "src/relation/ooc_private_cleanup_union_internal.hpp"
)
CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE = (
    "src/relation/ooc_private_handoff_adoption.cpp"
)
CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE = (
    "src/relation/ooc_private_handoff_adoption_internal.hpp"
)
CONSUMED_CANONICAL_ADOPTION_TEST_FILE = (
    "tests/test_ooc_cleanup_transaction.cpp"
)
MERGE_PREPARED_ADMISSION_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_merge_prepared_admission_internal.hpp"
)
MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE = (
    "src/sieve/distributed_sieve_wave_store.cpp"
)
MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_wave_store_internal.hpp"
)
MERGE_PREPARED_ADMISSION_IDENTIFIER = (
    "DistributedSieveMergePreparedAdmissionV1"
)
MERGE_PREPARED_ADMISSION_FRESH_PUBLISH_FUNCTION = "publish_impl"
MERGE_PREPARED_ADMISSION_FRESH_VALIDATOR = (
    "validate_prepared_admission_origin"
)
MERGE_PREPARED_ADMISSION_RECOVERED_VALIDATOR = (
    "recovered_merge_prepared_admission_state_valid"
)
MERGE_PREPARED_ADMISSION_OPEN_RESULT = "DistributedSieveWaveStoreOpenResult"
MERGE_PREPARED_ADMISSION_OPEN_FUNCTION = "DistributedSieveWaveStore::open"
CONSUMED_CANONICAL_ADOPTION_TOKEN = (
    "OOCPrivateHandoffConsumedPublicationBaseLockV1"
)
CONSUMED_CANONICAL_ADOPTION_BRIDGE = (
    "adopt_private_handoff_with_consumed_publication_base_lock_v1"
)
CONSUMED_CANONICAL_ADOPTION_ENTRY = (
    "adopt_consumed_canonical_private_handoff_publication_v1"
)
CONSUMED_CANONICAL_READER_ADOPTION_ENTRY = (
    "adopt_consumed_canonical_private_handoff_reader_v1"
)
CONSUMED_CANONICAL_READER_ADOPTION_RESULT = (
    "PrivateHandoffPublicationReaderAdoptionResultV1"
)
CONSUMED_CANONICAL_READER_REVALIDATOR = (
    "PrivateHandoffPublicationAdoptionRevalidatorV1"
)
CONSUMED_CANONICAL_READER_TRUSTED_AUTHORITY = (
    "MergePreparedAdmissionRevalidatorAuthorityV1"
)
CONSUMED_CANONICAL_ADOPTION_COMMIT = "commit_canonical_publication_terminal"
CONSUMED_CANONICAL_ADOPTION_SHAPE_MATCHER = (
    "publication_terminal_shape_valid_for_adoption"
)
CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER = (
    "require_publication_terminal_match"
)
CONSUMED_CANONICAL_ADOPTION_IDENTIFIER_USE_COUNTS = {
    CONSUMED_CANONICAL_ADOPTION_TOKEN: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 2,
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 2,
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE: 12,
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: 16,
    },
    CONSUMED_CANONICAL_ADOPTION_BRIDGE: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 2,
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 1,
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE: 2,
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: 4,
    },
    CONSUMED_CANONICAL_ADOPTION_ENTRY: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 1,
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 2,
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: 1,
    },
    CONSUMED_CANONICAL_READER_ADOPTION_ENTRY: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 1,
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 3,
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: 1,
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE: 1,
    },
    CONSUMED_CANONICAL_READER_REVALIDATOR: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 11,
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 17,
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE: 1,
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: 4,
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE: 3,
    },
    CONSUMED_CANONICAL_READER_TRUSTED_AUTHORITY: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE: 2,
        MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE: 2,
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE: 13,
    },
    CONSUMED_CANONICAL_ADOPTION_COMMIT: {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 3,
    },
    CONSUMED_CANONICAL_ADOPTION_SHAPE_MATCHER: {
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE: 2,
    },
    CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER: {
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE: 3,
    },
    "ConsumedCanonical": {
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE: 4,
    },
}
CONSUMED_CANONICAL_ADOPTION_FLEXIBLE_TEST_USES = {
    CONSUMED_CANONICAL_ADOPTION_ENTRY: {CONSUMED_CANONICAL_ADOPTION_TEST_FILE},
    CONSUMED_CANONICAL_READER_ADOPTION_ENTRY: {
        CONSUMED_CANONICAL_ADOPTION_TEST_FILE
    },
    CONSUMED_CANONICAL_READER_REVALIDATOR: {
        CONSUMED_CANONICAL_ADOPTION_TEST_FILE
    },
}
CONSUMED_CANONICAL_ADOPTION_PUBLIC_IDENTIFIERS = tuple(
    CONSUMED_CANONICAL_ADOPTION_IDENTIFIER_USE_COUNTS
)
CONSUMED_CANONICAL_ADOPTION_RELATION_DECLARATION = (
    "[[nodiscard]]OOCPrivateHandoffAdoptionResult"
    "adopt_consumed_canonical_private_handoff_publication_v1("
    "PrivateHandoffPublicationValidatedPermitV1&&permit,"
    "OOCPrivateHandoffAdoptionTestHookshooks={})noexcept;"
)
CONSUMED_CANONICAL_ADOPTION_RELATION_FRIEND = (
    "friendOOCPrivateHandoffAdoptionResult"
    "adopt_consumed_canonical_private_handoff_publication_v1("
    "PrivateHandoffPublicationValidatedPermitV1&&permit,"
    "OOCPrivateHandoffAdoptionTestHookshooks)noexcept;"
)
CONSUMED_CANONICAL_READER_ADOPTION_RELATION_DECLARATION = (
    "[[nodiscard]]PrivateHandoffPublicationReaderAdoptionResultV1"
    "adopt_consumed_canonical_private_handoff_reader_v1("
    "PrivateHandoffPublicationValidatedPermitV1&permit,"
    "PrivateHandoffPublicationAdoptionRevalidatorV1&&revalidator,"
    "OOCPrivateHandoffAdoptionTestHookshooks={})noexcept;"
)
CONSUMED_CANONICAL_READER_ADOPTION_RELATION_FRIEND = (
    "friendPrivateHandoffPublicationReaderAdoptionResultV1"
    "adopt_consumed_canonical_private_handoff_reader_v1("
    "PrivateHandoffPublicationValidatedPermitV1&permit,"
    "PrivateHandoffPublicationAdoptionRevalidatorV1&&revalidator,"
    "OOCPrivateHandoffAdoptionTestHookshooks)noexcept;"
)
CONSUMED_CANONICAL_ADOPTION_BRIDGE_DECLARATION = (
    "[[nodiscard]]OOCPrivateHandoffAdoptionResult"
    "adopt_private_handoff_with_consumed_publication_base_lock_v1("
    "conststd::filesystem::path&base_path,"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1&&authority,"
    "OOCPrivateHandoffAdoptionTestHookshooks={})noexcept;"
)
CONSUMED_CANONICAL_ADOPTION_REVALIDATING_BRIDGE_DECLARATION = (
    "[[nodiscard]]OOCPrivateHandoffAdoptionResult"
    "adopt_private_handoff_with_consumed_publication_base_lock_v1("
    "conststd::filesystem::path&base_path,"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1&&authority,"
    "PrivateHandoffPublicationAdoptionRevalidatorV1&revalidator,"
    "OOCPrivateHandoffAdoptionTestHookshooks={})noexcept;"
)
CONSUMED_CANONICAL_ADOPTION_TOKEN_CLASS_BODY = r"""
public:
    OOCPrivateHandoffConsumedPublicationBaseLockV1() = delete;
    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        const OOCPrivateHandoffConsumedPublicationBaseLockV1&) = delete;
    OOCPrivateHandoffConsumedPublicationBaseLockV1&
    operator=(const OOCPrivateHandoffConsumedPublicationBaseLockV1&) = delete;

    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        OOCPrivateHandoffConsumedPublicationBaseLockV1&& other) noexcept;
    OOCPrivateHandoffConsumedPublicationBaseLockV1&
    operator=(OOCPrivateHandoffConsumedPublicationBaseLockV1&&) = delete;
    ~OOCPrivateHandoffConsumedPublicationBaseLockV1() = default;

private:
    OOCPrivateHandoffConsumedPublicationBaseLockV1(
        std::shared_ptr<BaseLock> live_lock,
        std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1> terminal,
        std::uint64_t creator_process_id) noexcept;

    std::shared_ptr<BaseLock> live_lock_;
    std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1> terminal_;
    std::uint64_t creator_process_id_ = 0;
    bool consumed_ = false;

    friend OOCPrivateHandoffAdoptionResult
    adopt_consumed_canonical_private_handoff_publication_v1(
        PrivateHandoffPublicationValidatedPermitV1&& permit,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
    friend PrivateHandoffPublicationReaderAdoptionResultV1
    adopt_consumed_canonical_private_handoff_reader_v1(
        PrivateHandoffPublicationValidatedPermitV1& permit,
        PrivateHandoffPublicationAdoptionRevalidatorV1&& revalidator,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
    friend OOCPrivateHandoffAdoptionResult
    adopt_private_handoff_with_consumed_publication_base_lock_v1(
        const std::filesystem::path& base_path,
        OOCPrivateHandoffConsumedPublicationBaseLockV1&& authority,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
    friend OOCPrivateHandoffAdoptionResult
    adopt_private_handoff_with_consumed_publication_base_lock_v1(
        const std::filesystem::path& base_path,
        OOCPrivateHandoffConsumedPublicationBaseLockV1&& authority,
        PrivateHandoffPublicationAdoptionRevalidatorV1& revalidator,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
"""
CONSUMED_CANONICAL_ADOPTION_TOKEN_CONSTRUCTOR_FRAGMENT = (
    "OOCPrivateHandoffConsumedPublicationBaseLockV1::"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1("
    "std::shared_ptr<BaseLock>live_lock,"
    "std::shared_ptr<constPrivateHandoffPublicationPrefixWitnessV1>terminal,"
    "std::uint64_tcreator_process_id)noexcept:"
    "live_lock_(std::move(live_lock)),terminal_(std::move(terminal)),"
    "creator_process_id_(creator_process_id){}"
)
CONSUMED_CANONICAL_ADOPTION_TOKEN_MOVE_FRAGMENT = (
    "OOCPrivateHandoffConsumedPublicationBaseLockV1::"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1("
    "OOCPrivateHandoffConsumedPublicationBaseLockV1&&other)noexcept:"
    "live_lock_(std::move(other.live_lock_)),"
    "terminal_(std::move(other.terminal_)),"
    "creator_process_id_(std::exchange(other.creator_process_id_,0)),"
    "consumed_(std::exchange(other.consumed_,true)){}"
)
CONSUMED_CANONICAL_ADOPTION_TOKEN_ASSERT_FRAGMENT = (
    "static_assert(!std::is_default_constructible_v<"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1>);"
    "static_assert(!std::is_copy_constructible_v<"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1>);"
    "static_assert(!std::is_copy_assignable_v<"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1>);"
    "static_assert(std::is_nothrow_move_constructible_v<"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1>);"
    "static_assert(!std::is_move_assignable_v<"
    "OOCPrivateHandoffConsumedPublicationBaseLockV1>);"
)
CONSUMED_CANONICAL_ADOPTION_PHASE_BODY = r"""
Observed,
Validated,
ConsumedNonTerminal,
ConsumedCanonical,
"""
CONSUMED_CANONICAL_ADOPTION_COMMIT_BODY = r"""
    const bool canonical_disposition =
        completed.disposition == PrivateHandoffPublicationResumeDispositionV1::CanonicalTerminal ||
        completed.disposition ==
            PrivateHandoffPublicationResumeDispositionV1::CanonicalConverged;
    if (state.phase !=
            PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedNonTerminal ||
        state.canonical_terminal || !canonical_disposition || !completed.expected_prefix ||
        !completed.terminal_prefix ||
        completed.result.status != OOCCleanupStatus::HandoffPresent ||
        completed.result.stage != OOCCleanupStage::None || completed.result.native_error ||
        !canonical_publication_terminal_shape_valid(*completed.terminal_prefix)) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }

    auto retained_terminal = *completed.terminal_prefix;
    state.canonical_terminal.emplace(std::move(retained_terminal));
    state.phase = PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedCanonical;
    return completed;
"""
CONSUMED_CANONICAL_ADOPTION_ENTRY_BODY = r"""
    auto state = std::move(permit.state_);
    try {
        if (!state || !state->lock ||
            state->phase !=
                PrivateHandoffPublicationObservedPermitV1::State::Phase::ConsumedCanonical ||
            state->creator_process_id != static_cast<std::uint64_t>(gnfs::util::process_id())) {
            fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
        }
        if (!state->canonical_terminal ||
            !canonical_publication_terminal_shape_valid(*state->canonical_terminal) ||
            state->expected_directory_identity != state->canonical_terminal->directory_identity ||
            state->lock->identity() != state->canonical_terminal->lock_identity) {
            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
        }

        auto owner = std::move(state);
        auto live_lock = std::shared_ptr<BaseLock>(owner, owner->lock.get());
        auto terminal = std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1>(
            owner, std::addressof(*owner->canonical_terminal));
        OOCPrivateHandoffConsumedPublicationBaseLockV1 authority(
            std::move(live_lock), std::move(terminal), owner->creator_process_id);
        return adopt_private_handoff_with_consumed_publication_base_lock_v1(
            owner->paths.base_path, std::move(authority), hooks);
    } catch (const Failure& failure) {
        return consumed_publication_adoption_failure(failure.status, failure.error);
    } catch (const std::bad_alloc&) {
        return consumed_publication_adoption_failure(
            OOCCleanupStatus::UnexpectedFailure,
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        return consumed_publication_adoption_failure(OOCCleanupStatus::UnexpectedFailure,
                                                     error.code());
    } catch (...) {
        return consumed_publication_adoption_failure(OOCCleanupStatus::UnexpectedFailure);
    }
"""
CONSUMED_CANONICAL_ADOPTION_SHAPE_MATCHER_BODY = r"""
    if (terminal.state != ooc_cleanup_detail::PrivateHandoffPublicationPrefixStateV1::Canonical ||
        !terminal.canonical_snapshot || terminal.pending_snapshot || terminal.rollback_snapshot ||
        !terminal.owner || !terminal.owned || terminal.reserved ||
        !ooc_cleanup_detail::private_lease_record_shape_valid(terminal.owner->record) ||
        !ooc_cleanup_detail::private_lease_record_shape_valid(terminal.owned->record)) {
        return false;
    }

    const auto& owner = *terminal.owner;
    const auto& owned = *terminal.owned;
    return terminal.record.lock_identity ==
               ooc_cleanup_detail::handoff_native_identity(terminal.lock_identity) &&
           terminal.record.directory_identity ==
               ooc_cleanup_detail::handoff_native_identity(terminal.directory_identity) &&
           terminal.record.owner_marker_identity ==
               ooc_cleanup_detail::handoff_native_identity(owner.identity) &&
           terminal.record.owned_marker_identity ==
               ooc_cleanup_detail::handoff_native_identity(owned.identity) &&
           terminal.record.lease_id == owned.record.lease_id &&
           owner.record == ooc_cleanup_detail::owner_record_for(owned.record) &&
           owned.record.phase == ooc_cleanup_detail::PrivateLeasePhase::Owned &&
           owned.record.capability ==
               ooc_cleanup_detail::PrivateLeaseCapability::RollbackPreactivePairAndLease &&
           owned.record.parent_identity == terminal.parent_identity &&
           owned.record.lock_identity == terminal.lock_identity &&
           owned.record.directory_identity == terminal.directory_identity &&
           owned.record.owner_identity == owner.identity;
"""
CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER_BODY = r"""
    if (!publication_terminal_shape_valid_for_adoption(terminal)) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                 ooc_cleanup_detail::protocol_error());
    }

    AdoptionDirectoryEntries expected_entries;
    expected_entries.present[static_cast<std::size_t>(AdoptionEntry::Owner)] = true;
    expected_entries.present[static_cast<std::size_t>(AdoptionEntry::Index)] = true;
    expected_entries.present[static_cast<std::size_t>(AdoptionEntry::Data)] = true;
    expected_entries.present[static_cast<std::size_t>(AdoptionEntry::Canonical)] = true;
    const auto marker_matches = [](const RelativeLeaseMarker& observed,
                                   const auto& expected) noexcept {
        return observed.record == expected.record &&
               observed.leaf.snapshot.identity ==
                   ooc_cleanup_detail::handoff_native_identity(expected.identity);
    };
    if (current.record != terminal.record ||
        current.canonical.snapshot != *terminal.canonical_snapshot || current.pending ||
        current.control.entries != expected_entries || current.control.reserved ||
        !marker_matches(current.control.owner, *terminal.owner) ||
        !marker_matches(current.control.owned, *terminal.owned) ||
        parent.identity() != terminal.parent_identity ||
        lock.identity() != terminal.lock_identity ||
        directory.identity() != terminal.directory_identity) {
        ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,
                                 OOCCleanupStage::None, ooc_cleanup_detail::protocol_error());
    }
"""
CONSUMED_CANONICAL_ADOPTION_BRIDGE_BODY = r"""
    const auto current_process_id = static_cast<std::uint64_t>(gnfs::util::process_id());
    if (authority.consumed_ || !authority.live_lock_ || !authority.terminal_ ||
        authority.creator_process_id_ == 0 || authority.creator_process_id_ != current_process_id) {
        return adoption_failure(OOCCleanupStatus::InvalidRequest,
                                OOCPrivateHandoffState::TaintedPreserved,
                                ooc_cleanup_detail::invalid_argument_error());
    }

    auto terminal = authority.terminal_;
    return adopt_private_handoff_impl(
        base_path, hooks, true,
        [&](const OOCCleanupPaths&, AdoptionParentDirectoryHandle&) {
            if (authority.consumed_ || !authority.live_lock_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            authority.consumed_ = true;
            return std::move(authority.live_lock_);
        },
        terminal.get(), nullptr);
"""
CONSUMED_CANONICAL_ADOPTION_REVALIDATING_BRIDGE_BODY = r"""
    const auto current_process_id = static_cast<std::uint64_t>(gnfs::util::process_id());
    if (authority.consumed_ || !authority.live_lock_ || !authority.terminal_ ||
        authority.creator_process_id_ == 0 || authority.creator_process_id_ != current_process_id ||
        revalidator.validate_ == nullptr || revalidator.creator_process_id_ == 0 ||
        revalidator.creator_process_id_ != current_process_id) {
        return adoption_failure(OOCCleanupStatus::InvalidRequest,
                                OOCPrivateHandoffState::TaintedPreserved,
                                ooc_cleanup_detail::invalid_argument_error());
    }

    auto terminal = authority.terminal_;
    const AdoptionAggregateRevalidatorV1 aggregate_revalidator{
        .validate = revalidator.validate_,
        .context = revalidator.context_,
    };
    return adopt_private_handoff_impl(
        base_path, hooks, true,
        [&](const OOCCleanupPaths&, AdoptionParentDirectoryHandle&) {
            if (authority.consumed_ || !authority.live_lock_) {
                ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                         ooc_cleanup_detail::invalid_argument_error());
            }
            authority.consumed_ = true;
            return std::move(authority.live_lock_);
        },
        terminal.get(), &aggregate_revalidator);
"""
CONSUMED_CANONICAL_ADOPTION_INITIAL_MATCH_FRAGMENT = (
    "if(expected_terminal!=nullptr){"
    "require_publication_terminal_match("
    "*classified.witness,*expected_terminal,*parent,*directory,*lock);}"
    "if(aggregate_revalidator!=nullptr){"
    "require_aggregate_revalidation(paths,*parent,*directory,*lock,"
    "*aggregate_revalidator,nullptr);}"
    "observe_adoption_boundary(paths,*parent,*directory,*lock,hooks,"
    "OOCPrivateHandoffAdoptionFaultPoint::CanonicalClassified);"
)
CONSUMED_CANONICAL_ADOPTION_REVALIDATION_MATCH_FRAGMENT = (
    "if(*current.witness!=*classified.witness){"
    "ooc_cleanup_detail::fail(OOCCleanupStatus::ForeignReplacementPreserved,"
    "OOCCleanupStage::None,ooc_cleanup_detail::protocol_error());}"
    "if(expected_terminal!=nullptr){"
    "require_publication_terminal_match("
    "*current.witness,*expected_terminal,*parent,*directory,*lock);}"
    "autoindex_confirmation=open_private_leaf_exact("
)
CONSUMED_CANONICAL_ADOPTION_RECEIPT_ORDER_FRAGMENT = (
    "revalidate_before_receipt();"
    "observe_adoption_boundary(paths,*parent,*directory,*lock,hooks,"
    "OOCPrivateHandoffAdoptionFaultPoint::BeforeReceiptCommitRevalidation);"
    "revalidate_before_receipt();"
    "if(aggregate_revalidator!=nullptr){"
    "require_aggregate_revalidation(paths,*parent,*directory,*lock,"
    "*aggregate_revalidator,nullptr);"
    "revalidate_before_receipt();}"
    "constautopending_handoff_snapshot="
)
CONSUMED_CANONICAL_ADOPTION_ENTRY_ALIAS_FRAGMENT = (
    "autoowner=std::move(state);"
    "autolive_lock=std::shared_ptr<BaseLock>(owner,owner->lock.get());"
    "autoterminal=std::shared_ptr<constPrivateHandoffPublicationPrefixWitnessV1>("
    "owner,std::addressof(*owner->canonical_terminal));"
)
CONSUMED_CANONICAL_READER_ADOPTION_SUCCESS_ORDER = (
    "autoreader=std::make_unique<OOCPrivateHandoffReader>(std::move(*adopted.adoption));"
    "if(!reader->valid()){"
    "fail(OOCCleanupStatus::UnexpectedFailure,OOCCleanupStage::None,protocol_error());}"
    "#if!defined(__APPLE__)"
    "returnconsumed_publication_reader_adoption_failure("
    "OOCCleanupStatus::PlatformUnsupported,"
    "std::make_error_code(std::errc::operation_not_supported));"
    "#else"
    "constautorequire_terminal="
)
CONSUMED_CANONICAL_READER_ADOPTION_COMMIT_TAIL = (
    "require_terminal();"
    "constboolaggregate_valid=invoke_with_stable_base_lock(*owner->lock,[&]{"
    "returnretained_revalidator.validate_(reader.get(),retained_revalidator.context_);});"
    "if(!aggregate_valid){"
    "fail(OOCCleanupStatus::ForeignReplacementPreserved,OOCCleanupStage::None,"
    "protocol_error());}"
    "require_terminal();"
    "if(!reader->valid()){"
    "fail(OOCCleanupStatus::UnexpectedFailure,OOCCleanupStage::None,protocol_error());}"
    "permit.state_.reset();"
    "returnPrivateHandoffPublicationReaderAdoptionResultV1("
    "adopted.result,adopted.state,std::move(reader));"
    "#endif"
)
MERGE_PREPARED_ADMISSION_VALID_BODY = (
    "if(lifetime_anchor_==nullptr||record_==nullptr||creator_process_id_==0||"
    "origin_validator_==nullptr){returnfalse;}"
    "constintprocess_id=gnfs::util::process_id();"
    "returnprocess_id>0&&"
    "creator_process_id_==static_cast<std::uint64_t>(process_id)&&"
    "origin_validator_(lifetime_anchor_.get(),record_,creator_process_id_);"
)
MERGE_PREPARED_ADMISSION_FRESH_PUBLISH_FRAGMENT = (
    "constauto*stable_record=std::addressof(*state->prepared_record);"
    "conststd::uint64_tcreator_process_id=state->mint.creator_process_id_;"
    "std::shared_ptr<constvoid>lifetime_anchor(std::move(state));"
    "DistributedSieveMergePreparedAdmissionV1admission("
    "std::move(lifetime_anchor),stable_record,creator_process_id,"
    "&DistributedSieveMergeWriterAuthorityV1::"
    "validate_prepared_admission_origin);"
)
MERGE_PREPARED_ADMISSION_FRESH_VALIDATOR_BODY = r"""
    const auto* state =
        static_cast<const DistributedSieveMergeWriterAuthorityStateV1*>(lifetime_anchor);
    if (state == nullptr || state->writer == nullptr || state->manifest == nullptr ||
        state->merge_started_chain.empty() || !state->stream_receipt.has_value() ||
        !state->prepared_record.has_value() || state->prepared_payload.empty() ||
        !state->handoff_published || !state->worker_result || stable_record == nullptr ||
        stable_record != std::addressof(*state->prepared_record) ||
        creator_process_id != state->mint.creator_process_id_ || !state_process_owned(*state) ||
        state->writer->state() != gnfs::relation::OOCWriterState::Finalized ||
        !cached_prepared_payload_is_exact(*state)) {
        return false;
    }
    for (const auto& coordinated : state->worker_result.chunks) {
        if (coordinated.adopted.has_value() && !coordinated.adopted->valid()) {
            return false;
        }
    }
    return true;
"""
MERGE_PREPARED_ADMISSION_OPEN_RESULT_BOOL_BODY = (
    "constboolstore_ready=store!=nullptr&&!prepared_admission.has_value();"
    "constboolprepared_ready=store==nullptr&&prepared_admission.has_value()&&"
    "prepared_admission->valid();"
    "returndiagnostic.status==DistributedSieveWaveStoreStatus::ready&&"
    "store_ready!=prepared_ready;"
)
MERGE_PREPARED_ADMISSION_COLD_CLAIM_FRAGMENT = (
    "autocoordinator=store->claim_worker_coordinator_v1();"
    "if(!coordinator){"
    "returnopen_failure(std::move(coordinator.diagnostic));}"
)
MERGE_PREPARED_ADMISSION_COLD_CLASSIFIER_FRAGMENT = (
    "automerge_prepared=classify_merge_prepared_publication_prefix_v1("
    "MergePreparedAdmissionRevalidatorAuthorityV1::root_fd(*store),"
    "MergePreparedAdmissionRevalidatorAuthorityV1::absolute_root(*store),"
    "MergePreparedAdmissionRevalidatorAuthorityV1::manifest(*store),"
    "store->wave_root_identity(),creator_process_id);"
)
MERGE_PREPARED_ADMISSION_RECOVERED_MINT_FRAGMENT = (
    "usingPreparedAdmission=distributed_sieve_merge_writer_authority_detail::"
    "DistributedSieveMergePreparedAdmissionV1;"
    "std::shared_ptr<constvoid>lifetime_anchor=recovered_prepared_state;"
    "PreparedAdmissionprepared_admission("
    "std::move(lifetime_anchor),"
    "std::addressof(recovered_prepared_state->prepared_record),"
    "creator_process_id,recovered_merge_prepared_admission_state_valid);"
)
MERGE_PREPARED_ADMISSION_RECOVERED_SUCCESS_FRAGMENT = (
    "std::optional<PreparedAdmission>prepared_result;"
    "prepared_result.emplace(std::move(prepared_admission));"
    "return{nullptr,std::move(prepared_result),std::move(published.diagnostic)};"
)
MERGE_PREPARED_RECOVERED_TEST_FILE = "tests/test_distributed_sieve_resume.cpp"
MERGE_PREPARED_RECOVERED_SUBJECT_ENUM = (
    "DistributedSieveRecoveredPreparedPublicationSubjectV1"
)
MERGE_PREPARED_RECOVERED_PHASE_ENUM = (
    "DistributedSieveRecoveredPreparedAggregatePhaseV1"
)
MERGE_PREPARED_RECOVERED_STOP_HOOK = (
    "stop_before_recovered_aggregate_revalidation"
)
MERGE_PREPARED_RECOVERED_SEAM_ALLOWLIST = {
    MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
    MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE,
    MERGE_PREPARED_RECOVERED_TEST_FILE,
}
MERGE_PREPARED_RECOVERED_SUBJECT_ENUM_BODY = "Target,Worker,Count,"
MERGE_PREPARED_RECOVERED_PHASE_ENUM_BODY = (
    "InitialNullReader,ReceiptCommitNullReader,LiveReaderFinal,Count,"
)
MERGE_PREPARED_RECOVERED_STOP_HOOK_FRAGMENT = (
    "if(context->hooks->stop_before_recovered_aggregate_revalidation!=nullptr&&"
    "context->hooks->stop_before_recovered_aggregate_revalidation("
    "context->subject,context->manifest_slot,phase,context->hooks->context)){"
    "context->diagnostic=diagnostic(DistributedSieveWaveStoreStatus::interrupted,"
    "std::make_error_code(std::errc::operation_canceled));"
    "returnfalse;}"
    "if(!process_matches("
    "MergePreparedAdmissionRevalidatorAuthorityV1::creator_process_id("
    "*context->store))){"
    "context->diagnostic=process_mismatch();returnfalse;}"
    "autoheld=context->held;"
    "held.current_reader=current_reader;"
    "context->diagnostic=revalidate_recovered_merge_prepared_projection("
    "*context->store,*context->expected,held);"
)
MERGE_PREPARED_RECOVERED_PHASE_COUNTER_FRAGMENT = (
    "context->aggregate_revalidation_count>="
    "static_cast<std::size_t>("
    "DistributedSieveRecoveredPreparedAggregatePhaseV1::Count)){returnfalse;}"
    "constautophase=static_cast<"
    "DistributedSieveRecoveredPreparedAggregatePhaseV1>("
    "context->aggregate_revalidation_count++);"
    "constboollive_reader_phase=phase=="
    "DistributedSieveRecoveredPreparedAggregatePhaseV1::LiveReaderFinal;"
    "if(live_reader_phase!=(current_reader!=nullptr)){"
)
MERGE_PREPARED_RECOVERED_COUNT_COMMIT_FRAGMENT = (
    "if(context.aggregate_revalidation_count!="
    "static_cast<std::size_t>("
    "DistributedSieveRecoveredPreparedAggregatePhaseV1::Count)){"
    "returndiagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,"
    "protocol_error());}"
)
MERGE_PREPARED_RECOVERED_TARGET_CALL_FRAGMENT = (
    "adopt_retained_publication("
    "retained_target,held_publications.back(),"
    "DistributedSieveRecoveredPreparedPublicationSubjectV1::Target,"
    "static_cast<std::size_t>(DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX))"
)
MERGE_PREPARED_RECOVERED_WORKER_CALL_FRAGMENT = (
    "adopt_retained_publication("
    "worker,held_publications[index],"
    "DistributedSieveRecoveredPreparedPublicationSubjectV1::Worker,"
    "worker.manifest_slot)"
)
MERGE_PREPARED_RECOVERED_WORKER_SLOT_FRAGMENT = (
    "worker.manifest_slot>=store->manifest().chunks.size()||"
    "store->manifest().chunks[worker.manifest_slot].chunk_id!="
    "worker.coordinate.chunk_id"
)
CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_PRIMITIVES = (
    "F_DUPFD",
    "F_DUPFD_CLOEXEC",
    "fcntl",
    "flock",
    "LOCK_EX",
    "LOCK_NB",
    "LOCK_UN",
    "open",
    "openat",
    "CreateFileW",
    "AdoptInheritedOpenFileDescription",
    "acquire_private_handoff_publication_resume_v1",
)
CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_ADOPTION_PATHS = (
    "OOCPrivateHandoffBorrowedBaseLockV1",
    "adopt_private_handoff_with_borrowed_base_lock_v1",
    "OOCCleanupTransaction",
    "adopt_private_handoff",
)
CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_RELEASES = (
    "release_private_cleanup_action",
    "release_noexcept",
    "unlock",
)
WORKER_HANDOFF_BRIDGE_IDENTIFIERS = (
    "OOCFinalizedCorpusEvidenceV1",
    "OOCPrivateHandoffPayloadV1",
    "OOCPrivateHandoffPayloadBuilderV1",
    "capture_finalized_corpus_evidence",
    "finalize_and_publish_private_handoff_built",
)
WORKER_HANDOFF_EVIDENCE_ONLY_FILES = {
    "src/sieve/distributed_sieve_merge_writer_codec_internal.cpp",
    "src/sieve/distributed_sieve_merge_writer_codec_internal.hpp",
    "tests/test_distributed_sieve_merge_writer_codec.cpp",
}
WORKER_HANDOFF_BRIDGE_IDENTIFIER_ALLOWLISTS = {
    "OOCFinalizedCorpusEvidenceV1": {
        "include/gnfs/relation/ooc_relation_store.hpp",
        WORKER_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    }
    | WORKER_HANDOFF_EVIDENCE_ONLY_FILES,
    "OOCPrivateHandoffPayloadV1": {
        "include/gnfs/relation/ooc_relation_store.hpp",
        WORKER_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "OOCPrivateHandoffPayloadBuilderV1": {
        "include/gnfs/relation/ooc_relation_store.hpp",
        WORKER_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "capture_finalized_corpus_evidence": {
        "include/gnfs/relation/ooc_relation_store.hpp",
        WORKER_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
    "finalize_and_publish_private_handoff_built": {
        "include/gnfs/relation/ooc_relation_store.hpp",
        WORKER_WRITER_IMPLEMENTATION_FILE,
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
    },
}
RAW_PRIVATE_HANDOFF_PUBLISHER_IDENTIFIER = "finalize_and_publish_private_handoff"
RAW_PRIVATE_HANDOFF_PUBLISHER_ALLOWLIST = {
    "include/gnfs/relation/ooc_relation_store.hpp",
    "tests/test_ooc_cleanup_transaction.cpp",
    WORKER_WRITER_IMPLEMENTATION_FILE,
}
WORKER_HANDOFF_PUBLICATION_FUNCTION = "finalize_and_publish_handoff_impl"
WORKER_HANDOFF_TYPED_BUILDER_IDENTIFIER = (
    "finalize_and_publish_private_handoff_built"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE = (
    "src/relation/ooc_private_cleanup_union_internal.hpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE = (
    "src/relation/ooc_private_cleanup_union.cpp"
)
PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE = (
    "include/gnfs/relation/ooc_cleanup_transaction.hpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE = (
    "src/sieve/distributed_sieve_wave_store.cpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_wave_store_internal.hpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FILE = (
    "src/relation/ooc_private_handoff_adoption.cpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE = (
    "tests/test_ooc_cleanup_transaction.cpp"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE = (
    "tests/test_distributed_sieve_resume.cpp"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_ALLOWLIST = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE,
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER = (
    "acquire_private_handoff_publication_resume_v1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER = (
    "validate_private_handoff_publication_resume_v1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER = (
    "revalidate_private_handoff_publication_resume_v1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER = (
    "reconcile_private_handoff_publication_for_resume_v1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS = (
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER,
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_DIRECT_CALL_COUNTS = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 3,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 3,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 5,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 3,
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_IDENTIFIERS = (
    "PrivateHandoffPublicationPrefixStateV1",
    "PrivateHandoffPublicationLeaseMarkerWitnessV1",
    "PrivateHandoffPublicationPrefixWitnessV1",
    "PrivateHandoffPublicationObservedPermitV1",
    "PrivateHandoffPublicationValidatedPermitV1",
    "PrivateHandoffPublicationResumeAdmissionV1",
    "PrivateHandoffPublicationResumeValidationV1",
    "PrivateHandoffPublicationResumeRevalidationV1",
    "PrivateHandoffPublicationResumeObservationPointV1",
    "PrivateHandoffPublicationResumeTestHooksV1",
    "PrivateHandoffPublicationTypedValidatorV1",
    "PrivateHandoffPublicationTypedValidatorTestAuthorityV1",
    "PrivateHandoffPublicationResumeDispositionV1",
    "PrivateHandoffPublicationResumeResultV1",
) + PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS
PRIVATE_HANDOFF_PUBLICATION_RESUME_AUXILIARY_USE_SITE_COUNTS = {
    CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: {
        "PrivateHandoffPublicationPrefixWitnessV1": 3,
        "PrivateHandoffPublicationValidatedPermitV1": 3,
    },
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_NARROW_TEST_DIRECT_CALL_IDENTIFIERS = {
    PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE: {
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
    },
}
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_USE_SITE_IDENTIFIERS = (
    "DistributedSieveMergePreparedResumeObservationPointV1",
    "DistributedSieveMergePreparedResumeTestHooksV1",
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_USE_SITE_ALLOWLIST = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_INTERFACE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE,
}
PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER = (
    "WorkerHandoffTypedValidatorAuthorityV1"
)
PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_ALLOWLIST = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
}
PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_IDENTIFIER = (
    "PrivateHandoffPublicationTypedValidatorTestAuthorityV1"
)
PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_ALLOWLIST = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE,
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION = (
    "capture_recoverable_worker_handoff_inventory"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION = (
    "classify_merge_prepared_publication_prefix_v1"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION = (
    "validate_merge_prepared_prefix_type"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION = (
    "DistributedSieveWaveStore::open"
)
PRIVATE_HANDOFF_PUBLICATION_TYPED_VALIDATOR_IDENTIFIER = (
    "validate_worker_handoff_envelope"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_DECLARATION_COUNTS = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 2,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 4,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 2,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 2,
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_IMPLEMENTATION_DEFINITION_COUNTS = {
    identifier: 1
    for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_SCOPES = {
    "PrivateHandoffPublicationTypedValidatorV1": {
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 0,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 1,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 0,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 0,
    },
    "PrivateHandoffPublicationObservedPermitV1": {
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 1,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 1,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 0,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 0,
    },
    "PrivateHandoffPublicationValidatedPermitV1": {
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 0,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 1,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 1,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 1,
    },
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_FRIEND_COUNTS = {
    "PrivateHandoffPublicationTypedValidatorV1": 3,
    "PrivateHandoffPublicationObservedPermitV1": 3,
    "PrivateHandoffPublicationValidatedPermitV1": 5,
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_FRIEND_CLASSES = {
    "PrivateHandoffPublicationTypedValidatorV1": (
        "PrivateHandoffPublicationTypedValidatorTestAuthorityV1",
        "gnfs::sieve::distributed_sieve_resume_detail::"
        "WorkerHandoffTypedValidatorAuthorityV1",
    ),
    "PrivateHandoffPublicationObservedPermitV1": (
        "PrivateHandoffPublicationValidatedPermitV1",
    ),
    "PrivateHandoffPublicationValidatedPermitV1": (),
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_DECLARATION_SHAPES = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: (
        "friendPrivateHandoffPublicationResumeAdmissionV1"
        "acquire_private_handoff_publication_resume_v1("
        "constOOCCleanupPaths&paths,"
        "conststd::array<std::uint64_t,3>&expected_directory_identity)noexcept;",
        "[[nodiscard]]PrivateHandoffPublicationResumeAdmissionV1"
        "acquire_private_handoff_publication_resume_v1("
        "constOOCCleanupPaths&paths,"
        "conststd::array<std::uint64_t,3>&expected_directory_identity)noexcept;",
    ),
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: (
        "friendPrivateHandoffPublicationResumeValidationV1"
        "validate_private_handoff_publication_resume_v1("
        "PrivateHandoffPublicationObservedPermitV1&&observed,"
        "PrivateHandoffPublicationTypedValidatorV1&&validator)noexcept;",
        "friendPrivateHandoffPublicationResumeValidationV1"
        "validate_private_handoff_publication_resume_v1("
        "PrivateHandoffPublicationObservedPermitV1&&observed,"
        "PrivateHandoffPublicationTypedValidatorV1&&validator)noexcept;",
        "friendPrivateHandoffPublicationResumeValidationV1"
        "validate_private_handoff_publication_resume_v1("
        "PrivateHandoffPublicationObservedPermitV1&&observed,"
        "PrivateHandoffPublicationTypedValidatorV1&&validator)noexcept;",
        "[[nodiscard]]PrivateHandoffPublicationResumeValidationV1"
        "validate_private_handoff_publication_resume_v1("
        "PrivateHandoffPublicationObservedPermitV1&&observed,"
        "PrivateHandoffPublicationTypedValidatorV1&&validator)noexcept;",
    ),
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: (
        "friendPrivateHandoffPublicationResumeRevalidationV1"
        "revalidate_private_handoff_publication_resume_v1("
        "constPrivateHandoffPublicationValidatedPermitV1&permit)noexcept;",
        "[[nodiscard]]PrivateHandoffPublicationResumeRevalidationV1"
        "revalidate_private_handoff_publication_resume_v1("
        "constPrivateHandoffPublicationValidatedPermitV1&permit)noexcept;",
    ),
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: (
        "friendPrivateHandoffPublicationResumeResultV1"
        "reconcile_private_handoff_publication_for_resume_v1("
        "PrivateHandoffPublicationValidatedPermitV1&permit,"
        "PrivateHandoffPublicationResumeTestHooksV1hooks)noexcept;",
        "[[nodiscard]]PrivateHandoffPublicationResumeResultV1"
        "reconcile_private_handoff_publication_for_resume_v1("
        "PrivateHandoffPublicationValidatedPermitV1&permit,"
        "PrivateHandoffPublicationResumeTestHooksV1hooks={})noexcept;",
    ),
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_IMPLEMENTATION_DEFINITION_SHAPES = {
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER:
        "PrivateHandoffPublicationResumeAdmissionV1"
        "acquire_private_handoff_publication_resume_v1("
        "constOOCCleanupPaths&paths,"
        "conststd::array<std::uint64_t,3>&expected_directory_identity)noexcept",
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER:
        "PrivateHandoffPublicationResumeValidationV1"
        "validate_private_handoff_publication_resume_v1("
        "PrivateHandoffPublicationObservedPermitV1&&observed,"
        "PrivateHandoffPublicationTypedValidatorV1&&validator)noexcept",
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER:
        "PrivateHandoffPublicationResumeRevalidationV1"
        "revalidate_private_handoff_publication_resume_v1("
        "constPrivateHandoffPublicationValidatedPermitV1&permit)noexcept",
    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER:
        "PrivateHandoffPublicationResumeResultV1"
        "reconcile_private_handoff_publication_for_resume_v1("
        "PrivateHandoffPublicationValidatedPermitV1&permit,"
        "PrivateHandoffPublicationResumeTestHooksV1hooks)noexcept",
}
PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS = (
    "AfterExpectedPrefixValidated",
    "BeforePendingRollbackSourceDirectorySync",
    "AfterPendingRollbackSourceDirectoryDurable",
    "BeforePendingRollbackDestinationDirectorySync",
    "AfterPendingRollbackDestinationDirectoryDurable",
    "AfterPendingRollbackPreactiveDirectoryQuarantinedDurable",
    "AfterPendingRollbackPreactiveDataRemovedDurable",
    "AfterPendingRollbackPreactiveIndexRemovedDurable",
    "AfterPendingRollbackOwnerRemovedDurable",
    "AfterPendingRollbackLeaseDirectoryRemovedDurable",
    "AfterPendingRollbackReservedRemovedDurable",
    "AfterPendingRollbackOwnedRemovedDurable",
    "BeforePendingRollbackTombstoneRemovalValidated",
    "AfterPendingRollbackTombstoneRemovedDurable",
    "AfterCanonicalConfirmedDurable",
    "AfterReservedRevokedDurable",
    "Count",
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_OBSERVATION_ENUM_BODY = "".join(
    f"{point}," for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_TEST_HOOKS_BODY = (
    "usingStopAfter=bool(*)(PrivateHandoffPublicationResumeObservationPointV1point,"
    "void*context)noexcept;"
    "usingFailBefore=bool(*)(PrivateHandoffPublicationResumeObservationPointV1point,"
    "void*context)noexcept;"
    "StopAfterstop_after=nullptr;"
    "FailBeforefail_before=nullptr;"
    "void*context=nullptr;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_OBSERVATION_ENUM = (
    "DistributedSieveWorkerHandoffResumeObservationPointV1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_TEST_HOOKS = (
    "DistributedSieveWorkerHandoffResumeTestHooksV1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ROUND_RELEASE_IDENTIFIER = (
    "after_round_locks_released"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_OBSERVATION_ENUM_BODY = "".join(
    f"{point}," for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_TEST_HOOKS_BODY = (
    "usingStopAfter=bool(*)("
    "DistributedSieveWorkerHandoffResumeObservationPointV1point,"
    "void*context)noexcept;"
    "usingAfterRoundLocksReleased=void(*)(void*context)noexcept;"
    "StopAfterstop_after=nullptr;"
    "AfterRoundLocksReleasedafter_round_locks_released=nullptr;"
    "void*context=nullptr;"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_OBSERVATION_ENUM = (
    "DistributedSieveMergePreparedResumeObservationPointV1"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_TEST_HOOKS = (
    "DistributedSieveMergePreparedResumeTestHooksV1"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_OBSERVATION_ENUM_BODY = "".join(
    f"{point}," for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_TEST_HOOKS_BODY = (
    "usingStopAfter=bool(*)("
    "DistributedSieveMergePreparedResumeObservationPointV1point,"
    "void*context)noexcept;"
    "usingFailBefore=bool(*)("
    "DistributedSieveMergePreparedResumeObservationPointV1point,"
    "void*context)noexcept;"
    "usingAfterRoundLocksReleased=void(*)(void*context)noexcept;"
    "usingStopBeforeRecoveredAggregateRevalidation=bool(*)("
    "DistributedSieveRecoveredPreparedPublicationSubjectV1subject,"
    "std::size_tmanifest_slot,"
    "DistributedSieveRecoveredPreparedAggregatePhaseV1phase,"
    "void*context)noexcept;"
    "StopAfterstop_after=nullptr;"
    "FailBeforefail_before=nullptr;"
    "AfterRoundLocksReleasedafter_round_locks_released=nullptr;"
    "StopBeforeRecoveredAggregateRevalidation"
    "stop_before_recovered_aggregate_revalidation=nullptr;"
    "void*context=nullptr;"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_MIRROR_ASSERTION = (
    "static_assert([]{"
    "usingMergePoint="
    "DistributedSieveMergePreparedResumeObservationPointV1;"
    "usingRelationPoint=private_lease::"
    "PrivateHandoffPublicationResumeObservationPointV1;"
    "constexprstd::arraywave{"
    + "".join(
        f"MergePoint::{point},"
        for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
    )
    + "};constexprstd::arrayrelation{"
    + "".join(
        f"RelationPoint::{point},"
        for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
    )
    + "};"
    "for(std::size_tindex=0;index<wave.size();++index){"
    "if(static_cast<std::size_t>(wave[index])!="
    "static_cast<std::size_t>(relation[index])){returnfalse;}}"
    "returntrue;}());"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_EXCEPTION_SAFETY_FRAGMENT = (
    "autostate=std::make_shared<"
    "PrivateHandoffPublicationObservedPermitV1::State>("
    "paths,expected_directory_identity,std::move(*captured.retained));"
    "state->lock=std::move(lock);"
    "claim.transfer_to_permit();"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_CONSUME_PREFIX = (
    "autostate=std::move(observed.state_);"
    "constautotyped_validate=std::exchange(validator.validate_,nullptr);"
    "void*consttyped_context=std::exchange(validator.context_,nullptr);"
    "constautotyped_creator_process_id="
    "std::exchange(validator.creator_process_id_,0);"
    "try{"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_ADAPTER = (
    "PrivateHandoffLeaseRecoveryObservationAdapterV1"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_ADAPTER_BODY = (
    "constPrivateHandoffPublicationResumeTestHooksV1*outer=nullptr;"
    "constOOCCleanupPaths*paths=nullptr;"
    "constBaseLock*lock=nullptr;"
    "conststd::array<std::uint64_t,3>*expected_directory_identity=nullptr;"
    "constPrivateHandoffPublicationPrefixWitnessV1*initial=nullptr;"
    "std::optional<OOCCleanupResult>exact_failure;"
    "boolunknown_point=false;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_MAP_FUNCTION = (
    "map_private_handoff_lease_recovery_observation"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_MAP_BODY = (
    "usingOuterPoint=PrivateHandoffPublicationResumeObservationPointV1;"
    "switch(point){"
    "caseOOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:"
    "returnOuterPoint::AfterPendingRollbackPreactiveDirectoryQuarantinedDurable;"
    "caseOOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackPreactiveDataRemovedDurable;"
    "caseOOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackPreactiveIndexRemovedDurable;"
    "caseOOCPrivateLeaseFaultPoint::OwnerRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackOwnerRemovedDurable;"
    "caseOOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackLeaseDirectoryRemovedDurable;"
    "caseOOCPrivateLeaseFaultPoint::ReservedRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackReservedRemovedDurable;"
    "caseOOCPrivateLeaseFaultPoint::OwnedRemovedDurable:"
    "returnOuterPoint::AfterPendingRollbackOwnedRemovedDurable;"
    "default:returnstd::nullopt;}"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_STAGE_FUNCTION = (
    "private_handoff_lease_recovery_stage_matches"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_STAGE_BODY = (
    "constboolfinal_directory="
    "current.generation.final_directory_identity.has_value();"
    "constboolstaging_directory="
    "current.generation.staging_directory_identity.has_value();"
    "constboolowner=current.witness.owner.has_value();"
    "constboolowned=current.witness.owned.has_value();"
    "constboolreserved=current.witness.reserved.has_value();"
    "constboolindex=current.rollback_index_present;"
    "constbooldata=current.rollback_data_present;"
    "switch(point){"
    "caseOOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:"
    "return!final_directory&&staging_directory&&owner&&owned&&reserved&&index&&data;"
    "caseOOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:"
    "return!final_directory&&staging_directory&&owner&&owned&&reserved&&index&&!data;"
    "caseOOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:"
    "return!final_directory&&staging_directory&&owner&&owned&&reserved&&!index&&!data;"
    "caseOOCPrivateLeaseFaultPoint::OwnerRemovedDurable:"
    "return!final_directory&&staging_directory&&!owner&&owned&&reserved&&!index&&!data;"
    "caseOOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:"
    "return!final_directory&&!staging_directory&&!owner&&owned&&reserved&&!index&&!data;"
    "caseOOCPrivateLeaseFaultPoint::ReservedRemovedDurable:"
    "return!final_directory&&!staging_directory&&!owner&&owned&&!reserved&&!index&&!data;"
    "caseOOCPrivateLeaseFaultPoint::OwnedRemovedDurable:"
    "return!final_directory&&!staging_directory&&!owner&&!owned&&!reserved&&!index&&!data;"
    "default:returnfalse;}"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_OBSERVER_FUNCTION = (
    "observe_private_handoff_lease_recovery"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_OBSERVER_BODY = (
    "auto&adapter=*static_cast<"
    "PrivateHandoffLeaseRecoveryObservationAdapterV1*>(opaque);"
    "constautomapped=map_private_handoff_lease_recovery_observation(point);"
    "if(!mapped){adapter.unknown_point=true;returntrue;}"
    "if(adapter.outer!=nullptr&&adapter.outer->stop_after!=nullptr&&"
    "adapter.outer->stop_after(*mapped,adapter.outer->context)){returntrue;}"
    "try{"
    "if(adapter.paths==nullptr||adapter.lock==nullptr||"
    "adapter.expected_directory_identity==nullptr||adapter.initial==nullptr){"
    "adapter.exact_failure=resume_unexpected_result(protocol_error());returntrue;}"
    "autocurrent=capture_private_handoff_publication_prefix_v1_locked("
    "*adapter.paths,*adapter.lock,*adapter.expected_directory_identity);"
    "constauto&initial=*adapter.initial;"
    "constautoremaining_marker_matches_initial=[]("
    "conststd::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>&observed,"
    "conststd::optional<PrivateHandoffPublicationLeaseMarkerWitnessV1>&expected){"
    "return!observed||(expected&&*observed==*expected);};"
    "if(!current.retained||"
    "initial.state!=PrivateHandoffPublicationPrefixStateV1::PendingRollback||"
    "initial.canonical_snapshot||initial.pending_snapshot||"
    "!initial.rollback_snapshot||"
    "current.retained->witness.state!="
    "PrivateHandoffPublicationPrefixStateV1::PendingRollback||"
    "current.retained->witness.record!=initial.record||"
    "current.retained->witness.canonical_snapshot||"
    "current.retained->witness.pending_snapshot||"
    "!current.retained->witness.rollback_snapshot||"
    "current.retained->witness.rollback_snapshot!=initial.rollback_snapshot||"
    "current.retained->witness.parent_identity!=initial.parent_identity||"
    "current.retained->witness.lock_identity!=initial.lock_identity||"
    "current.retained->witness.directory_identity!=initial.directory_identity||"
    "!remaining_marker_matches_initial("
    "current.retained->witness.owner,initial.owner)||"
    "!remaining_marker_matches_initial("
    "current.retained->witness.owned,initial.owned)||"
    "!remaining_marker_matches_initial("
    "current.retained->witness.reserved,initial.reserved)||"
    "!private_handoff_lease_recovery_stage_matches(point,*current.retained)){"
    "adapter.exact_failure="
    "current.retained?resume_foreign_replacement():current.result;"
    "if(adapter.exact_failure->status==OOCCleanupStatus::NoTransaction){"
    "adapter.exact_failure=resume_foreign_replacement();}"
    "returntrue;}"
    "adapter.lock->require_stable();returnfalse;}"
    "catch(constFailure&failure){"
    "adapter.exact_failure=resume_failure_result(failure);}"
    "catch(conststd::bad_alloc&){"
    "adapter.exact_failure=resume_unexpected_result("
    "std::make_error_code(std::errc::not_enough_memory));}"
    "catch(conststd::system_error&error){"
    "adapter.exact_failure=resume_unexpected_result(error.code());}"
    "catch(...){adapter.exact_failure=resume_unexpected_result();}"
    "returntrue;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_BINDING_FRAGMENT = (
    "PrivateHandoffLeaseRecoveryObservationAdapterV1lease_adapter{"
    ".outer=&hooks,"
    ".paths=&state->paths,"
    ".lock=&lock,"
    ".expected_directory_identity=&state->expected_directory_identity,"
    ".initial=&rollback_retained.witness,};"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_UNKNOWN_FAILURE_FRAGMENT = (
    ".stop_after=observe_private_handoff_lease_recovery,"
    ".context=&lease_adapter,});"
    "if(lease_adapter.exact_failure){"
    "returnresume_failed(*lease_adapter.exact_failure,expected);}"
    "if(lease_adapter.unknown_point){"
    "returnresume_failed(resume_unexpected_result(protocol_error()),expected);}"
    "if(!recovered.completed())"
)
PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER = (
    "recover_private_handoff_rollback_generation_locked"
)
PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER = "recover_owned_private_lease_locked"
PRIVATE_HANDOFF_ROLLBACK_RECOVERY_DEFINITION_SHAPE = r"""
[[nodiscard]] OOCCleanupResult recover_private_handoff_rollback_generation_locked(
    const OOCCleanupPaths& paths, const BaseLock& lock,
    const std::array<std::uint64_t, 3>& parent_identity,
    const LoadedPrivateLeaseMarker& loaded_owned,
    const std::optional<LoadedPrivateLeaseMarker>& loaded_reserved,
    const OOCPrivateLeaseTestHooks& hooks)
"""
PRIVATE_HANDOFF_ROLLBACK_RECOVERY_BODY = r"""
    lock.require_stable();
    const auto& owned = loaded_owned.record;
    validate_private_lease_record_context(owned, paths, parent_identity, lock.identity());
    if (owned.phase != PrivateLeasePhase::Owned ||
        owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_owned_path, owned, loaded_owned.identity);

    if (loaded_reserved) {
        validate_private_lease_record_context(loaded_reserved->record, paths, parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(loaded_reserved->record, owned);
        confirm_private_lease_marker(paths.lease_reserved_path, loaded_reserved->record,
                                     loaded_reserved->identity);
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    const auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if ((staging_identity && final_identity) ||
        (staging_identity && *staging_identity != owned.directory_identity) ||
        (final_identity && *final_identity != owned.directory_identity) ||
        (!loaded_reserved && (staging_identity || final_identity))) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (staging_identity || final_identity) {
        const auto rolled_back = rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
        if (!rolled_back.completed()) {
            return rolled_back;
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }

    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_private_lease_marker_durable(paths.lease_reserved_path, loaded_reserved->record,
                                                loaded_reserved->identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    }
    invoke_with_stable_base_lock(lock, [&] {
        remove_private_lease_marker_durable(paths.lease_owned_path, owned, loaded_owned.identity);
    });
    if (invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_private_lease(hooks,
                                                  OOCPrivateLeaseFaultPoint::OwnedRemovedDurable);
        })) {
        return private_lease_interrupted();
    }
    lock.require_stable();
    return private_lease_completed();
"""
PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPE = r"""
[[nodiscard]] inline OOCCleanupResult
recover_owned_private_lease_locked(const OOCCleanupPaths& paths, const BaseLock& lock,
                                   const std::array<std::uint64_t, 3>& parent_identity,
                                   const LoadedPrivateLeaseMarker& loaded_owned,
                                   const std::optional<LoadedPrivateLeaseMarker>& loaded_reserved,
                                   const OOCPrivateLeaseTestHooks& hooks)
"""
PRIVATE_LEASE_GENERIC_RECOVERY_BODY = r"""
    lock.require_stable();
    const auto& owned = loaded_owned.record;
    validate_private_lease_record_context(owned, paths, parent_identity, lock.identity());
    if (owned.phase != PrivateLeasePhase::Owned) {
        fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
    }
    confirm_private_lease_marker(paths.lease_owned_path, owned, loaded_owned.identity);

    if (loaded_reserved) {
        validate_private_lease_record_context(loaded_reserved->record, paths, parent_identity,
                                              lock.identity());
        validate_private_lease_record_chain(loaded_reserved->record, owned);
        confirm_private_lease_marker(paths.lease_reserved_path, loaded_reserved->record,
                                     loaded_reserved->identity);
    }

    const bool preactive_pair_rollback =
        loaded_reserved &&
        owned.capability == PrivateLeaseCapability::RollbackPreactivePairAndLease;
    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    const auto staging_identity = inspect_directory_identity_locked(staging_path);
    const auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity) {
        if (preactive_pair_rollback) {
            (void)inspect_private_lease_preactive_entries(staging_path, paths);
        } else {
            (void)inspect_private_lease_control_entries(staging_path);
        }
    }
    if (final_identity) {
        inspect_private_lease_transaction_entries(paths.private_directory, paths);
    }

    invoke_with_stable_base_lock(lock, [&] {
        remove_matching_private_lease_pending(paths.lease_owned_pending_path, owned);
    });
    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                  loaded_reserved->record);
        });
    }
    if (preactive_pair_rollback) {
        invoke_with_stable_base_lock(
            lock, [&] { discard_matching_preactive_intent_pending_locked(paths); });
    }

    if (staging_identity) {
        if (!loaded_reserved) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        if (preactive_pair_rollback) {
            const auto rolled_back =
                rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            const auto entries = inspect_private_lease_control_entries(staging_path);
            if (!entries.owner) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            validate_private_lease_owner_at(staging_path, owned);
            invoke_with_stable_base_lock(
                lock, [&] { remove_owner_marker_durable_locked(staging_path, owned, false); });
            invoke_with_stable_base_lock(lock, [&] {
                remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
            });
        }
    } else if (final_identity) {
        const auto owner_path = private_lease_owner_path(paths.private_directory);
        const auto owner_inspection = inspect_private_lease_marker(owner_path);
        if (owner_inspection.kind == InspectKind::Error) {
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, owner_inspection.error);
        }
        if (owner_inspection.kind == InspectKind::Rejected) {
            fail(OOCCleanupStatus::ForeignReplacementPreserved, OOCCleanupStage::None,
                 protocol_error());
        }
        const bool owner_present = owner_inspection.kind == InspectKind::Present;
        if (owner_present) {
            validate_private_lease_owner_at(paths.private_directory, owned);
            inspect_private_lease_transaction_entries(paths.private_directory, paths);
            const auto pair_result =
                run_transaction_locked(paths, lock, nullptr, false, nullptr, nullptr, {});
            if (!pair_result.transaction_terminal()) {
                return pair_result;
            }
        }
        if (preactive_pair_rollback) {
            const auto rolled_back =
                rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
            if (!rolled_back.completed()) {
                return rolled_back;
            }
        } else {
            try {
                require_pair_namespace_reusable_locked(paths);
            } catch (const Failure& failure) {
                if (failure.status == OOCCleanupStatus::NamespaceConflict) {
                    return OOCCleanupResult{
                        .status = OOCCleanupStatus::RecoveryRequired,
                        .stage = OOCCleanupStage::None,
                        .native_error = failure.error,
                    };
                }
                throw;
            }

            const auto entries = inspect_private_lease_control_entries(paths.private_directory);
            if (entries.owner != owner_present) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_owner_marker_durable_locked(paths.private_directory, owned, true);
            });
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::OwnerRemovedDurable);
                })) {
                return private_lease_interrupted();
            }
            invoke_with_stable_base_lock(lock, [&] {
                remove_empty_directory_durable_locked(paths.private_directory,
                                                      owned.directory_identity);
            });
            if (invoke_with_stable_base_lock(lock, [&] {
                    return should_interrupt_private_lease(
                        hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable);
                })) {
                return private_lease_interrupted();
            }
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }

    if (loaded_reserved) {
        invoke_with_stable_base_lock(lock, [&] {
            remove_private_lease_marker_durable(paths.lease_reserved_path, loaded_reserved->record,
                                                loaded_reserved->identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::ReservedRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    }
    invoke_with_stable_base_lock(lock, [&] {
        remove_private_lease_marker_durable(paths.lease_owned_path, owned, loaded_owned.identity);
    });
    if (invoke_with_stable_base_lock(lock, [&] {
            return should_interrupt_private_lease(hooks,
                                                  OOCPrivateLeaseFaultPoint::OwnedRemovedDurable);
        })) {
        return private_lease_interrupted();
    }
    lock.require_stable();
    return private_lease_completed();
"""
PRIVATE_HANDOFF_ROLLBACK_RECOVERY_TYPED_CALL_FRAGMENT = (
    "recovered=recover_private_handoff_rollback_generation_locked("
    "state->paths,lock,rollback_retained.generation.parent_identity,"
    "*rollback_retained.generation.owned,rollback_retained.generation.reserved,"
    "OOCPrivateLeaseTestHooks{"
    ".stop_after=observe_private_handoff_lease_recovery,"
    ".context=&lease_adapter,});"
)
PRIVATE_LEASE_GENERIC_RECOVERY_CALL_FRAGMENTS = (
    "returnrecover_owned_private_lease_locked("
    "paths,held_lock,parent_identity,*owned,reserved,hooks);",
    "returnooc_cleanup_detail::recover_owned_private_lease_locked("
    "paths,retained_lock,generation.parent_identity,*generation.owned,"
    "generation.reserved,hooks);",
)
PRIVATE_LEASE_GENERIC_RECOVERY_SCOPES = {
    "recover_private_lease_locked": (
        "autoadmission=admit_private_cleanup_action_locked("
        "paths,std::move(lock),PrivateNamespaceAction::RecoverPrivateLease);"
        "if(admission.blocked){return*admission.blocked;}",
        "constautohandoff=reconcile_private_handoff_from_permit("
        "permit,PrivateNamespaceAction::RecoverPrivateLease);"
        "if(handoff.state!=OOCPrivateHandoffState::None){returnhandoff.result;}",
        PRIVATE_LEASE_GENERIC_RECOVERY_CALL_FRAGMENTS[0],
    ),
    "OOCCleanupTransaction::remove_private_lease": (
        "autoadmission=ooc_cleanup_detail::admit_private_lease_removal_locked("
        "paths,held_lock,ownership.lease_id_,ownership.directory_identity_,"
        "ownership.owner_identity_,ownership.owned_identity_);"
        "if(admission.blocked){return*admission.blocked;}",
        "constautohandoff="
        "ooc_cleanup_detail::reconcile_private_handoff_from_permit("
        "permit,ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);"
        "if(handoff.state!=OOCPrivateHandoffState::None){returnhandoff.result;}",
        PRIVATE_LEASE_GENERIC_RECOVERY_CALL_FRAGMENTS[1],
    ),
}
PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPES = {
    "recover_private_lease_locked": r"""
OOCCleanupResult recover_private_lease_locked(const OOCCleanupPaths& paths,
                                              std::shared_ptr<BaseLock> lock,
                                              const OOCPrivateLeaseTestHooks& hooks)
""",
    "OOCCleanupTransaction::remove_private_lease": r"""
OOCCleanupResult
OOCCleanupTransaction::remove_private_lease(OOCPrivateLeaseOwnershipReceipt& ownership,
                                            OOCPrivateLeaseTestHooks hooks) noexcept
""",
}
PRIVATE_LEASE_GENERIC_RECOVERY_BODIES = {
    "recover_private_lease_locked": r"""
    if (paths.private_directory.empty() || !lock || !lock->matches(paths.lock_path)) {
        fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None, invalid_argument_error());
    }

    auto admission = admit_private_cleanup_action_locked(
        paths, std::move(lock), PrivateNamespaceAction::RecoverPrivateLease);
    if (admission.blocked) {
        return *admission.blocked;
    }
    if (!admission.permit) {
        fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());
    }
    auto permit = std::move(*admission.permit);
    admission.permit.reset();
    const auto& held_lock =
        begin_private_cleanup_action(permit, paths, PrivateNamespaceAction::RecoverPrivateLease);
    if (invoke_with_stable_base_lock(held_lock, [&] {
            return should_interrupt_private_lease(
                hooks, OOCPrivateLeaseFaultPoint::RecoveryPermitAcquired);
        })) {
        return private_lease_interrupted();
    }

    const auto handoff =
        reconcile_private_handoff_from_permit(permit, PrivateNamespaceAction::RecoverPrivateLease);
    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
    const auto parent = paths.private_directory.parent_path();
    sync_parent_directory(parent, OOCCleanupStage::None);
    const auto parent_identity = capture_directory_identity_locked(parent);

    auto reserved = load_optional_private_lease_marker(paths.lease_reserved_path);
    if (!reserved) {
        const auto pending = load_optional_private_lease_marker(paths.lease_reserved_pending_path);
        if (pending) {
            validate_private_lease_record_context(pending->record, paths, parent_identity,
                                                  held_lock.identity());
            if (pending->record.phase != PrivateLeasePhase::Reserved) {
                fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
            }

            const auto staging_path = private_lease_staging_path(paths, pending->record.lease_id);
            if (inspect_directory_identity_locked(staging_path) ||
                inspect_directory_identity_locked(paths.private_directory) ||
                load_optional_private_lease_marker(paths.lease_owned_path) ||
                load_optional_private_lease_marker(paths.lease_owned_pending_path)) {
                return OOCCleanupResult{
                    .status = OOCCleanupStatus::RecoveryRequired,
                    .stage = OOCCleanupStage::None,
                    .native_error = protocol_error(),
                };
            }
            invoke_with_stable_base_lock(held_lock, [&] {
                remove_matching_private_lease_pending(paths.lease_reserved_pending_path,
                                                      pending->record);
            });
            return private_lease_completed();
        }
    }

    auto owned = load_optional_private_lease_marker(paths.lease_owned_path);
    if (owned) {
        return recover_owned_private_lease_locked(paths, held_lock, parent_identity, *owned,
                                                  reserved, hooks);
    }

    if (reserved) {
        rollback_reserved_staging_locked(paths, held_lock, parent_identity, *reserved);
        held_lock.require_stable();
        return private_lease_completed();
    }

    const auto owned_pending = load_optional_private_lease_marker(paths.lease_owned_pending_path);
    if (owned_pending || inspect_directory_identity_locked(paths.private_directory)) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }
    held_lock.require_stable();
    return private_lease_no_transaction();
""",
    "OOCCleanupTransaction::remove_private_lease": r"""
    if (ownership.spent_ || ownership.base_path_.empty() || ownership.private_directory_.empty() ||
        ownership.lock_path_.empty() ||
        ownership.owner_process_id_ != static_cast<std::uint64_t>(gnfs::util::process_id())) {
        return OOCCleanupResult{
            .status = OOCCleanupStatus::InvalidRequest,
            .stage = OOCCleanupStage::None,
            .native_error = ooc_cleanup_detail::invalid_argument_error(),
        };
    }

    const auto result = invoke([&] {
        const auto paths = ooc_cleanup_detail::freeze_paths(ownership.base_path_);
        if (paths.private_directory.empty() || paths.base_path != ownership.base_path_ ||
            paths.private_directory != ownership.private_directory_ ||
            paths.lock_path != ownership.lock_path_) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }
        std::shared_ptr<ooc_cleanup_detail::BaseLock> held_lock = ownership.live_lock_;
        if (!held_lock) {
            held_lock = std::make_shared<ooc_cleanup_detail::BaseLock>(paths.lock_path, false);
        }
        if (!held_lock->matches(paths.lock_path)) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::InvalidRequest, OOCCleanupStage::None,
                                     ooc_cleanup_detail::invalid_argument_error());
        }

        auto admission = ooc_cleanup_detail::admit_private_lease_removal_locked(
            paths, held_lock, ownership.lease_id_, ownership.directory_identity_,
            ownership.owner_identity_, ownership.owned_identity_);
        if (admission.blocked) {
            return *admission.blocked;
        }
        if (!admission.permit || !admission.generation) {
            ooc_cleanup_detail::fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None,
                                     ooc_cleanup_detail::protocol_error());
        }
        auto generation = std::move(*admission.generation);
        admission.generation.reset();
        auto permit = std::move(*admission.permit);
        admission.permit.reset();
        const auto& retained_lock = ooc_cleanup_detail::begin_private_cleanup_action(
            permit, paths, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        ooc_cleanup_detail::bind_private_lease_removal_generation(permit, generation);
        if (ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                return ooc_cleanup_detail::should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::RemovalPermitAcquired);
            })) {
            return ooc_cleanup_detail::private_lease_interrupted();
        }

        const auto handoff = ooc_cleanup_detail::reconcile_private_handoff_from_permit(
            permit, ooc_cleanup_detail::PrivateNamespaceAction::RemovePrivateLease);
        if (handoff.state != OOCPrivateHandoffState::None) {
            return handoff.result;
        }
        if (!generation.owned) {
            ooc_cleanup_detail::invoke_with_stable_base_lock(retained_lock, [&] {
                ooc_cleanup_detail::sync_parent_directory(paths.private_directory.parent_path(),
                                                          OOCCleanupStage::None);
            });
            return ooc_cleanup_detail::private_lease_completed();
        }

        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
    });
    if (result.completed()) {
        ownership.spent_ = true;
        ownership.live_lock_.reset();
    }
    return result;
""",
}
PRIVATE_LEASE_PREACTIVE_SCANNER_IDENTIFIER = (
    "inspect_private_lease_preactive_entries"
)
PRIVATE_LEASE_PREACTIVE_SCANNER_DEFINITION_SHAPE = r"""
[[nodiscard]] inline PrivateLeasePreactiveEntries
inspect_private_lease_preactive_entries(const std::filesystem::path& directory_path,
                                        const OOCCleanupPaths& paths)
"""
PRIVATE_LEASE_PREACTIVE_SCANNER_BODY = r"""
    PrivateLeasePreactiveEntries entries;
    std::error_code error;
    std::filesystem::directory_iterator cursor(directory_path, error);
    if (error) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, error);
    }
    for (const auto& entry : cursor) {
        const auto leaf = entry.path().filename();
        if (path_leaf_equals_ascii(leaf, ".gnfs-private-lease-v1.owner")) {
            if (entries.owner) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.owner = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.index_path.filename())) {
            if (entries.index) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.index = true;
            continue;
        }
        if (path_leaf_equals(leaf, paths.data_path.filename())) {
            if (entries.data) {
                fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
            }
            entries.data = true;
            continue;
        }
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
"""
PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER = (
    "rollback_owned_preactive_pair_locked"
)
PRIVATE_LEASE_PREACTIVE_ROLLBACK_DEFINITION_SHAPE = r"""
[[nodiscard]] inline OOCCleanupResult
rollback_owned_preactive_pair_locked(const OOCCleanupPaths& paths,
                                     const PrivateLeaseRecord& owned,
                                     const BaseLock& lock,
                                     const OOCPrivateLeaseTestHooks& hooks)
"""
PRIVATE_LEASE_PREACTIVE_ROLLBACK_BODY = r"""
    if (owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
        fail(OOCCleanupStatus::RecoveryRequired, OOCCleanupStage::None, protocol_error());
    }

    const auto staging_path = private_lease_staging_path(paths, owned.lease_id);
    auto staging_identity = inspect_directory_identity_locked(staging_path);
    auto final_identity = inspect_directory_identity_locked(paths.private_directory);
    if (staging_identity && final_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (staging_identity && *staging_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    if (final_identity && *final_identity != owned.directory_identity) {
        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }

    if (final_identity) {
        const auto entries =
            inspect_private_lease_preactive_entries(paths.private_directory, paths);
        if (!entries.owner) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }
        validate_private_lease_owner_at(paths.private_directory, owned);
        validate_preactive_pair_leaf_before_quarantine(paths.index_path, entries.index);
        validate_preactive_pair_leaf_before_quarantine(paths.data_path, entries.data);

        const auto renamed = invoke_with_stable_base_lock(
            lock, [&] { return rename_no_replace(paths.private_directory, staging_path); });
        switch (renamed.result) {
        case RenameResult::Succeeded:
            invoke_with_stable_base_lock(lock, [&] {
                sync_parent_directory(staging_path.parent_path(), OOCCleanupStage::None);
            });
            break;
        case RenameResult::DestinationExists:
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, renamed.error);
        case RenameResult::Unsupported:
            fail(OOCCleanupStatus::PlatformUnsupported, OOCCleanupStage::None, renamed.error);
        case RenameResult::Failed:
            fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None, renamed.error);
        }
        if (inspect_directory_identity_locked(paths.private_directory)) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        staging_identity = inspect_directory_identity_locked(staging_path);
        if (!staging_identity || *staging_identity != owned.directory_identity) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
        }
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable);
            })) {
            return private_lease_interrupted();
        }
    }

    if (staging_identity) {
        const auto entries = inspect_private_lease_preactive_entries(staging_path, paths);
        if (entries.owner) {
            validate_private_lease_owner_at(staging_path, owned);
        } else if (entries.index || entries.data) {
            fail(OOCCleanupStatus::IntentConflict, OOCCleanupStage::None, protocol_error());
        }

        const auto staged_data = staging_path / paths.data_path.filename();
        if (entries.data && invoke_with_stable_base_lock(lock, [&] {
                return remove_preactive_pair_leaf_durable_locked(
                    staged_data, staging_path,
                    OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable, hooks);
            })) {
            return private_lease_interrupted();
        }
        const auto staged_index = staging_path / paths.index_path.filename();
        if (entries.index && invoke_with_stable_base_lock(lock, [&] {
                return remove_preactive_pair_leaf_durable_locked(
                    staged_index, staging_path,
                    OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable, hooks);
            })) {
            return private_lease_interrupted();
        }

        invoke_with_stable_base_lock(
            lock, [&] { remove_owner_marker_durable_locked(staging_path, owned, true); });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::OwnerRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
        invoke_with_stable_base_lock(lock, [&] {
            remove_empty_directory_durable_locked(staging_path, owned.directory_identity);
        });
        if (invoke_with_stable_base_lock(lock, [&] {
                return should_interrupt_private_lease(
                    hooks, OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable);
            })) {
            return private_lease_interrupted();
        }
    } else {
        invoke_with_stable_base_lock(lock, [&] {
            sync_parent_directory(paths.private_directory.parent_path(), OOCCleanupStage::None);
        });
    }
    lock.require_stable();
    return private_lease_completed();
"""
PRIVATE_LEASE_GENERIC_PREACTIVE_SCAN_FRAGMENT = (
    "if(staging_identity){if(preactive_pair_rollback){"
    "(void)inspect_private_lease_preactive_entries(staging_path,paths);"
    "}else{(void)inspect_private_lease_control_entries(staging_path);}}"
)
PRIVATE_LEASE_GENERIC_PREACTIVE_ROLLBACK_FRAGMENTS = (
    "if(staging_identity){"
    "if(!loaded_reserved){"
    "fail(OOCCleanupStatus::IntentConflict,OOCCleanupStage::None,"
    "protocol_error());}"
    "if(preactive_pair_rollback){"
    "constautorolled_back="
    "rollback_owned_preactive_pair_locked(paths,owned,lock,hooks);"
    "if(!rolled_back.completed()){returnrolled_back;}}"
    "else{constautoentries="
    "inspect_private_lease_control_entries(staging_path);",
    "if(preactive_pair_rollback){"
    "constautorolled_back="
    "rollback_owned_preactive_pair_locked(paths,owned,lock,hooks);"
    "if(!rolled_back.completed()){returnrolled_back;}}"
    "else{try{require_pair_namespace_reusable_locked(paths);",
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_FINAL_ABSENCE_FRAGMENT = (
    "autoabsent=capture_private_handoff_publication_prefix_v1_locked("
    "state->paths,lock,state->expected_directory_identity);"
    "if(absent.retained||"
    "absent.result.status!=OOCCleanupStatus::NoTransaction||"
    "absent.result.stage!=OOCCleanupStage::None||"
    "absent.result.native_error){"
    "returnresume_failed("
    "absent.retained?resume_foreign_replacement():absent.result,expected);}"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FUNCTION = (
    "adopt_private_handoff_impl"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_ROLLBACK_BLOCKER = (
    "parent->require_lock_binding(paths.lock_path.filename(),*lock);"
    "if(parent->leaf_exists(paths.private_handoff_rollback_path.filename())){"
    "returnassign(adoption_failure("
    "OOCCleanupStatus::NamespaceConflict,"
    "OOCPrivateHandoffState::TaintedPreserved,"
    "ooc_cleanup_detail::protocol_error()));}"
    "constautodirectory_identity="
    "parent->child_directory_identity(paths.private_directory.filename());"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ROUND_RELEASE_FRAGMENT = (
    "while(!recoverable.retained.entries.empty()){"
    "recoverable.retained.entries.pop_back();}"
    "if(hooks.worker_handoff_resume.after_round_locks_released!=nullptr){"
    "hooks.worker_handoff_resume.after_round_locks_released("
    "hooks.worker_handoff_resume.context);"
    "if(!process_matches(creator_process_id)){"
    "returnopen_failure(process_mismatch());}}"
    "++resume_round;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FUNCTION = (
    "bridge_worker_handoff_resume_observation"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_BODY = (
    "auto*context=static_cast<WorkerHandoffResumeBridgeContext*>(opaque);"
    "if(context==nullptr){returntrue;}"
    "constautowave_point=static_cast<"
    "DistributedSieveWorkerHandoffResumeObservationPointV1>(point);"
    "constbooluser_requested_stop="
    "context->user_hooks.stop_after!=nullptr&&"
    "context->user_hooks.stop_after(wave_point,context->user_hooks.context);"
    "if(context->parent_components==nullptr||context->root_leaf==nullptr||"
    "context->manifest_bytes==nullptr||context->absolute_root==nullptr||"
    "context->manifest==nullptr||context->aggregate==nullptr||"
    "context->retained==nullptr||context->attempt_names==nullptr||"
    "context->attempt_record==nullptr){"
    "context->revalidation_failed=true;"
    "context->revalidation_diagnostic=diagnostic("
    "DistributedSieveWaveStoreStatus::unexpected_failure,protocol_error());"
    "returntrue;}"
    "autorevalidated=validate_held_wave_store_manifest_authority("
    "context->parent_fd,*context->parent_components,context->root_fd,"
    "*context->root_leaf,context->root_identity,context->lock_fd,"
    "context->lock_identity,*context->manifest_bytes,"
    "context->manifest_snapshot,context->creator_process_id);"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "revalidated=revalidate_worker_handoff_aggregate_projection("
    "context->root_fd,*context->absolute_root,*context->manifest,"
    "*context->aggregate,*context->retained,context->current_attempt_index,"
    "context->creator_process_id,wave_point);}"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "revalidated=revalidate_exact_canonical_worker_attempt("
    "context->root_fd,*context->attempt_names,*context->attempt_record,"
    "context->creator_process_id);}"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "returnuser_requested_stop;}"
    "context->revalidation_failed=true;"
    "context->revalidation_diagnostic=std::move(revalidated);"
    "returntrue;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FACTORY_FUNCTION = (
    "relation_worker_handoff_resume_hooks"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FACTORY_BODY = (
    "return{.stop_after=bridge_worker_handoff_resume_observation,"
    ".fail_before=nullptr,.context=&context,};"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FUNCTION = (
    "bridge_merge_prepared_resume_observation"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_BODY = (
    "auto*context=static_cast<MergePreparedResumeBridgeContext*>(opaque);"
    "if(context==nullptr){returntrue;}"
    "constautowave_point=static_cast<"
    "DistributedSieveMergePreparedResumeObservationPointV1>(point);"
    "constbooluser_requested_stop=fail_before?"
    "context->user_hooks.fail_before!=nullptr&&"
    "context->user_hooks.fail_before(wave_point,context->user_hooks.context):"
    "context->user_hooks.stop_after!=nullptr&&"
    "context->user_hooks.stop_after(wave_point,context->user_hooks.context);"
    "if(context->parent_components==nullptr||context->root_leaf==nullptr||"
    "context->manifest_bytes==nullptr||context->absolute_root==nullptr||"
    "context->manifest==nullptr||context->aggregate==nullptr||"
    "context->retained==nullptr){"
    "context->revalidation_failed=true;"
    "context->revalidation_diagnostic=diagnostic("
    "DistributedSieveWaveStoreStatus::unexpected_failure,protocol_error());"
    "returntrue;}"
    "autorevalidated=validate_held_wave_store_manifest_authority("
    "context->parent_fd,*context->parent_components,context->root_fd,"
    "*context->root_leaf,context->root_identity,context->lock_fd,"
    "context->lock_identity,*context->manifest_bytes,"
    "context->manifest_snapshot,context->creator_process_id);"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "revalidated=revalidate_merge_prepared_aggregate_projection("
    "context->root_fd,*context->absolute_root,*context->manifest,"
    "*context->aggregate,*context->retained,context->creator_process_id,"
    "wave_point);}"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "revalidated=revalidate_exact_canonical_merge_started("
    "context->root_fd,context->retained->names,"
    "context->retained->start_record,context->creator_process_id);}"
    "if(revalidated.status==DistributedSieveWaveStoreStatus::ready){"
    "returnuser_requested_stop;}"
    "context->revalidation_failed=true;"
    "context->revalidation_diagnostic=std::move(revalidated);"
    "returntrue;"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_STOP_BRIDGE_FUNCTION = (
    "bridge_merge_prepared_resume_stop_after"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_STOP_BRIDGE_BODY = (
    "returnbridge_merge_prepared_resume_observation(point,opaque,false);"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_FAIL_BRIDGE_FUNCTION = (
    "bridge_merge_prepared_resume_fail_before"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_FAIL_BRIDGE_BODY = (
    "returnbridge_merge_prepared_resume_observation(point,opaque,true);"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FACTORY_FUNCTION = (
    "relation_merge_prepared_resume_hooks"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FACTORY_BODY = (
    "return{.stop_after=bridge_merge_prepared_resume_stop_after,"
    ".fail_before=bridge_merge_prepared_resume_fail_before,"
    ".context=&context,};"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_OPEN_FRAGMENT = (
    "constexprstd::size_tmaximum_merge_prepared_resume_rounds=1;"
    "std::size_tmerge_prepared_resume_round=0;"
    "while(true){"
    "if(constautoauthority=validate_held_wave_store_manifest_authority("
    "root.parent.get(),frozen->parent_components,root.root.get(),frozen->leaf,"
    "root.root_identity,lock.lock.get(),lock.lock_identity,*existing.bytes,"
    "*resume_manifest.snapshot,creator_process_id);"
    "authority.status!=DistributedSieveWaveStoreStatus::ready){"
    "returnopen_failure(authority);}"
    "automerge_prepared_prefix=classify_merge_prepared_publication_prefix_v1("
    "root.root.get(),frozen->absolute,*existing.manifest,root.root_identity,"
    "creator_process_id);"
    "if(!merge_prepared_prefix){"
    "returnopen_failure(std::move(merge_prepared_prefix.diagnostic));}"
    "if(!merge_prepared_prefix.prefix_present()){break;}"
    "if(!merge_prepared_prefix.witness.has_value()||"
    "!merge_prepared_prefix.retained.has_value()){"
    "returnopen_failure(diagnostic("
    "DistributedSieveWaveStoreStatus::unexpected_failure,protocol_error()));}"
    "auto&aggregate=*merge_prepared_prefix.witness;"
    "auto&retained=*merge_prepared_prefix.retained;"
    "if(retained.witness.canonical_terminal()){"
    "merge_prepared_prefix.retained.reset();"
    "merge_prepared_prefix.witness.reset();break;}"
    "if(merge_prepared_resume_round>=maximum_merge_prepared_resume_rounds){"
    "returnopen_failure(diagnostic("
    "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error()));}"
    "if(constautoprojection=revalidate_merge_prepared_aggregate_projection("
    "root.root.get(),frozen->absolute,*existing.manifest,aggregate,retained,"
    "creator_process_id);"
    "projection.status!=DistributedSieveWaveStoreStatus::ready){"
    "returnopen_failure(projection);}"
    "if(constautoexact=revalidate_exact_canonical_merge_started("
    "root.root.get(),retained.names,retained.start_record,creator_process_id);"
    "exact.status!=DistributedSieveWaveStoreStatus::ready){"
    "returnopen_failure(exact);}"
    "if(constautoauthority=validate_held_wave_store_manifest_authority("
    "root.parent.get(),frozen->parent_components,root.root.get(),frozen->leaf,"
    "root.root_identity,lock.lock.get(),lock.lock_identity,*existing.bytes,"
    "*resume_manifest.snapshot,creator_process_id);"
    "authority.status!=DistributedSieveWaveStoreStatus::ready){"
    "returnopen_failure(authority);}"
    "constautoexpected_prefix=retained.witness;"
    "constautoexpected_disposition="
    "expected_prefix.pending_only()||expected_prefix.rollback_armed()?"
    "private_lease::PrivateHandoffPublicationResumeDispositionV1::"
    "PendingRolledBack:"
    "private_lease::PrivateHandoffPublicationResumeDispositionV1::"
    "CanonicalConverged;"
    "MergePreparedResumeBridgeContextresume_bridge{"
    ".user_hooks=hooks.merge_prepared_resume,"
    ".parent_fd=root.parent.get(),"
    ".parent_components=&frozen->parent_components,"
    ".root_fd=root.root.get(),"
    ".root_leaf=&frozen->leaf,"
    ".root_identity=root.root_identity,"
    ".lock_fd=lock.lock.get(),"
    ".lock_identity=lock.lock_identity,"
    ".manifest_bytes=&*existing.bytes,"
    ".manifest_snapshot=*resume_manifest.snapshot,"
    ".absolute_root=&frozen->absolute,"
    ".manifest=&*existing.manifest,"
    ".aggregate=&aggregate,"
    ".retained=&retained,"
    ".creator_process_id=creator_process_id,};"
    "autoconverged=private_lease::"
    "reconcile_private_handoff_publication_for_resume_v1("
    "retained.permit,relation_merge_prepared_resume_hooks(resume_bridge));"
    "if(resume_bridge.revalidation_failed){"
    "returnopen_failure(std::move("
    "resume_bridge.revalidation_diagnostic));}"
    "if(!converged.converged()){"
    "returnopen_failure(worker_handoff_inspection_failure(converged.result));}"
    "if(converged.disposition!=expected_disposition||"
    "!converged.expected_prefix.has_value()||"
    "*converged.expected_prefix!=expected_prefix){"
    "returnopen_failure(diagnostic("
    "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error()));}"
    "if(expected_disposition==private_lease::"
    "PrivateHandoffPublicationResumeDispositionV1::PendingRolledBack?"
    "converged.terminal_prefix.has_value():"
    "!converged.terminal_prefix.has_value()||"
    "!converged.terminal_prefix->canonical_terminal()){"
    "returnopen_failure(diagnostic("
    "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error()));}"
    "if(!retained.permit.held()||retained.permit.valid()){"
    "returnopen_failure(diagnostic("
    "DistributedSieveWaveStoreStatus::unexpected_failure,protocol_error()));}"
    "retained.consumed=true;"
    "merge_prepared_prefix.retained.reset();"
    "merge_prepared_prefix.witness.reset();"
    "if(hooks.merge_prepared_resume.after_round_locks_released!=nullptr){"
    "hooks.merge_prepared_resume.after_round_locks_released("
    "hooks.merge_prepared_resume.context);"
    "if(!process_matches(creator_process_id)){"
    "returnopen_failure(process_mismatch());}}"
    "++merge_prepared_resume_round;"
    "if(constautoauthority=validate_held_wave_store_manifest_authority("
    "root.parent.get(),frozen->parent_components,root.root.get(),frozen->leaf,"
    "root.root_identity,lock.lock.get(),lock.lock_identity,*existing.bytes,"
    "*resume_manifest.snapshot,creator_process_id);"
    "authority.status!=DistributedSieveWaveStoreStatus::ready){"
    "returnopen_failure(authority);}"
    "inventory=inspect_namespace(root.root.get());"
    "if(!inventory){"
    "returnopen_failure(std::move(inventory.diagnostic));}}"
    "conststd::size_tmaximum_resume_rounds="
    "existing.manifest->chunks.size()*static_cast<std::size_t>("
    "existing.manifest->max_worker_attempts);"
    "std::size_tresume_round=0;"
)
PRIVATE_HANDOFF_PUBLICATION_RETAINED_WORKER_STACK_SOURCE = r"""
class RetainedWorkerHandoffPublicationPrefixStack final {
public:
    RetainedWorkerHandoffPublicationPrefixStack() = default;
    RetainedWorkerHandoffPublicationPrefixStack(
        const RetainedWorkerHandoffPublicationPrefixStack&) = delete;
    RetainedWorkerHandoffPublicationPrefixStack&
    operator=(const RetainedWorkerHandoffPublicationPrefixStack&) = delete;
    RetainedWorkerHandoffPublicationPrefixStack(
        RetainedWorkerHandoffPublicationPrefixStack&& other) noexcept
        : entries(std::move(other.entries)) {}
    RetainedWorkerHandoffPublicationPrefixStack&
    operator=(RetainedWorkerHandoffPublicationPrefixStack&&) = delete;

    ~RetainedWorkerHandoffPublicationPrefixStack() {
        while (!entries.empty()) {
            entries.pop_back();
        }
    }

    std::vector<RetainedWorkerHandoffPublicationPrefix> entries;
};
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_RETAINED_SUFFIX = (
    "DistributedSieveMergeStartedRecordInventoryWitnessV1start_record;"
    "RetainedWorkerHandoffPublicationPrefixStackretained_workers;"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CAPTURE_RESULT_SOURCE = r"""
struct MergePreparedPublicationPrefixCaptureResult final {
    std::optional<MergePreparedPublicationAggregateWitness> witness;
    std::optional<RetainedMergePreparedPublicationPrefix> retained;
    DistributedSieveWaveStoreDiagnostic diagnostic;

    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostic.status == DistributedSieveWaveStoreStatus::ready;
    }

    [[nodiscard]] bool prefix_present() const noexcept {
        return witness.has_value() || retained.has_value();
    }
};
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_CAPTURE_SOURCE = r"""
std::sort(worker_candidates.begin(), worker_candidates.end(),
          [&](const WorkerHandoffPublicationPrefixCandidate& left,
              const WorkerHandoffPublicationPrefixCandidate& right) {
              const auto& left_attempt = (*parsed.attempts)[left.attempt_index];
              const auto& right_attempt = (*parsed.attempts)[right.attempt_index];
              return std::pair{left_attempt.manifest_chunk_order,
                               left_attempt.worker_coordinate->attempt_ordinal} <
                     std::pair{right_attempt.manifest_chunk_order,
                               right_attempt.worker_coordinate->attempt_ordinal};
          });

RetainedWorkerHandoffPublicationPrefixStack retained_workers;
retained_workers.entries.reserve(worker_candidates.size());
for (const auto& worker_candidate : worker_candidates) {
    const auto& attempt = (*parsed.attempts)[worker_candidate.attempt_index];
    const auto expected_directory_identity =
        worker_candidate.attempt_record.record.lease.directory;
    const auto base_path = absolute_root / attempt.names.private_directory_leaf / "corpus";
    const auto paths = gnfs::relation::OOCCleanupTransaction::paths_for(base_path);
    auto admission = private_lease::acquire_private_handoff_publication_resume_v1(
        paths, relation_identity(expected_directory_identity));
    if (!admission.acquired() || !admission.observed.has_value()) {
        return fail_with(worker_handoff_inspection_failure(admission.result));
    }
    const auto* observed = admission.observed->witness();
    if (observed == nullptr || !observed->canonical_terminal() ||
        observed->state != worker_candidate.expected_state ||
        !worker_handoff_publication_prefix_marker_chain_matches(
            *observed, expected_wave_root_identity, attempt, expected_directory_identity)) {
        return conflict();
    }
    const auto observed_prefix = *observed;
    if (const auto exact = revalidate_exact_canonical_worker_attempt(
            root_fd, *attempt.worker_attempt_names, worker_candidate.attempt_record,
            creator_process_id);
        exact.status != DistributedSieveWaveStoreStatus::ready) {
        return fail_with(exact);
    }
    WorkerHandoffTypedValidationContext typed_context{
        .attempt = &attempt,
        .manifest = &manifest,
        .attempt_record = &worker_candidate.attempt_record,
        .root_fd = root_fd,
        .expected_directory_identity = expected_directory_identity,
        .creator_process_id = creator_process_id,
    };
    auto validation = private_lease::validate_private_handoff_publication_resume_v1(
        std::move(*admission.observed),
        WorkerHandoffTypedValidatorAuthorityV1::bind(validate_worker_handoff_prefix_type,
                                                     &typed_context));
    if (!validation.validated() || !validation.permit.has_value() ||
        !typed_context.typed_handoff.has_value()) {
        if (typed_context.diagnostic.status != DistributedSieveWaveStoreStatus::ready) {
            return fail_with(std::move(typed_context.diagnostic));
        }
        return fail_with(worker_handoff_inspection_failure(validation.result));
    }
    const auto typed_handoff = *typed_context.typed_handoff;
    PrivateLeaseReservationWitness provisional{
        .base_lock_leaf = attempt.names.base_lock_leaf,
        .boundary = DistributedSievePrivateLeaseReservationBoundary::FinalDirectoryDurable,
        .lease_id = observed_prefix.record.lease_id,
        .directory_identity = expected_directory_identity,
        .owner_marker_identity =
            protocol_identity(observed_prefix.record.owner_marker_identity),
        .owned_marker_identity =
            protocol_identity(observed_prefix.record.owned_marker_identity),
        .worker_handoff = typed_handoff,
    };
    lease_slots[worker_candidate.attempt_index] = provisional;
    retained_workers.entries.push_back(RetainedWorkerHandoffPublicationPrefix{
        .attempt_index = worker_candidate.attempt_index,
        .manifest_slot = attempt.manifest_chunk_order,
        .names = *attempt.worker_attempt_names,
        .coordinate = *attempt.worker_coordinate,
        .witness = observed_prefix,
        .typed_handoff = typed_handoff,
        .provisional_lease = std::move(provisional),
        .attempt_record = worker_candidate.attempt_record,
        .permit = std::move(*validation.permit),
    });
}
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_POST_TARGET_SOURCE = r"""
for (const auto& worker : retained_workers.entries) {
    if (worker.consumed || !worker.permit.valid() || !worker.permit.held() ||
        worker.attempt_index >= private_leases.size() ||
        private_leases[worker.attempt_index] != worker.provisional_lease) {
        return conflict();
    }
    const auto exact_worker =
        private_lease::revalidate_private_handoff_publication_resume_v1(worker.permit);
    if (!exact_worker.revalidated() || !exact_worker.witness.has_value()) {
        return fail_with(worker_handoff_inspection_failure(exact_worker.result));
    }
    if (*exact_worker.witness != worker.witness) {
        return conflict();
    }
    if (const auto attempt_record = revalidate_exact_canonical_worker_attempt(
            root_fd, worker.names, worker.attempt_record, creator_process_id);
        attempt_record.status != DistributedSieveWaveStoreStatus::ready) {
        return fail_with(attempt_record);
    }
}
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_AGGREGATE_MOVE_FRAGMENT = (
    ".start_record=start_record,"
    ".retained_workers=std::move(retained_workers),"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_REVALIDATION_FUNCTION = (
    "revalidate_merge_prepared_aggregate_projection"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_COPY_EXCEPTION_SOURCE = r"""
const auto current = inspect_namespace(root_fd);
if (!current) {
    return current.diagnostic;
}
try {
    auto expected_inventory = expected.inventory;
    auto current_inventory = *current.inventory;
    if (observation_point.has_value() &&
        !project_selected_merge_prepared_protocol_state(expected_inventory, retained,
                                                        *observation_point)) {
        return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                          protocol_error());
    }
    expected_inventory.worker_attempt_records.clear();
    expected_inventory.merge_started_records.clear();
    expected_inventory.chunk_terminal_failure_records.clear();
    current_inventory.worker_attempt_records.clear();
    current_inventory.merge_started_records.clear();
    current_inventory.chunk_terminal_failure_records.clear();
    if (current_inventory != expected_inventory) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                          protocol_error());
    }
} catch (const std::bad_alloc&) {
    return diagnostic(DistributedSieveWaveStoreStatus::resource_exhausted,
                      std::make_error_code(std::errc::not_enough_memory));
} catch (...) {
    return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                      std::make_error_code(std::errc::io_error));
}
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_RETAINED_REVALIDATION_SOURCE = r"""
std::size_t retained_worker_matches = 0;
for (std::size_t index = 0; index < parsed.attempts->size(); ++index) {
    const auto& attempt = (*parsed.attempts)[index];
    if (index == retained.attempt_index) {
        if (!attempt.merge_generation_names.has_value() ||
            !attempt.merge_attempt_ordinal.has_value() ||
            *attempt.merge_generation_names != retained.names ||
            *attempt.merge_attempt_ordinal != retained.start_record.merge_attempt_ordinal) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        continue;
    }

    const auto retained_worker = std::find_if(
        expected.retained_workers.entries.begin(), expected.retained_workers.entries.end(),
        [&](const RetainedWorkerHandoffPublicationPrefix& worker) {
            return worker.attempt_index == index;
        });
    if (retained_worker != expected.retained_workers.entries.end()) {
        ++retained_worker_matches;
        const bool canonical_handoff_root_shape =
            !attempt.reserved && !attempt.reserved_pending && attempt.owned &&
            !attempt.owned_pending && attempt.final_directory &&
            !attempt.staging_directory_leaf.has_value();
        if (!canonical_handoff_root_shape || !attempt.worker_attempt_names.has_value() ||
            !attempt.worker_coordinate.has_value() ||
            *attempt.worker_attempt_names != retained_worker->names ||
            *attempt.worker_coordinate != retained_worker->coordinate ||
            retained_worker->consumed || !retained_worker->permit.valid() ||
            !retained_worker->permit.held() ||
            expected.private_leases[index] != retained_worker->provisional_lease ||
            !expected.private_leases[index].worker_handoff.has_value() ||
            *expected.private_leases[index].worker_handoff != retained_worker->typed_handoff ||
            std::ranges::find(expected.worker_attempt_records,
                              retained_worker->attempt_record) ==
                expected.worker_attempt_records.end() ||
            !worker_handoff_publication_prefix_marker_chain_matches(
                retained_worker->witness, manifest.wave_root_identity, attempt,
                retained_worker->typed_handoff.handoff.lease.directory)) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        const auto exact = private_lease::revalidate_private_handoff_publication_resume_v1(
            retained_worker->permit);
        if (!exact.revalidated() || !exact.witness.has_value()) {
            return worker_handoff_inspection_failure(exact.result);
        }
        if (*exact.witness != retained_worker->witness) {
            return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                              protocol_error());
        }
        if (const auto attempt_record = revalidate_exact_canonical_worker_attempt(
                root_fd, retained_worker->names, retained_worker->attempt_record,
                creator_process_id);
            attempt_record.status != DistributedSieveWaveStoreStatus::ready) {
            return attempt_record;
        }
        continue;
    }

    const bool canonical_handoff_root_shape = !attempt.reserved && !attempt.reserved_pending &&
                                              attempt.owned && !attempt.owned_pending &&
                                              attempt.final_directory &&
                                              !attempt.staging_directory_leaf.has_value();
    if (canonical_handoff_root_shape ||
        expected.private_leases[index].worker_handoff.has_value() ||
        expected.private_leases[index].merge_prepared.has_value()) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                          protocol_error());
    }
    auto lease = validate_private_lease_attempt_inventory(root_fd, absolute_root, attempt,
                                                          manifest, creator_process_id);
    if (!lease) {
        return lease.diagnostic;
    }
    if (*lease.witness != expected.private_leases[index]) {
        return diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                          protocol_error());
    }
}
if (retained_worker_matches != expected.retained_workers.entries.size()) {
    return diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure, protocol_error());
}
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_OPEN_RELEASE_ORDER_FRAGMENT = (
    "retained.consumed=true;"
    "merge_prepared_prefix.retained.reset();"
    "merge_prepared_prefix.witness.reset();"
    "if(hooks.merge_prepared_resume.after_round_locks_released!=nullptr){"
    "hooks.merge_prepared_resume.after_round_locks_released("
    "hooks.merge_prepared_resume.context);"
    "if(!process_matches(creator_process_id)){"
    "returnopen_failure(process_mismatch());}}"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_ASSIGNMENT_PREFIX = (
    "autoadmission=private_lease::acquire_private_handoff_publication_resume_v1("
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_FUNCTION = (
    "validate_worker_handoff_prefix_type"
)
PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_BODY = (
    "public:[[nodiscard]]staticgnfs::relation::ooc_cleanup_detail::"
    "PrivateHandoffPublicationTypedValidatorV1bind("
    "gnfs::relation::ooc_cleanup_detail::"
    "PrivateHandoffPublicationTypedValidatorV1::Validatevalidate,"
    "void*context)noexcept{returngnfs::relation::ooc_cleanup_detail::"
    "PrivateHandoffPublicationTypedValidatorV1(validate,context);}"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_BODY = (
    "auto*context=static_cast<WorkerHandoffTypedValidationContext*>(opaque);"
    "if(context==nullptr||context->root_fd<0||context->attempt==nullptr||"
    "context->manifest==nullptr||"
    "context->attempt_record==nullptr||"
    "!context->attempt->worker_attempt_names.has_value()||"
    "!context->attempt->worker_coordinate.has_value()||"
    "!context->attempt_record->canonical_snapshot.has_value()||"
    "context->attempt_record->pending_snapshot.has_value()){returnfalse;}"
    "if(constautoexact=revalidate_exact_canonical_worker_attempt("
    "context->root_fd,*context->attempt->worker_attempt_names,"
    "*context->attempt_record,"
    "context->creator_process_id);"
    "exact.status!=DistributedSieveWaveStoreStatus::ready){"
    "context->diagnostic=exact;returnfalse;}"
    "constdurable_record::RecordSnapshot*handoff_snapshot=nullptr;"
    "switch(prefix.state){"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::PendingOnly:"
    "if(prefix.canonical_snapshot.has_value()||"
    "!prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.pending_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::PendingRollback:"
    "if(prefix.canonical_snapshot.has_value()||"
    "prefix.pending_snapshot.has_value()||"
    "!prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.rollback_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::Canonical:"
    "if(!prefix.canonical_snapshot.has_value()||"
    "prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.canonical_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::IdenticalDual:"
    "if(!prefix.canonical_snapshot.has_value()||"
    "!prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.canonical_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::Count:returnfalse;}"
    "constdurable_record::RecordSnapshotindex_snapshot{"
    ".identity=prefix.record.index.identity,"
    ".size=prefix.record.index.extent,};"
    "constdurable_record::RecordSnapshotdata_snapshot{"
    ".identity=prefix.record.data.identity,"
    ".size=prefix.record.data.extent,};"
    "autotyped=validate_worker_handoff_envelope("
    "*context->attempt,*context->manifest,"
    "context->expected_directory_identity,prefix.record,*handoff_snapshot,"
    "index_snapshot,data_snapshot,context->creator_process_id);"
    "if(!typed){context->diagnostic=std::move(typed.diagnostic);returnfalse;}"
    "constauto&handoff=typed.witness->handoff;"
    "constauto&started=context->attempt_record->record;"
    "if(handoff.attempt_started_digest!=started.self_digest||"
    "handoff.lease!=started.lease||"
    "handoff.chunk_id!=context->attempt->worker_coordinate->chunk_id||"
    "handoff.attempt_ordinal!="
    "context->attempt->worker_coordinate->attempt_ordinal){"
    "context->diagnostic=diagnostic("
    "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error());"
    "returnfalse;}"
    "context->typed_handoff=std::move(*typed.witness);returntrue;"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_TYPED_VALIDATION_FRAGMENT = (
    "WorkerHandoffTypedValidationContexttyped_context{"
    ".attempt=&attempt,"
    ".manifest=&manifest,"
    ".attempt_record=&candidate.attempt_record,"
    ".root_fd=root_fd,"
    ".expected_directory_identity=expected_directory_identity,"
    ".creator_process_id=creator_process_id,};"
    "autovalidation=private_lease::validate_private_handoff_publication_resume_v1("
    "std::move(*admission.observed),"
    "WorkerHandoffTypedValidatorAuthorityV1::bind("
    "validate_worker_handoff_prefix_type,&typed_context));"
)
PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_TYPED_SUCCESS_FRAGMENT = (
    "if(!validation.validated()||!validation.permit.has_value()||"
    "!typed_context.typed_handoff.has_value()){"
    "if(typed_context.diagnostic.status!="
    "DistributedSieveWaveStoreStatus::ready){"
    "returnfail_with(std::move(typed_context.diagnostic));}"
    "returnfail_with(worker_handoff_inspection_failure(validation.result));}"
    "constautotyped_handoff=*typed_context.typed_handoff;"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_ACQUIRE_STATEMENT = (
    "autoadmission=private_lease::"
    "acquire_private_handoff_publication_resume_v1("
    "paths,relation_identity(expected_directory_identity));"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_VALIDATION_FRAGMENT = (
    "MergePreparedTypedValidationContexttyped_context{"
    ".attempt=&target_attempt,"
    ".manifest=&manifest,"
    ".start_record=&marker_bound_starts.witnesses->back(),"
    ".worker_attempts=&*worker_attempts.witnesses,"
    ".private_leases=&private_leases,"
    ".merge_starts=&*marker_bound_starts.witnesses,"
    ".root_fd=root_fd,"
    ".expected_directory_identity=expected_directory_identity,"
    ".creator_process_id=creator_process_id,};"
    "autovalidation=private_lease::"
    "validate_private_handoff_publication_resume_v1("
    "std::move(*admission.observed),"
    "WorkerHandoffTypedValidatorAuthorityV1::bind("
    "validate_merge_prepared_prefix_type,&typed_context));"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_REVALIDATE_STATEMENT = (
    "constautorevalidated=private_lease::"
    "revalidate_private_handoff_publication_resume_v1(*validation.permit);"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDING_IDENTIFIERS = (
    "closed_merge_started_revalidator_v1",
    "closed_merge_prepared_envelope_validator_v1",
    "closed_merge_prepared_dependency_validator_v1",
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDINGS_SOURCE = r"""
using MergePreparedStartedRevalidatorV1 = DistributedSieveWaveStoreDiagnostic (*)(
    int, const DistributedSieveMergeGenerationNamesV1&,
    const DistributedSieveMergeStartedRecordInventoryWitnessV1&, std::uint64_t) noexcept;
using MergePreparedEnvelopeValidatorV1 = MergePreparedInventoryValidationResult (*)(
    const PrivateLeaseAttemptInventory&, const WaveManifestV1&, const NativeIdentityV1&,
    const gnfs::relation::OOCPrivateHandoffRecordV1&,
    const durable_record::RecordSnapshot&, const durable_record::RecordSnapshot&,
    const durable_record::RecordSnapshot&, std::uint64_t) noexcept;
using MergePreparedDependencyValidatorV1 = DistributedSieveProtocolStatus (*)(
    const WaveManifestV1&,
    std::span<const DistributedSieveWorkerAttemptRecordInventoryWitness>,
    std::span<const PrivateLeaseReservationWitness>,
    std::span<const DistributedSieveMergeStartedRecordInventoryWitnessV1>,
    const MergePreparedV1&) noexcept;

constexpr MergePreparedStartedRevalidatorV1 closed_merge_started_revalidator_v1 =
    &revalidate_exact_canonical_merge_started;
constexpr MergePreparedEnvelopeValidatorV1 closed_merge_prepared_envelope_validator_v1 =
    &validate_merge_prepared_envelope;
constexpr MergePreparedDependencyValidatorV1 closed_merge_prepared_dependency_validator_v1 =
    &validate_merge_prepared_dependency_projection;
"""
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_CALLS = (
    PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDING_IDENTIFIERS
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS = (
    (
        "auto*context=static_cast<MergePreparedTypedValidationContext*>(opaque);"
        "if(context==nullptr||context->attempt==nullptr||context->manifest==nullptr||"
        "context->start_record==nullptr||context->worker_attempts==nullptr||"
        "context->private_leases==nullptr||context->merge_starts==nullptr||"
        "context->root_fd<0||"
        "!context->attempt->merge_generation_names.has_value()||"
        "!context->attempt->merge_attempt_ordinal.has_value()||"
        "!context->start_record->canonical_snapshot.has_value()||"
        "context->start_record->pending_snapshot.has_value()){returnfalse;}"
    ),
    (
        "if(constautoexact=closed_merge_started_revalidator_v1("
        "context->root_fd,*context->attempt->merge_generation_names,"
        "*context->start_record,context->creator_process_id);"
        "exact.status!=DistributedSieveWaveStoreStatus::ready){"
        "context->diagnostic=exact;returnfalse;}"
    ),
    (
        "autotyped=closed_merge_prepared_envelope_validator_v1("
        "*context->attempt,*context->manifest,"
        "context->expected_directory_identity,prefix.record,*handoff_snapshot,"
        "index_snapshot,data_snapshot,context->creator_process_id);"
        "if(!typed){context->diagnostic=std::move(typed.diagnostic);returnfalse;}"
    ),
    (
        "constauto&prepared=typed.witness->prepared;"
        "constauto&started=context->start_record->record;"
        "if(prepared.merge_started_digest!=started.self_digest||"
        "prepared.merged_lease!=started.merged_lease||"
        "prepared.ordered_inputs!=started.ordered_inputs||"
        "*context->attempt->merge_attempt_ordinal!="
        "started.merge_attempt_ordinal){"
        "context->diagnostic=diagnostic("
        "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error());"
        "returnfalse;}"
    ),
    (
        "constautodependency=closed_merge_prepared_dependency_validator_v1("
        "*context->manifest,*context->worker_attempts,*context->private_leases,"
        "*context->merge_starts,prepared);"
        "if(!dependency){"
        "context->diagnostic=diagnostic("
        "dependency.error==DistributedSieveProtocolError::resource_exhausted?"
        "DistributedSieveWaveStoreStatus::resource_exhausted:"
        "DistributedSieveWaveStoreStatus::namespace_conflict,protocol_error());"
        "context->diagnostic.protocol_status=dependency;returnfalse;}"
    ),
    "context->typed_prepared=std::move(*typed.witness);returntrue;",
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_PREFIX_STATE_FRAGMENT = (
    "constdurable_record::RecordSnapshot*handoff_snapshot=nullptr;"
    "switch(prefix.state){"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::PendingOnly:"
    "if(prefix.canonical_snapshot.has_value()||"
    "!prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.pending_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::PendingRollback:"
    "if(prefix.canonical_snapshot.has_value()||"
    "prefix.pending_snapshot.has_value()||"
    "!prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.rollback_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::Canonical:"
    "if(!prefix.canonical_snapshot.has_value()||"
    "prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.canonical_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::IdenticalDual:"
    "if(!prefix.canonical_snapshot.has_value()||"
    "!prefix.pending_snapshot.has_value()||"
    "prefix.rollback_snapshot.has_value()){returnfalse;}"
    "handoff_snapshot=&*prefix.canonical_snapshot;break;"
    "caseprivate_lease::PrivateHandoffPublicationPrefixStateV1::Count:"
    "returnfalse;}"
    "constdurable_record::RecordSnapshotindex_snapshot{"
    ".identity=prefix.record.index.identity,"
    ".size=prefix.record.index.extent,};"
    "constdurable_record::RecordSnapshotdata_snapshot{"
    ".identity=prefix.record.data.identity,"
    ".size=prefix.record.data.extent,};"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_CATCH_FRAGMENT = (
    "}catch(conststd::bad_alloc&){"
    "context->diagnostic=diagnostic("
    "DistributedSieveWaveStoreStatus::resource_exhausted,"
    "std::make_error_code(std::errc::not_enough_memory));"
    "returnfalse;}catch(...){"
    "context->diagnostic=diagnostic("
    "DistributedSieveWaveStoreStatus::unexpected_failure,"
    "std::make_error_code(std::errc::io_error));returnfalse;}"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_BODY = (
    PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[0]
    + "try{"
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[1]
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_PREFIX_STATE_FRAGMENT
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[2]
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[3]
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[4]
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS[5]
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_CATCH_FRAGMENT
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS = (
    PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION,
    PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION,
    "revalidate_exact_canonical_merge_started",
    "validate_merge_prepared_envelope",
    "validate_merge_prepared_dependency_projection",
    *PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDING_IDENTIFIERS,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER,
    PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER,
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_ERROR = (
    "MergePrepared authority identifiers must not be preprocessor macros"
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE = (
    "#if defined("
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS[0]
    + ") || \\\n"
    + "".join(
        f"    defined({identifier}) || \\\n"
        for identifier in (
            PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS[1:-1]
        )
    )
    + "    defined("
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS[-1]
    + ")\n"
    + '#error "'
    + PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_ERROR
    + '"\n#endif'
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_CALLBACK_DECLARATION = (
    "[[nodiscard]] bool validate_merge_prepared_prefix_type("
)
PRIVATE_HANDOFF_PUBLICATION_MERGE_PROTECTED_DEFINITION_IDENTIFIERS = (
    "validate_merge_prepared_envelope",
    "validate_merge_prepared_dependency_projection",
    PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION,
    PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION,
    "revalidate_exact_canonical_merge_started",
)
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
WORKER_PROCESS_FORBIDDEN_PROCESS_IDENTIFIERS = frozenset(
    (
        "_Fork",
        "vfork",
        "posix_spawnp",
        "waitid",
        "wait3",
        "wait4",
    )
    + ALTERNATE_PROCESS_EXECUTION_IDENTIFIERS
)
WORKER_PROCESS_TRANSPORT_FORBIDDEN_IDENTIFIERS = ("environ",)

# Code-token bans close indirect ambient-policy entrances that do not contain
# getenv/random_device themselves. The runtime mapper is deliberately a pure
# projection: it must not construct a legacy runtime object or invoke a basis
# helper whose overload could re-read process or host state.
DURABLE_FORBIDDEN_IDENTIFIERS = ("from_env",)
DURABLE_FORBIDDEN_CALLS = ("hardware_concurrency",)
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
MERGE_STREAM_WRITER_FILES = {
    "src/sieve/distributed_sieve_merge_writer_internal.cpp",
    "src/sieve/distributed_sieve_merge_writer_internal.hpp",
}
MERGE_STREAM_WRITER_FORBIDDEN_IDENTIFIERS = (
    "read_all",
    "read_range",
    "OOCPrivateLease",
    "OOCCleanupTransaction",
    "OOCPrivateHandoffPayloadBuilderV1",
    "capture_finalized_corpus_evidence",
    "finalize_and_publish_private_handoff",
    "finalize_and_publish_private_handoff_built",
    "abort_and_remove_owned_fresh_artifacts_noexcept",
    "remove_owned_artifacts_noexcept",
)
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
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
    WORKER_COORDINATOR_IMPLEMENTATION_FILE,
} | WORKER_EXECUTOR_BOUND_WORK_USE_SITE_FILES
WORK_PACKAGE_CARRIER_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerWorkPackageFileV1",
    "create_distributed_sieve_worker_work_package_file_v1",
)
WORK_PACKAGE_CARRIER_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
}
WORK_PACKAGE_RESIDUE_INSPECTOR_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerWorkPackageResidueInspectionRequestV1",
    "DistributedSieveWorkerWorkPackageResidueWitnessV1",
    "DistributedSieveWorkerWorkPackageResidueInspectionResultV1",
    "inspect_distributed_sieve_worker_work_package_residue_v1",
)
WORK_PACKAGE_RESIDUE_INSPECTOR_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
    "tests/test_distributed_sieve_resume.cpp",
}
WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_IDENTIFIER = (
    "inspect_distributed_sieve_worker_work_package_residue_v1_with_ops"
)
WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
}
WORK_PACKAGE_RESIDUE_INSPECTION_FUNCTIONS = (
    "validate_private_lease_attempt_inventory",
    "reconcile_worker_attempt_started",
)
WORK_PACKAGE_RESIDUE_INSPECTION_CALL = (
    "inspect_distributed_sieve_worker_work_package_residue_v1"
)
WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_IDENTIFIERS = (
    "DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1",
    "DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1",
    "DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1",
    "DistributedSieveWorkerWorkPackageResidueReconciliationResultV1",
    "DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1",
    "reconcile_distributed_sieve_worker_work_package_residue_v1",
)
WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
}
WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_IDENTIFIER = (
    "reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops"
)
WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
}
WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE = (
    "src/sieve/distributed_sieve_worker_work_package_file.cpp"
)
WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp"
)
WORK_PACKAGE_RESIDUE_RECONCILER_TEST_FILE = (
    "tests/test_distributed_sieve_worker_work_package_file.cpp"
)
WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION = "reconcile_worker_attempt_started"
WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL = (
    "reconcile_distributed_sieve_worker_work_package_residue_v1"
)
WORK_PACKAGE_RESIDUE_RECONCILIATION_FORBIDDEN_UNLINK_IDENTIFIERS = (
    "unlinkat",
    "private_lease_unlink_at",
    "unlink_at",
)
WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER = "unlink_at"
WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_ops_internal.hpp",
    "tests/test_distributed_sieve_worker_work_package_file.cpp",
}
WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER = "private_lease_unlink_at"
WAVE_STORE_PRIVATE_LEASE_UNLINK_CALL_FUNCTIONS = (
    "DistributedSieveFdPrivateLeaseRecoveryTarget::unlink_exact_marker",
    "DistributedSieveFdPrivateLeaseRecoveryTarget::remove_exact_empty_staging_directory",
)
WAVE_STORE_RAW_UNLINK_IDENTIFIER = "unlinkat"
WORK_PACKAGE_FIXED_LEAF_IDENTIFIER = (
    "DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1"
)
WORK_PACKAGE_FIXED_LEAF_LITERAL = ".gnfs-worker-work-package-v1"
WORK_PACKAGE_FIXED_LEAF_USE_SITE_ALLOWLIST = {
    WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE,
    WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE,
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
    WORK_PACKAGE_RESIDUE_RECONCILER_TEST_FILE,
    "tests/test_distributed_sieve_resume.cpp",
}
WORK_PACKAGE_FIXED_LEAF_PRODUCTION_USE_COUNTS = {
    WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE: (1, 1),
    WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE: (4, 0),
    WORKER_LAUNCHER_IMPLEMENTATION_FILE: (4, 0),
}
WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER = "run_residue_reconciliation"
WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER_CALL_FUNCTIONS = (
    WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL,
    WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_IDENTIFIER,
)
WORK_PACKAGE_CARRIER_UNLINK_CALL_FUNCTIONS = (
    WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER,
    "run_file_creation",
)
WORK_PACKAGE_CARRIER_OPS_INTERFACE_FILE = (
    "src/sieve/distributed_sieve_worker_work_package_file_ops_internal.hpp"
)
PRODUCTION_RAW_UNLINKAT_FUNCTIONS = {
    WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE: "unlink_at",
    WORKER_LAUNCHER_IMPLEMENTATION_FILE: WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER,
    "src/util/durable_immutable_record.cpp": "remove_exact_at",
    "include/gnfs/relation/ooc_cleanup_transaction.hpp": "remove_exact_private_handoff_pending",
    "include/gnfs/relation/ooc_relation_store.hpp":
        "remove_path_if_same_identity_at_noexcept",
}
WORK_PACKAGE_READER_USE_SITE_IDENTIFIER = "retained_reader_"
WORK_PACKAGE_READER_USE_SITE_ALLOWLIST = {
    "src/sieve/distributed_sieve_worker_work_package_file.cpp",
    "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
    WORKER_LAUNCHER_IMPLEMENTATION_FILE,
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


def find_code_identifier_tokens(
    text: str,
) -> list[tuple[str, CodeIdentifierUse]]:
    tokens: list[tuple[str, CodeIdentifierUse]] = []
    cursor = 0
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = skipped
            continue
        if text[cursor].isalpha() or text[cursor] == "_":
            start = cursor
            cursor += 1
            while cursor < len(text) and (
                text[cursor].isalnum() or text[cursor] == "_"
            ):
                cursor += 1
            tokens.append(
                (
                    text[start:cursor],
                    CodeIdentifierUse(
                        line=text.count("\n", 0, start) + 1,
                        offset=start,
                    ),
                )
            )
            continue
        cursor += 1
    return tokens


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
            if not (
                (before.isalnum() or before == "_") or (after.isalnum() or after == "_")
            ):
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


def find_exact_string_literal_uses(text: str, literal: str) -> list[CodeIdentifierUse]:
    uses: list[CodeIdentifierUse] = []
    cursor = 0
    while cursor < len(text):
        raw_end = _skip_raw_string(text, cursor)
        if raw_end is not None:
            if literal in text[cursor:raw_end]:
                uses.append(
                    CodeIdentifierUse(
                        line=text.count("\n", 0, cursor) + 1,
                        offset=cursor,
                    )
                )
            cursor = raw_end
            continue
        if text.startswith("//", cursor) or text.startswith("/*", cursor):
            skipped = _skip_non_code(text, cursor)
            cursor = len(text) if skipped is None else skipped
            continue
        if text[cursor] == '"':
            end = _skip_quoted(text, cursor, '"')
            if literal in text[cursor:end]:
                uses.append(
                    CodeIdentifierUse(
                        line=text.count("\n", 0, cursor) + 1,
                        offset=cursor,
                    )
                )
            cursor = end
            continue
        if text[cursor] == "'":
            cursor = _skip_quoted(text, cursor, "'")
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


def find_call_identifier_uses(text: str, identifier: str) -> list[CodeIdentifierUse]:
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


def _compact_cpp_code(text: str) -> str:
    """Remove C++ trivia while preserving quoted tokens."""

    compact: list[str] = []
    cursor = 0
    while cursor < len(text):
        raw_end = _skip_raw_string(text, cursor)
        if raw_end is not None:
            compact.append(text[cursor:raw_end])
            cursor = raw_end
            continue
        if text[cursor] in {'"', "'"}:
            quoted_end = _skip_quoted(text, cursor, text[cursor])
            compact.append(text[cursor:quoted_end])
            cursor = quoted_end
            continue
        if text.startswith("//", cursor) or text.startswith("/*", cursor):
            skipped = _skip_non_code(text, cursor)
            cursor = len(text) if skipped is None else skipped
            continue
        if text[cursor].isspace():
            cursor += 1
            continue
        compact.append(text[cursor])
        cursor += 1
    return "".join(compact)


def _mask_cpp_comments_and_literals(text: str) -> str:
    """Preserve source layout while blanking comments and quoted tokens."""

    masked = list(text)
    cursor = 0
    while cursor < len(text):
        skipped = _skip_non_code(text, cursor)
        if skipped is None:
            cursor += 1
            continue
        for index in range(cursor, skipped):
            if masked[index] not in {"\n", "\r"}:
                masked[index] = " "
        cursor = skipped
    return "".join(masked)


def _logical_preprocessor_text(text: str) -> str:
    """Expose active-looking directives after translation-phase line splicing."""

    return re.sub(r"\\\r?\n", "", _mask_cpp_comments_and_literals(text))


def _preprocessor_macro_records(
    text: str,
) -> list[tuple[str, str, str, int]]:
    logical = _logical_preprocessor_text(text)
    directives: list[tuple[str, str, str, int]] = []
    for match in re.finditer(
        r"(?m)^[ \t]*(?:#|%:)[ \t]*(define|undef)\b"
        r"[ \t]+([A-Za-z_][A-Za-z0-9_]*)([^\n]*)",
        logical,
    ):
        directives.append(
            (
                match.group(1),
                match.group(2),
                match.group(3),
                logical.count("\n", 0, match.start()) + 1,
            )
        )
    return directives


def _preprocessor_macro_directives(
    text: str,
) -> list[tuple[str, str, int]]:
    return [
        (directive, identifier, line)
        for directive, identifier, _, line in _preprocessor_macro_records(text)
    ]


def _preprocessor_conditional_directives(
    text: str,
) -> list[tuple[str, int]]:
    logical = _logical_preprocessor_text(text)
    return [
        (
            match.group(1),
            logical.count("\n", 0, match.start()) + 1,
        )
        for match in re.finditer(
            r"(?m)^[ \t]*(?:#|%:)[ \t]*"
            r"(if|ifdef|ifndef|elif|else|endif)\b",
            logical,
        )
    ]


def _preprocessor_directives(text: str) -> list[tuple[str, int]]:
    logical = _logical_preprocessor_text(text)
    return [
        (
            match.group(1),
            logical.count("\n", 0, match.start()) + 1,
        )
        for match in re.finditer(
            r"(?m)^[ \t]*(?:#|%:)[ \t]*"
            r"([A-Za-z_][A-Za-z0-9_]*)\b",
            logical,
        )
    ]


def _preprocessor_conditional_depth_at(text: str, offset: int) -> int:
    return len(_preprocessor_conditional_stack_at(text, offset))


def _preprocessor_conditional_stack_at(
    text: str, offset: int
) -> tuple[str, ...]:
    logical = _logical_preprocessor_text(text[:offset])
    stack: list[str] = []
    for match in re.finditer(
        r"(?m)^[ \t]*(?:#|%:)[ \t]*"
        r"(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)",
        logical,
    ):
        directive = match.group(1)
        suffix = _compact_cpp_code(match.group(2))
        if directive in {"if", "ifdef", "ifndef"}:
            stack.append(directive + suffix)
        elif directive in {"elif", "else"}:
            if not stack:
                stack.append("<conditional-underflow>")
            else:
                opening = stack[-1].split("|", 1)[0]
                stack[-1] = opening + "|" + directive + suffix
        elif stack:
            stack.pop()
        else:
            stack.append("<conditional-underflow>")
    return tuple(stack)


def _merge_prepared_macro_guard_matches(text: str) -> list[re.Match[str]]:
    defined_expression = (
        rf"defined[ \t]*\([ \t]*"
        rf"{re.escape(PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS[0])}"
        rf"[ \t]*\)"
    )
    for identifier in PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS[1:]:
        defined_expression += (
            rf"[ \t]*\|\|[ \t]*\\\r?\n[ \t]*"
            rf"defined[ \t]*\([ \t]*{re.escape(identifier)}[ \t]*\)"
        )
    pattern = re.compile(
        rf"(?m)^[ \t]*#[ \t]*if[ \t]+{defined_expression}[ \t]*\r?\n"
        rf"[ \t]*#[ \t]*error[ \t]+"
        rf'"{re.escape(PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_ERROR)}"'
        rf"[ \t]*\r?\n"
        rf"[ \t]*#[ \t]*endif[ \t]*$"
    )
    return list(pattern.finditer(text))


def _compact_cpp_tokens(text: str) -> str:
    """Remove trivia and literal payloads, retaining only active-looking tokens."""

    return _compact_cpp_code(_mask_cpp_comments_and_literals(text))


def _contains_conditional_preprocessor_directive(text: str) -> bool:
    if re.search(r"\\\r?\n", text) is not None:
        return True
    return bool(_preprocessor_conditional_directives(text))


def _active_brace_stack(text: str, offset: int) -> tuple[int, ...]:
    stack: list[int] = []
    cursor = 0
    limit = min(max(offset, 0), len(text))
    while cursor < limit:
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = min(skipped, limit)
            continue
        if text[cursor] == "{":
            stack.append(cursor)
        elif text[cursor] == "}" and stack:
            stack.pop()
        cursor += 1
    return tuple(stack)


def _statement_start_at_scope(
    text: str, offset: int, target_stack: tuple[int, ...] | None = None
) -> int:
    """Find the current statement start at one exact lexical brace scope."""

    if target_stack is None:
        target_stack = _active_brace_stack(text, offset)
    stack: list[int] = []
    parentheses = 0
    brackets = 0
    boundary = target_stack[-1] + 1 if target_stack else 0
    cursor = 0
    limit = min(max(offset, 0), len(text))
    while cursor < limit:
        skipped = _skip_non_code(text, cursor)
        if skipped is not None:
            cursor = min(skipped, limit)
            continue
        char = text[cursor]
        if char == "{":
            stack.append(cursor)
            if tuple(stack) == target_stack:
                boundary = cursor + 1
        elif char == "}":
            if stack:
                stack.pop()
            if tuple(stack) == target_stack and parentheses == 0 and brackets == 0:
                boundary = cursor + 1
        elif char == "(":
            parentheses += 1
        elif char == ")" and parentheses > 0:
            parentheses -= 1
        elif char == "[":
            brackets += 1
        elif char == "]" and brackets > 0:
            brackets -= 1
        elif (
            char == ";"
            and tuple(stack) == target_stack
            and parentheses == 0
            and brackets == 0
        ):
            boundary = cursor + 1
        cursor += 1
    return boundary


def _call_parentheses(
    text: str, use: CodeIdentifierUse, identifier: str
) -> tuple[int, int] | None:
    opening = _skip_call_trivia(text, use.offset + len(identifier))
    if opening >= len(text) or text[opening] != "(":
        return None
    closing = _matching_parenthesis(text, opening)
    if closing is None:
        return None
    return opening, closing


def _direct_call_statement_end(
    text: str, use: CodeIdentifierUse, identifier: str
) -> int | None:
    parentheses = _call_parentheses(text, use, identifier)
    if parentheses is None:
        return None
    _, closing = parentheses
    semicolon = _skip_call_trivia(text, closing + 1)
    if semicolon >= len(text) or text[semicolon] != ";":
        return None
    return semicolon + 1


def _function_declarator_terminator(
    text: str, use: CodeIdentifierUse, identifier: str
) -> int | None:
    parentheses = _call_parentheses(text, use, identifier)
    if parentheses is None:
        return None
    _, closing = parentheses
    cursor = _skip_call_trivia(text, closing + 1)
    if text.startswith("noexcept", cursor):
        after = cursor + len("noexcept")
        boundary = text[after] if after < len(text) else ""
        if boundary.isalnum() or boundary == "_":
            return None
        cursor = _skip_call_trivia(text, after)
        if cursor < len(text) and text[cursor] == "(":
            noexcept_closing = _matching_parenthesis(text, cursor)
            if noexcept_closing is None:
                return None
            cursor = _skip_call_trivia(text, noexcept_closing + 1)
    return cursor


def _function_definition_spans(
    text: str, identifier: str
) -> list[tuple[int, int]]:
    spans: list[tuple[int, int]] = []
    for use in find_code_identifier_uses(text, identifier):
        opening = _function_declarator_terminator(text, use, identifier)
        if opening is None or opening >= len(text) or text[opening] != "{":
            continue
        closing = _matching_brace(text, opening)
        if closing is not None:
            spans.append((use.offset, closing + 1))
    return spans


def _merge_prepared_protected_interval(
    text: str,
) -> tuple[int, int] | None:
    definition_spans = [
        _function_definition_spans(text, identifier)
        for identifier in (
            PRIVATE_HANDOFF_PUBLICATION_MERGE_PROTECTED_DEFINITION_IDENTIFIERS
        )
    ]
    if any(len(spans) != 1 for spans in definition_spans):
        return None
    ordered_spans = [spans[0] for spans in definition_spans]
    if [span[0] for span in ordered_spans] != sorted(
        span[0] for span in ordered_spans
    ):
        return None
    return ordered_spans[0][0], ordered_spans[-1][1]


def _merge_prepared_protected_code_tokens(text: str) -> set[str]:
    interval = _merge_prepared_protected_interval(text)
    if interval is None:
        return set()
    protected = _mask_cpp_comments_and_literals(text[interval[0] : interval[1]])
    return set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", protected))


def _class_definition_body_span(
    text: str, class_name: str
) -> tuple[int, int] | None:
    pattern = re.compile(
        rf"\b(?:class|struct)\s+{re.escape(class_name)}\b[^;{{]*{{",
        re.MULTILINE,
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        return None
    opening = matches[0].end() - 1
    closing = _matching_brace(text, opening)
    if closing is None:
        return None
    return opening + 1, closing


def _enum_class_definition_body_span(
    text: str, enum_name: str
) -> tuple[int, int] | None:
    pattern = re.compile(
        rf"\benum\s+class\s+{re.escape(enum_name)}\b[^;{{]*{{",
        re.MULTILINE,
    )
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        return None
    opening = matches[0].end() - 1
    closing = _matching_brace(text, opening)
    if closing is None:
        return None
    return opening + 1, closing


def _forbidden_control_scope_introducer(
    text: str, opening: int, *, forbid_for: bool = False
) -> str | None:
    parent_stack = _active_brace_stack(text, opening)
    start = _statement_start_at_scope(text, opening, parent_stack)
    introducer = _compact_cpp_code(text[start:opening])
    if re.search(
        r"(?:^|[=(:,?])\[[^\]]*\]"
        r"(?:\([^{}]*\))?(?:mutable)?(?:noexcept(?:\([^{}]*\))?)?"
        r"(?:->[A-Za-z_][^{}]*)?$",
        introducer,
    ):
        return "lambda"
    keywords = (
        "if(",
        "ifconstexpr(",
        "switch(",
        "while(",
        "catch(",
        "else",
        "do",
    ) + (("for(",) if forbid_for else ())
    for keyword in keywords:
        if introducer.startswith(keyword):
            return keyword.rstrip("(")
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
        errors.append(
            (use.line, f"{function_name} is not followed by a parameter list")
        )
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


def find_function_definition_body(
    text: str, function_name: str
) -> tuple[str | None, int, list[tuple[int, str]]]:
    """Find one definition while ignoring ordinary calls of the same identifier."""

    candidates: list[tuple[CodeIdentifierUse, int]] = []
    for use in find_code_identifier_uses(text, function_name):
        opening = _skip_call_trivia(text, use.offset + len(function_name))
        if opening >= len(text) or text[opening] != "(":
            continue
        closing = _matching_parenthesis(text, opening)
        if closing is None:
            continue
        cursor = _skip_call_trivia(text, closing + 1)
        while True:
            matched_qualifier = False
            for qualifier in ("const", "volatile"):
                if not text.startswith(qualifier, cursor):
                    continue
                after = cursor + len(qualifier)
                boundary = text[after] if after < len(text) else ""
                if boundary.isalnum() or boundary == "_":
                    continue
                cursor = _skip_call_trivia(text, after)
                matched_qualifier = True
                break
            if matched_qualifier:
                continue
            if text.startswith("&&", cursor):
                cursor = _skip_call_trivia(text, cursor + 2)
                continue
            if cursor < len(text) and text[cursor] == "&":
                cursor = _skip_call_trivia(text, cursor + 1)
                continue
            break
        if text.startswith("noexcept", cursor):
            after = cursor + len("noexcept")
            boundary = text[after] if after < len(text) else ""
            if not (boundary.isalnum() or boundary == "_"):
                cursor = _skip_call_trivia(text, after)
                if cursor < len(text) and text[cursor] == "(":
                    noexcept_closing = _matching_parenthesis(text, cursor)
                    if noexcept_closing is None:
                        continue
                    cursor = _skip_call_trivia(text, noexcept_closing + 1)
        while True:
            matched_specifier = False
            for specifier in ("override", "final"):
                if not text.startswith(specifier, cursor):
                    continue
                after = cursor + len(specifier)
                boundary = text[after] if after < len(text) else ""
                if boundary.isalnum() or boundary == "_":
                    continue
                cursor = _skip_call_trivia(text, after)
                matched_specifier = True
                break
            if not matched_specifier:
                break
        if cursor < len(text) and text[cursor] == "{":
            candidates.append((use, cursor))

    if len(candidates) != 1:
        return (
            None,
            0,
            [
                (
                    1,
                    f"expected exactly one {function_name} definition, "
                    f"found {len(candidates)} definitions",
                )
            ],
        )

    use, body_opening = candidates[0]
    body_closing = _matching_brace(text, body_opening)
    if body_closing is None:
        return (
            None,
            0,
            [(use.line, f"{function_name} has an unterminated function body")],
        )
    body_line_offset = text.count("\n", 0, body_opening + 1)
    return text[body_opening + 1 : body_closing], body_line_offset, []


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
        self.merge_prepared_protected_tokens: set[str] = set()

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

    def validate_durable_ambient_api_uses(self, relative: str, text: str) -> None:
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

        if relative in MERGE_STREAM_WRITER_FILES:
            for identifier in MERGE_STREAM_WRITER_FORBIDDEN_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        f"streaming merge writer must not use authority/full-corpus API "
                        f"{identifier}",
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

        for identifier, use in find_code_identifier_tokens(text):
            if identifier in WORKER_PROCESS_FORBIDDEN_PROCESS_IDENTIFIERS:
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

    def validate_work_package_carrier_use_site(self, relative: str, text: str) -> None:
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

    def validate_work_package_residue_inspector_use_site(
        self, relative: str, text: str
    ) -> None:
        if relative not in WORK_PACKAGE_RESIDUE_INSPECTOR_USE_SITE_ALLOWLIST:
            for identifier in WORK_PACKAGE_RESIDUE_INSPECTOR_USE_SITE_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "named work-package residue inspector is not "
                        f"allowlisted: {identifier}",
                    )
        if relative not in WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_ALLOWLIST:
            for use in find_code_identifier_uses(
                text, WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_IDENTIFIER
            ):
                self.fail(
                    relative,
                    use.line,
                    "test-only named work-package residue inspector is not "
                    f"allowlisted: {WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_IDENTIFIER}",
                )

    def validate_work_package_residue_inspection_body(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_LAUNCHER_IMPLEMENTATION_FILE:
            return

        all_uses = find_code_identifier_uses(text, WORK_PACKAGE_RESIDUE_INSPECTION_CALL)
        allowed_use_count = 0
        for function_name in WORK_PACKAGE_RESIDUE_INSPECTION_FUNCTIONS:
            body, body_line_offset, body_errors = find_function_definition_body(
                text, function_name
            )
            for line, error in body_errors:
                self.fail(relative, line, error)
            if body is None:
                continue

            body_uses = find_code_identifier_uses(
                body, WORK_PACKAGE_RESIDUE_INSPECTION_CALL
            )
            body_calls = find_call_identifier_uses(
                body, WORK_PACKAGE_RESIDUE_INSPECTION_CALL
            )
            allowed_use_count += len(body_uses)
            if len(body_uses) != 1 or len(body_calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{function_name} must contain exactly "
                    f"1 direct {WORK_PACKAGE_RESIDUE_INSPECTION_CALL} call, found "
                    f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                )
        if len(all_uses) != allowed_use_count:
            self.fail(
                relative,
                1,
                f"all {WORK_PACKAGE_RESIDUE_INSPECTION_CALL} uses must remain inside "
                f"{' and '.join(WORK_PACKAGE_RESIDUE_INSPECTION_FUNCTIONS)}",
            )

    def validate_work_package_residue_reconciler_use_site(
        self, relative: str, text: str
    ) -> None:
        if relative not in WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_ALLOWLIST:
            for identifier in WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "named work-package residue reconciliation authority is not "
                        f"allowlisted: {identifier}",
                    )
        if relative not in WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_ALLOWLIST:
            for use in find_code_identifier_uses(
                text, WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_IDENTIFIER
            ):
                self.fail(
                    relative,
                    use.line,
                    "test-only named work-package residue reconciler is not "
                    f"allowlisted: {WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_IDENTIFIER}",
                )

    def validate_work_package_residue_reconciler_definition_boundary(
        self, relative: str, text: str
    ) -> None:
        identifiers = (
            WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL,
            WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_IDENTIFIER,
        )
        if relative == WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE:
            for identifier in identifiers:
                uses = find_code_identifier_uses(text, identifier)
                calls = find_call_identifier_uses(text, identifier)
                if len(uses) != 1 or len(calls) != 1:
                    self.fail(
                        relative,
                        1,
                        f"carrier interface must contain exactly 1 declaration-shaped "
                        f"{identifier} identifier, found {len(uses)} identifiers and "
                        f"{len(calls)} call-shaped uses",
                    )
            return

        if relative == WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE:
            for identifier in identifiers:
                body, _, body_errors = find_function_definition_body(text, identifier)
                for line, error in body_errors:
                    self.fail(relative, line, error)
                uses = find_code_identifier_uses(text, identifier)
                if body is not None and len(uses) != 1:
                    self.fail(
                        relative,
                        1,
                        f"carrier implementation must contain only the exact "
                        f"{identifier} definition, found {len(uses)} identifiers",
                    )
            return

        if relative == WORK_PACKAGE_RESIDUE_RECONCILER_TEST_FILE:
            for identifier in identifiers:
                for use in find_non_call_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        f"dedicated carrier test must use {identifier} only as a "
                        "direct call",
                    )

    def validate_work_package_fixed_leaf_use_site(
        self, relative: str, text: str
    ) -> None:
        identifier_uses = find_code_identifier_uses(
            text, WORK_PACKAGE_FIXED_LEAF_IDENTIFIER
        )
        literal_uses = find_exact_string_literal_uses(
            text, WORK_PACKAGE_FIXED_LEAF_LITERAL
        )
        if relative not in WORK_PACKAGE_FIXED_LEAF_USE_SITE_ALLOWLIST:
            for use in identifier_uses:
                self.fail(
                    relative,
                    use.line,
                    "fixed work-package leaf identifier is not allowlisted",
                )
            for use in literal_uses:
                self.fail(
                    relative,
                    use.line,
                    "fixed work-package leaf literal is not allowlisted",
                )
            return

        expected = WORK_PACKAGE_FIXED_LEAF_PRODUCTION_USE_COUNTS.get(relative)
        if expected is None:
            return
        expected_identifiers, expected_literals = expected
        if (
            len(identifier_uses) != expected_identifiers
            or len(literal_uses) != expected_literals
        ):
            self.fail(
                relative,
                1,
                "fixed work-package leaf production use count changed: "
                f"expected {expected_identifiers} identifiers and "
                f"{expected_literals} literals, found {len(identifier_uses)} "
                f"identifiers and {len(literal_uses)} literals",
            )

    def validate_production_raw_unlinkat_authority(
        self, relative: str, text: str
    ) -> None:
        if not relative.startswith(("include/", "src/")):
            return
        raw_uses = find_code_identifier_uses(text, WAVE_STORE_RAW_UNLINK_IDENTIFIER)
        allowed_function = PRODUCTION_RAW_UNLINKAT_FUNCTIONS.get(relative)
        if allowed_function is None:
            for use in raw_uses:
                self.fail(
                    relative,
                    use.line,
                    "production raw unlinkat authority is not allowlisted",
                )
            return

        body, body_line_offset, body_errors = find_function_definition_body(
            text, allowed_function
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return
        body_uses = find_code_identifier_uses(body, WAVE_STORE_RAW_UNLINK_IDENTIFIER)
        body_calls = find_call_identifier_uses(body, WAVE_STORE_RAW_UNLINK_IDENTIFIER)
        if len(raw_uses) != 1 or len(body_uses) != 1 or len(body_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"{allowed_function} must own the sole direct production "
                f"{WAVE_STORE_RAW_UNLINK_IDENTIFIER} call, found "
                f"{len(raw_uses)} file identifiers, {len(body_uses)} body "
                f"identifiers, and {len(body_calls)} body calls",
            )

    def validate_work_package_carrier_unlink_authority(
        self, relative: str, text: str
    ) -> None:
        if relative == WORK_PACKAGE_CARRIER_OPS_INTERFACE_FILE:
            uses = find_code_identifier_uses(
                text, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
            )
            calls = find_call_identifier_uses(
                text, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
            )
            if len(uses) != 1 or len(calls) != 1:
                self.fail(
                    relative,
                    1,
                    "carrier ops interface must contain exactly 1 "
                    f"declaration-shaped "
                    f"{WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER} identifier, "
                    f"found {len(uses)} identifiers and {len(calls)} "
                    "call-shaped uses",
                )
            return
        if relative != WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE:
            return

        all_runner_uses = find_code_identifier_uses(
            text, WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER
        )
        allowed_runner_uses = 0
        for function_name in WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER_CALL_FUNCTIONS:
            body, body_line_offset, body_errors = find_function_definition_body(
                text, function_name
            )
            for line, error in body_errors:
                self.fail(relative, line, error)
            if body is None:
                continue
            body_uses = find_code_identifier_uses(
                body, WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER
            )
            body_calls = find_call_identifier_uses(
                body, WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER
            )
            allowed_runner_uses += len(body_uses)
            if len(body_uses) != 1 or len(body_calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{function_name} must contain exactly 1 direct "
                    f"{WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER} call, found "
                    f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                )
        if len(all_runner_uses) != allowed_runner_uses + 1:
            self.fail(
                relative,
                1,
                f"all {WORK_PACKAGE_RESIDUE_INTERNAL_RUNNER} calls must remain "
                "inside the exact carrier entry points",
            )

        all_unlink_uses = find_code_identifier_uses(
            text, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
        )
        allowed_unlink_uses = 0
        for function_name in WORK_PACKAGE_CARRIER_UNLINK_CALL_FUNCTIONS:
            body, body_line_offset, body_errors = find_function_definition_body(
                text, function_name
            )
            for line, error in body_errors:
                self.fail(relative, line, error)
            if body is None:
                continue
            body_uses = find_code_identifier_uses(
                body, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
            )
            body_calls = find_call_identifier_uses(
                body, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
            )
            allowed_unlink_uses += len(body_uses)
            if len(body_uses) != 1 or len(body_calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{function_name} must contain exactly 1 direct "
                    f"{WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER} call, found "
                    f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                )
        if len(all_unlink_uses) != allowed_unlink_uses + 1:
            self.fail(
                relative,
                1,
                f"all carrier {WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER} calls "
                "must remain inside creation and residue reconciliation",
            )

        raw_uses = find_code_identifier_uses(text, WAVE_STORE_RAW_UNLINK_IDENTIFIER)
        raw_calls = find_call_identifier_uses(text, WAVE_STORE_RAW_UNLINK_IDENTIFIER)
        if len(raw_uses) != 1 or len(raw_calls) != 1:
            self.fail(
                relative,
                1,
                f"carrier must contain exactly 1 direct "
                f"{WAVE_STORE_RAW_UNLINK_IDENTIFIER} call, found "
                f"{len(raw_uses)} identifiers and {len(raw_calls)} calls",
            )

    def validate_work_package_residue_reconciliation_body(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_LAUNCHER_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_definition_body(
            text, WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        all_uses = find_code_identifier_uses(
            text, WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL
        )
        body_uses = find_code_identifier_uses(
            body, WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL
        )
        body_calls = find_call_identifier_uses(
            body, WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL
        )
        if len(all_uses) != len(body_uses):
            self.fail(
                relative,
                1,
                f"all {WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL} uses must remain inside "
                f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION}",
            )
        if len(body_uses) != 1 or len(body_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION} must contain exactly "
                f"1 direct {WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL} call, found "
                f"{len(body_uses)} identifiers and {len(body_calls)} calls",
            )
        for (
            identifier
        ) in WORK_PACKAGE_RESIDUE_RECONCILIATION_FORBIDDEN_UNLINK_IDENTIFIERS:
            for use in find_code_identifier_uses(body, identifier):
                self.fail(
                    relative,
                    body_line_offset + use.line,
                    f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION} must not bypass the "
                    f"carrier through {identifier}",
                )

    def validate_wave_store_private_lease_unlink_authority(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_LAUNCHER_IMPLEMENTATION_FILE:
            for use in find_code_identifier_uses(
                text, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
            ):
                self.fail(
                    relative,
                    use.line,
                    "WaveStore private-lease unlink helper is not allowlisted: "
                    f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER}",
                )
            return

        helper_body, helper_line_offset, helper_errors = find_function_definition_body(
            text, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
        )
        for line, error in helper_errors:
            self.fail(relative, line, error)
        if helper_body is not None:
            all_raw_uses = find_code_identifier_uses(
                text, WAVE_STORE_RAW_UNLINK_IDENTIFIER
            )
            helper_raw_uses = find_code_identifier_uses(
                helper_body, WAVE_STORE_RAW_UNLINK_IDENTIFIER
            )
            helper_raw_calls = find_call_identifier_uses(
                helper_body, WAVE_STORE_RAW_UNLINK_IDENTIFIER
            )
            if len(all_raw_uses) != len(helper_raw_uses):
                self.fail(
                    relative,
                    1,
                    f"all raw {WAVE_STORE_RAW_UNLINK_IDENTIFIER} uses must remain "
                    f"inside {WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER}",
                )
            if len(helper_raw_uses) != 1 or len(helper_raw_calls) != 1:
                self.fail(
                    relative,
                    helper_line_offset + 1,
                    f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER} must contain exactly "
                    f"1 direct {WAVE_STORE_RAW_UNLINK_IDENTIFIER} call, found "
                    f"{len(helper_raw_uses)} identifiers and "
                    f"{len(helper_raw_calls)} calls",
                )

        all_helper_uses = find_code_identifier_uses(
            text, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
        )
        allowed_call_uses = 0
        for function_name in WAVE_STORE_PRIVATE_LEASE_UNLINK_CALL_FUNCTIONS:
            body, body_line_offset, body_errors = find_function_definition_body(
                text, function_name
            )
            for line, error in body_errors:
                self.fail(relative, line, error)
            if body is None:
                continue
            body_uses = find_code_identifier_uses(
                body, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
            )
            body_calls = find_call_identifier_uses(
                body, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
            )
            allowed_call_uses += len(body_uses)
            if len(body_uses) != 1 or len(body_calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{function_name} must contain exactly 1 direct "
                    f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER} call, found "
                    f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                )
        if len(all_helper_uses) != allowed_call_uses + 1:
            self.fail(
                relative,
                1,
                f"all {WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER} calls must remain "
                "inside the exact private-lease recovery bodies",
            )

    def validate_raw_work_package_fixed_leaf_unlink(
        self, relative: str, text: str
    ) -> None:
        if relative in WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_ALLOWLIST:
            return

        for use in find_code_identifier_uses(
            text, WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER
        ):
            self.fail(
                relative,
                use.line,
                "raw fixed-leaf unlink seam is carrier-only: "
                f"{WORK_PACKAGE_RAW_FIXED_LEAF_UNLINK_IDENTIFIER}",
            )

        fixed_leaf_in_file = (
            bool(find_code_identifier_uses(text, WORK_PACKAGE_FIXED_LEAF_IDENTIFIER))
            or WORK_PACKAGE_FIXED_LEAF_LITERAL in text
        )
        if fixed_leaf_in_file and relative != WORKER_LAUNCHER_IMPLEMENTATION_FILE:
            for use in find_code_identifier_uses(
                text, WAVE_STORE_RAW_UNLINK_IDENTIFIER
            ):
                opening = _skip_call_trivia(
                    text, use.offset + len(WAVE_STORE_RAW_UNLINK_IDENTIFIER)
                )
                if opening < len(text) and text[opening] == "(":
                    closing = _matching_parenthesis(text, opening)
                    if closing is not None:
                        arguments = text[opening + 1 : closing]
                        if (
                            find_code_identifier_uses(
                                arguments, WORK_PACKAGE_FIXED_LEAF_IDENTIFIER
                            )
                            or WORK_PACKAGE_FIXED_LEAF_LITERAL in arguments
                        ):
                            continue
                self.fail(
                    relative,
                    use.line,
                    "fixed work-package leaf cannot share raw unlink authority "
                    f"outside the carrier: {WAVE_STORE_RAW_UNLINK_IDENTIFIER}",
                )
            for use in find_code_identifier_uses(
                text, WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER
            ):
                self.fail(
                    relative,
                    use.line,
                    "fixed work-package leaf cannot share raw unlink authority "
                    f"outside the carrier: "
                    f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER}",
                )

        for use in find_call_identifier_uses(text, "unlinkat"):
            opening = _skip_call_trivia(text, use.offset + len("unlinkat"))
            closing = _matching_parenthesis(text, opening)
            if closing is None:
                continue
            arguments = text[opening + 1 : closing]
            fixed_leaf_identifier = find_code_identifier_uses(
                arguments, WORK_PACKAGE_FIXED_LEAF_IDENTIFIER
            )
            if fixed_leaf_identifier or WORK_PACKAGE_FIXED_LEAF_LITERAL in arguments:
                self.fail(
                    relative,
                    use.line,
                    "raw fixed-leaf unlinkat bypass is carrier-only",
                )

    def validate_work_package_reader_use_site(self, relative: str, text: str) -> None:
        if relative in WORK_PACKAGE_READER_USE_SITE_ALLOWLIST:
            return
        for use in find_code_identifier_uses(
            text, WORK_PACKAGE_READER_USE_SITE_IDENTIFIER
        ):
            self.fail(
                relative,
                use.line,
                "anonymous work-package reader authority is not "
                f"receipt-gated/allowlisted: {WORK_PACKAGE_READER_USE_SITE_IDENTIFIER}",
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

    def validate_worker_entry_use_site(self, relative: str, text: str) -> None:
        if relative in WORKER_ENTRY_USE_SITE_ALLOWLIST:
            return
        for identifier in WORKER_ENTRY_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "single-use worker-entry capability use site is not "
                    f"allowlisted: {identifier}",
                )

    def validate_worker_writer_use_site(self, relative: str, text: str) -> None:
        if relative not in WORKER_WRITER_USE_SITE_ALLOWLIST:
            for identifier in WORKER_WRITER_USE_SITE_IDENTIFIERS:
                if relative in WORKER_WRITER_IDENTIFIER_EXCEPTIONS.get(
                    identifier, set()
                ):
                    continue
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "single-use worker-writer capability use site is not "
                        f"allowlisted: {identifier}",
                    )
        if relative not in WORKER_WRITER_BRIDGE_ALLOWLIST:
            for identifier in WORKER_WRITER_BRIDGE_IDENTIFIERS:
                if relative in WORKER_WRITER_BRIDGE_IDENTIFIER_EXCEPTIONS.get(
                    identifier, set()
                ):
                    continue
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "worker-writer private bridge use site is not "
                        f"allowlisted: {identifier}",
                    )
        for identifier in WORKER_HANDOFF_BRIDGE_IDENTIFIERS:
            if relative not in WORKER_HANDOFF_BRIDGE_IDENTIFIER_ALLOWLISTS[
                identifier
            ]:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "worker-handoff evidence bridge use site is not "
                        f"allowlisted: {identifier}",
                    )
        if relative not in RAW_PRIVATE_HANDOFF_PUBLISHER_ALLOWLIST:
            for use in find_code_identifier_uses(
                text, RAW_PRIVATE_HANDOFF_PUBLISHER_IDENTIFIER
            ):
                self.fail(
                    relative,
                    use.line,
                    "raw private-handoff publisher use is not allowlisted",
                )
        if relative not in MERGE_WRITER_EXACT_APPEND_BATCH_ALLOWLIST:
            for use in find_code_identifier_uses(
                text, MERGE_WRITER_EXACT_APPEND_BATCH_IDENTIFIER
            ):
                self.fail(
                    relative,
                    use.line,
                    "merge-writer exact append batch use is not allowlisted",
                )
        for identifier, allowlist in (
            MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS.items()
        ):
            if relative in allowlist:
                continue
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "merge-writer trusted-test hook use is not allowlisted: "
                    f"{identifier}",
                )

    def validate_borrowed_base_lock_bridge(
        self, relative: str, text: str
    ) -> None:
        for identifier, expected_by_file in (
            BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_USE_COUNTS.items()
        ):
            uses = find_code_identifier_uses(text, identifier)
            expected = expected_by_file.get(relative, 0)
            if len(uses) != expected:
                line = uses[0].line if uses else 1
                self.fail(
                    relative,
                    line,
                    "borrowed BaseLock bridge identifier count is not closed: "
                    f"{identifier} expected {expected}, found {len(uses)}",
                )

        if relative == BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER:
            token = "OOCPrivateHandoffBorrowedBaseLockV1"
            token_uses = find_code_identifier_uses(text, token)
            token_forward = list(
                re.finditer(
                    rf"(?m)^\s*class\s+{re.escape(token)}\s*;",
                    text,
                )
            )
            token_friend = list(
                re.finditer(
                    rf"\bfriend\s+class\s+{re.escape(token)}\s*;",
                    text,
                )
            )
            token_allowed_offsets = {
                match.start() + match.group(0).rfind(token)
                for match in (*token_forward, *token_friend)
            }
            if (
                len(token_uses) != 2
                or len(token_forward) != 1
                or len(token_friend) != 1
                or {use.offset for use in token_uses} != token_allowed_offsets
            ):
                self.fail(
                    relative,
                    token_uses[0].line if token_uses else 1,
                    "public cleanup header may expose borrowed BaseLock authority "
                    "only as one forward declaration and one private friend",
                )

            builder = "OOCPrivateHandoffAdoptionBuilderV1"
            builder_uses = find_code_identifier_uses(text, builder)
            builder_forward = list(
                re.finditer(
                    rf"(?m)^\s*class\s+{re.escape(builder)}\s*;",
                    text,
                )
            )
            builder_friends = list(
                re.finditer(
                    rf"\bfriend\s+class\s+ooc_cleanup_detail::"
                    rf"{re.escape(builder)}\s*;",
                    text,
                )
            )
            builder_allowed_offsets = {
                match.start() + match.group(0).rfind(builder)
                for match in (*builder_forward, *builder_friends)
            }
            if (
                len(builder_uses) != 3
                or len(builder_forward) != 1
                or len(builder_friends) != 2
                or {use.offset for use in builder_uses} != builder_allowed_offsets
            ):
                self.fail(
                    relative,
                    builder_uses[0].line if builder_uses else 1,
                    "private-handoff adoption builder must remain one forward "
                    "declaration and two exact private friends",
                )

            release_body, release_line_offset, release_errors = (
                find_function_definition_body(
                    text, BORROWED_BASE_LOCK_RELEASE_FUNCTION
                )
            )
            for line, error in release_errors:
                self.fail(relative, line, error)
            compact_text = _compact_cpp_code(text)
            if (
                release_body is None
                or _compact_cpp_code(release_body)
                != BORROWED_BASE_LOCK_RELEASE_BODY
                or compact_text.count(BORROWED_BASE_LOCK_RETAINED_FLOCK_FRAGMENT)
                != 1
                or compact_text.count("::flock(descriptor,") != 1
                or compact_text.count(
                    "identity_=expected_lock_identity;descriptor_=descriptor;"
                )
                != 1
                or len(
                    find_code_identifier_uses(
                        text, "AdoptInheritedOpenFileDescription"
                    )
                )
                != 2
                or find_code_identifier_uses(text, "LOCK_UN")
            ):
                self.fail(
                    relative,
                    release_line_offset + 1,
                    "inherited BaseLock must prove the retained same-OFD lock "
                    "exactly once and retain an exact close-only destructor",
                )
            return

        if relative == BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE:
            class_span = _class_definition_body_span(
                text, "OOCPrivateHandoffBorrowedBaseLockV1"
            )
            compact_text = _compact_cpp_code(text)
            declaration = (
                "[[nodiscard]]OOCPrivateHandoffAdoptionResult"
                "adopt_private_handoff_with_borrowed_base_lock_v1("
                "conststd::filesystem::path&base_path,"
                "OOCPrivateHandoffBorrowedBaseLockV1&&borrowed,"
                "OOCPrivateHandoffAdoptionTestHookshooks={})noexcept;"
            )
            if (
                class_span is None
                or _compact_cpp_code(text[class_span[0] : class_span[1]])
                != BORROWED_BASE_LOCK_TOKEN_CLASS_BODY
                or compact_text.count(declaration) != 1
            ):
                self.fail(
                    relative,
                    1 if class_span is None else text.count("\n", 0, class_span[0]) + 1,
                    "borrowed BaseLock token interface must remain the exact "
                    "private one-shot class and source-private adoption declaration",
                )
            return

        if relative == BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE:
            mint_body, mint_line_offset, mint_errors = (
                find_function_definition_body(
                    text, BORROWED_BASE_LOCK_WAVE_MINT_FUNCTION
                )
            )
            for line, error in mint_errors:
                self.fail(relative, line, error)
            if (
                mint_body is None
                or _compact_cpp_code(mint_body)
                != BORROWED_BASE_LOCK_WAVE_MINT_BODY
            ):
                self.fail(
                    relative,
                    mint_line_offset + 1,
                    "WaveStore must mint the borrowed BaseLock token only in "
                    "the exact owned-process adoption method",
                )
            return

        if relative != BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE:
            return

        consume_body, consume_line_offset, consume_errors = (
            find_function_definition_body(text, BORROWED_BASE_LOCK_CONSUME_FUNCTION)
        )
        for line, error in consume_errors:
            self.fail(relative, line, error)
        if consume_body is not None:
            compact_consume = _compact_cpp_code(consume_body)
            if (
                compact_consume.count(
                    BORROWED_BASE_LOCK_CONSTRUCTION_CHAIN_FRAGMENT
                )
                != 1
                or not compact_consume.endswith(
                    BORROWED_BASE_LOCK_CONSTRUCTION_CHAIN_FRAGMENT + "#endif"
                )
                or compact_consume.count("return") != 1
                or find_code_identifier_uses(consume_body, "flock")
                or find_code_identifier_uses(consume_body, "LOCK_UN")
            ):
                self.fail(
                    relative,
                    consume_line_offset + 1,
                    "borrowed BaseLock consumption must duplicate exactly one "
                    "close-on-exec descriptor into inherited-OFD construction "
                    "without flock or LOCK_UN",
                )

        adoption_body, adoption_line_offset, adoption_errors = (
            find_function_definition_body(
                text, BORROWED_BASE_LOCK_ADOPTION_FUNCTION
            )
        )
        for line, error in adoption_errors:
            self.fail(relative, line, error)
        if adoption_body is not None:
            compact_adoption = _compact_cpp_code(adoption_body)
            if compact_adoption != BORROWED_BASE_LOCK_ADOPTION_BODY:
                self.fail(
                    relative,
                    adoption_line_offset + 1,
                    "borrowed BaseLock adoption must consume the exact one-shot "
                    "token once inside the common adoption path",
                )

    def validate_consumed_canonical_adoption_bridge(
        self, relative: str, text: str
    ) -> None:
        for identifier, expected_by_file in (
            CONSUMED_CANONICAL_ADOPTION_IDENTIFIER_USE_COUNTS.items()
        ):
            if relative in CONSUMED_CANONICAL_ADOPTION_FLEXIBLE_TEST_USES.get(
                identifier, set()
            ):
                continue
            uses = find_code_identifier_uses(text, identifier)
            expected = expected_by_file.get(relative, 0)
            if len(uses) != expected:
                self.fail(
                    relative,
                    uses[0].line if uses else 1,
                    "consumed-canonical adoption identifier count is not closed: "
                    f"{identifier} expected {expected}, found {len(uses)}",
                )

        if relative.startswith("include/gnfs/"):
            for identifier in CONSUMED_CANONICAL_ADOPTION_PUBLIC_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "consumed-canonical adoption authority must not leak into "
                        f"public headers: {identifier}",
                    )
            return

        compact_text = _compact_cpp_code(text)
        if relative == CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE:
            if (
                compact_text.count(
                    CONSUMED_CANONICAL_ADOPTION_RELATION_DECLARATION
                )
                != 1
                or compact_text.count(CONSUMED_CANONICAL_ADOPTION_RELATION_FRIEND)
                != 1
            ):
                self.fail(
                    relative,
                    1,
                    "consumed-canonical adoption entry must retain its one exact "
                    "rvalue-only source-private declaration and private friend",
                )
            if (
                compact_text.count(
                    CONSUMED_CANONICAL_READER_ADOPTION_RELATION_DECLARATION
                )
                != 1
                or compact_text.count(
                    CONSUMED_CANONICAL_READER_ADOPTION_RELATION_FRIEND
                )
                != 2
            ):
                self.fail(
                    relative,
                    1,
                    "transactional reader adoption must retain one exact lvalue "
                    "permit declaration and its two private friends",
                )
            revalidator_span = _class_definition_body_span(
                text, CONSUMED_CANONICAL_READER_REVALIDATOR
            )
            if revalidator_span is None:
                self.fail(
                    relative,
                    1,
                    "transactional reader adoption requires its private trusted "
                    "revalidator capability",
                )
            else:
                revalidator_body = _compact_cpp_code(
                    text[revalidator_span[0] : revalidator_span[1]]
                )
                required_revalidator_fragments = (
                    "private:explicitPrivateHandoffPublicationAdoptionRevalidatorV1("
                    "Validatevalidate,void*context)noexcept;",
                    "Validatevalidate_=nullptr;void*context_=nullptr;"
                    "std::uint64_tcreator_process_id_=0;",
                    "friendclassgnfs::sieve::distributed_sieve_resume_detail::"
                    "MergePreparedAdmissionRevalidatorAuthorityV1;",
                    "friendclassPrivateHandoffPublicationAdoptionRevalidatorTestAuthorityV1;",
                    CONSUMED_CANONICAL_READER_ADOPTION_RELATION_FRIEND,
                )
                if any(
                    revalidator_body.count(fragment) != 1
                    for fragment in required_revalidator_fragments
                ) or revalidator_body.count("friendclass") != 2:
                    self.fail(
                        relative,
                        text.count("\n", 0, revalidator_span[0]) + 1,
                        "only the WaveStore trusted authority and test authority "
                        "may mint the aggregate adoption revalidator",
                    )
            return

        if relative == CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE:
            class_span = _class_definition_body_span(
                text, CONSUMED_CANONICAL_ADOPTION_TOKEN
            )
            if (
                class_span is None
                or _compact_cpp_code(text[class_span[0] : class_span[1]])
                != _compact_cpp_code(CONSUMED_CANONICAL_ADOPTION_TOKEN_CLASS_BODY)
                or compact_text.count(
                    CONSUMED_CANONICAL_ADOPTION_BRIDGE_DECLARATION
                )
                != 1
                or compact_text.count(
                    CONSUMED_CANONICAL_ADOPTION_REVALIDATING_BRIDGE_DECLARATION
                )
                != 1
            ):
                self.fail(
                    relative,
                    1 if class_span is None else text.count("\n", 0, class_span[0]) + 1,
                    "consumed-publication BaseLock authority must remain the exact "
                    "private one-shot alias token and bridge declaration",
                )
            return

        def validate_forbidden_bridge_body(
            body: str, line_offset: int, bridge_name: str
        ) -> None:
            for identifier in CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_PRIMITIVES:
                for use in find_code_identifier_uses(body, identifier):
                    self.fail(
                        relative,
                        line_offset + use.line,
                        f"{bridge_name} must not duplicate, open, or relock BaseLock "
                        f"authority: {identifier}",
                    )
            for identifier in CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_ADOPTION_PATHS:
                for use in find_code_identifier_uses(body, identifier):
                    self.fail(
                        relative,
                        line_offset + use.line,
                        f"{bridge_name} must not escape through borrowed or path "
                        f"adoption: {identifier}",
                    )
            for identifier in CONSUMED_CANONICAL_ADOPTION_FORBIDDEN_RELEASES:
                for use in find_code_identifier_uses(body, identifier):
                    self.fail(
                        relative,
                        line_offset + use.line,
                        f"{bridge_name} must not release the retained action claim "
                        f"before receipt ownership: {identifier}",
                    )
            compact_body = _compact_cpp_code(body)
            if (
                re.search(
                    r"(?:newBaseLock\(|(?:make_shared|make_unique)<"
                    r"(?:ooc_cleanup_detail::)?BaseLock>)",
                    compact_body,
                )
                is not None
            ):
                self.fail(
                    relative,
                    line_offset + 1,
                    f"{bridge_name} must not construct a replacement BaseLock",
                )
            if (
                re.search(
                    r"(?:state|owner|live_lock|terminal|authority\.live_lock_)"
                    r"(?:->|\.)?reset\(",
                    compact_body,
                )
                is not None
                or "std::move(owner->lock)" in compact_body
            ):
                self.fail(
                    relative,
                    line_offset + 1,
                    f"{bridge_name} must not release or move out the retained "
                    "action-claim owner before receipt ownership",
                )

        if relative == CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE:
            phase_span = _enum_class_definition_body_span(text, "Phase")
            if (
                phase_span is None
                or _compact_cpp_code(text[phase_span[0] : phase_span[1]])
                != _compact_cpp_code(CONSUMED_CANONICAL_ADOPTION_PHASE_BODY)
            ):
                self.fail(
                    relative,
                    1 if phase_span is None else text.count("\n", 0, phase_span[0]) + 1,
                    "publication permit phase must distinguish only observed, "
                    "validated, consumed-nonterminal, and consumed-canonical states",
                )

            commit_body, commit_line_offset, commit_errors = (
                find_function_definition_body(
                    text, CONSUMED_CANONICAL_ADOPTION_COMMIT
                )
            )
            for line, error in commit_errors:
                self.fail(relative, line, error)
            if (
                commit_body is not None
                and _compact_cpp_code(commit_body)
                != _compact_cpp_code(CONSUMED_CANONICAL_ADOPTION_COMMIT_BODY)
            ):
                self.fail(
                    relative,
                    commit_line_offset + 1,
                    "only an exact successful canonical terminal result may mint "
                    "ConsumedCanonical phase",
                )

            entry_body, entry_line_offset, entry_errors = (
                find_function_definition_body(
                    text, CONSUMED_CANONICAL_ADOPTION_ENTRY
                )
            )
            for line, error in entry_errors:
                self.fail(relative, line, error)
            if entry_body is None:
                return
            compact_entry = _compact_cpp_code(entry_body)
            entry_declarator = (
                "OOCPrivateHandoffAdoptionResult"
                "adopt_consumed_canonical_private_handoff_publication_v1("
                "PrivateHandoffPublicationValidatedPermitV1&&permit,"
                "OOCPrivateHandoffAdoptionTestHookshooks)noexcept{"
            )
            if compact_text.count(entry_declarator) != 1:
                self.fail(
                    relative,
                    entry_line_offset + 1,
                    "consumed-canonical adoption implementation must retain the "
                    "exact rvalue permit entry signature",
                )
            if compact_entry != _compact_cpp_code(
                CONSUMED_CANONICAL_ADOPTION_ENTRY_BODY
            ):
                self.fail(
                    relative,
                    entry_line_offset + 1,
                    "consumed-canonical adoption entry must remain the exact "
                    "relation-only authority bridge",
                )
            permit_move = "std::move(permit.state_)"
            if (
                not compact_entry.startswith("autostate=" + permit_move + ";")
                or compact_entry.find("std::move(")
                != compact_entry.find(permit_move)
                or compact_entry.count(permit_move) != 1
            ):
                self.fail(
                    relative,
                    entry_line_offset + 1,
                    "consumed-canonical adoption must make std::move(permit.state_) "
                    "its first capability move",
                )
            if (
                compact_entry.count(
                    "PrivateHandoffPublicationObservedPermitV1::State::Phase::"
                    "ConsumedCanonical"
                )
                != 1
                or "Phase::ConsumedNonTerminal" in compact_entry
            ):
                self.fail(
                    relative,
                    entry_line_offset + 1,
                    "consumed-canonical adoption entry must accept "
                    "ConsumedCanonical phase only",
                )
            if (
                compact_entry.count(
                    CONSUMED_CANONICAL_ADOPTION_ENTRY_ALIAS_FRAGMENT
                )
                != 1
            ):
                self.fail(
                    relative,
                    entry_line_offset + 1,
                    "consumed-canonical adoption must convert the moved State to "
                    "one shared owner and create exact aliasing BaseLock and "
                    "terminal shared_ptr values",
                )
            validate_forbidden_bridge_body(
                entry_body,
                entry_line_offset,
                "consumed-canonical adoption entry",
            )

            reader_body, reader_line_offset, reader_errors = (
                find_function_definition_body(
                    text, CONSUMED_CANONICAL_READER_ADOPTION_ENTRY
                )
            )
            for line, error in reader_errors:
                self.fail(relative, line, error)
            if reader_body is None:
                return
            compact_reader = _compact_cpp_code(reader_body)
            reader_declarator = (
                "PrivateHandoffPublicationReaderAdoptionResultV1"
                "adopt_consumed_canonical_private_handoff_reader_v1("
                "PrivateHandoffPublicationValidatedPermitV1&permit,"
                "PrivateHandoffPublicationAdoptionRevalidatorV1&&revalidator,"
                "OOCPrivateHandoffAdoptionTestHookshooks)noexcept{"
            )
            if compact_text.count(reader_declarator) != 1:
                self.fail(
                    relative,
                    reader_line_offset + 1,
                    "transactional reader adoption implementation must retain "
                    "the exact lvalue permit and rvalue revalidator signature",
                )
            if not compact_reader.startswith(
                "autoretained_revalidator=std::move(revalidator);"
                "autoowner=permit.state_;try{"
            ) or "std::move(permit.state_)" in compact_reader:
                self.fail(
                    relative,
                    reader_line_offset + 1,
                    "transactional reader adoption must retain the lvalue permit "
                    "until the final commit",
                )
            if compact_reader.count(CONSUMED_CANONICAL_READER_ADOPTION_SUCCESS_ORDER) != 1:
                self.fail(
                    relative,
                    reader_line_offset + 1,
                    "transactional reader adoption must construct and validate "
                    "the same-handle reader before aggregate commit checks",
                )
            if (
                compact_reader.count(
                    CONSUMED_CANONICAL_READER_ADOPTION_COMMIT_TAIL
                )
                != 1
                or compact_reader.count("permit.state_.reset()") != 1
            ):
                self.fail(
                    relative,
                    reader_line_offset + 1,
                    "transactional reader adoption may reset the permit only "
                    "after both terminal checks and trusted final revalidation",
                )
            validate_forbidden_bridge_body(
                reader_body,
                reader_line_offset,
                "transactional reader adoption entry",
            )
            return

        if relative != CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE:
            return

        if (
            compact_text.count(
                CONSUMED_CANONICAL_ADOPTION_TOKEN_CONSTRUCTOR_FRAGMENT
            )
            != 1
            or compact_text.count(CONSUMED_CANONICAL_ADOPTION_TOKEN_MOVE_FRAGMENT)
            != 1
            or compact_text.count(CONSUMED_CANONICAL_ADOPTION_TOKEN_ASSERT_FRAGMENT)
            != 1
        ):
            self.fail(
                relative,
                1,
                "consumed-publication alias token must retain exact move-only "
                "construction and one-shot assertions",
            )

        shape_body, shape_line_offset, shape_errors = find_function_definition_body(
            text, CONSUMED_CANONICAL_ADOPTION_SHAPE_MATCHER
        )
        for line, error in shape_errors:
            self.fail(relative, line, error)
        if (
            shape_body is not None
            and _compact_cpp_code(shape_body)
            != _compact_cpp_code(CONSUMED_CANONICAL_ADOPTION_SHAPE_MATCHER_BODY)
        ):
            self.fail(
                relative,
                shape_line_offset + 1,
                "consumed-canonical terminal shape matcher must remain exact",
            )

        matcher_body, matcher_line_offset, matcher_errors = (
            find_function_definition_body(
                text, CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER
            )
        )
        for line, error in matcher_errors:
            self.fail(relative, line, error)
        if (
            matcher_body is not None
            and _compact_cpp_code(matcher_body)
            != _compact_cpp_code(CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER_BODY)
        ):
            self.fail(
                relative,
                matcher_line_offset + 1,
                "consumed-canonical terminal matcher must compare the exact "
                "record, leaves, markers, parent, lock, and directory",
            )

        adoption_body, adoption_line_offset, adoption_errors = (
            find_function_definition_body(text, "adopt_private_handoff_impl")
        )
        for line, error in adoption_errors:
            self.fail(relative, line, error)
        if adoption_body is not None:
            compact_adoption = _compact_cpp_code(adoption_body)
            if (
                compact_adoption.count(
                    CONSUMED_CANONICAL_ADOPTION_INITIAL_MATCH_FRAGMENT
                )
                != 1
            ):
                self.fail(
                    relative,
                    adoption_line_offset + 1,
                    "consumed-canonical adoption requires its terminal matcher "
                    "immediately after initial canonical classification",
                )
            if (
                compact_adoption.count(
                    CONSUMED_CANONICAL_ADOPTION_REVALIDATION_MATCH_FRAGMENT
                )
                != 1
            ):
                self.fail(
                    relative,
                    adoption_line_offset + 1,
                    "consumed-canonical adoption requires its terminal matcher in "
                    "the receipt revalidation closure",
                )
            if (
                compact_adoption.count(
                    CONSUMED_CANONICAL_ADOPTION_RECEIPT_ORDER_FRAGMENT
                )
                != 1
                or compact_adoption.count("make_receipt(") != 1
            ):
                self.fail(
                    relative,
                    adoption_line_offset + 1,
                    "receipt construction must follow the second terminal-aware "
                    "revalidation with no earlier receipt commit",
                )
            matcher_uses = find_code_identifier_uses(
                adoption_body, CONSUMED_CANONICAL_ADOPTION_TERMINAL_MATCHER
            )
            if len(matcher_uses) != 2:
                self.fail(
                    relative,
                    adoption_line_offset + 1,
                    "common adoption path must contain exactly the initial and "
                    "pre-receipt terminal matcher call sites",
                )

        bridge_declarator = (
            "OOCPrivateHandoffAdoptionResult"
            "ooc_cleanup_detail::"
            "adopt_private_handoff_with_consumed_publication_base_lock_v1("
            "conststd::filesystem::path&base_path,"
            "OOCPrivateHandoffConsumedPublicationBaseLockV1&&authority,"
            "OOCPrivateHandoffAdoptionTestHookshooks)noexcept{"
        )
        revalidating_bridge_declarator = (
            "OOCPrivateHandoffAdoptionResult"
            "ooc_cleanup_detail::"
            "adopt_private_handoff_with_consumed_publication_base_lock_v1("
            "conststd::filesystem::path&base_path,"
            "OOCPrivateHandoffConsumedPublicationBaseLockV1&&authority,"
            "PrivateHandoffPublicationAdoptionRevalidatorV1&revalidator,"
            "OOCPrivateHandoffAdoptionTestHookshooks)noexcept{"
        )
        if (
            compact_text.count(bridge_declarator) != 1
            or compact_text.count(revalidating_bridge_declarator) != 1
        ):
            self.fail(
                relative,
                1,
                "consumed-publication BaseLock bridges must retain their exact "
                "plain and trusted-revalidator source-private signatures",
            )

        bridge_definitions: list[tuple[str, int, bool]] = []
        for use in find_code_identifier_uses(
            text, CONSUMED_CANONICAL_ADOPTION_BRIDGE
        ):
            opening = _function_declarator_terminator(
                text, use, CONSUMED_CANONICAL_ADOPTION_BRIDGE
            )
            if opening is None or opening >= len(text) or text[opening] != "{":
                continue
            closing = _matching_brace(text, opening)
            if closing is None:
                self.fail(relative, use.line, "unterminated consumed-publication bridge")
                continue
            signature = _compact_cpp_code(text[use.offset:opening])
            bridge_definitions.append(
                (
                    text[opening + 1 : closing],
                    text.count("\n", 0, opening + 1),
                    "PrivateHandoffPublicationAdoptionRevalidatorV1&revalidator"
                    in signature,
                )
            )
        if len(bridge_definitions) != 2 or sum(
            1 for _, _, revalidating in bridge_definitions if revalidating
        ) != 1:
            self.fail(
                relative,
                1,
                "consumed-publication adoption must define exactly one plain and "
                "one trusted-revalidating bridge",
            )
        for bridge_body, bridge_line_offset, revalidating in bridge_definitions:
            expected_body = (
                CONSUMED_CANONICAL_ADOPTION_REVALIDATING_BRIDGE_BODY
                if revalidating
                else CONSUMED_CANONICAL_ADOPTION_BRIDGE_BODY
            )
            if _compact_cpp_code(bridge_body) != _compact_cpp_code(expected_body):
                self.fail(
                    relative,
                    bridge_line_offset + 1,
                    "consumed-publication BaseLock bridge must move the exact "
                    "alias once into the terminal-aware common adoption path",
                )
            validate_forbidden_bridge_body(
                bridge_body,
                bridge_line_offset,
                "consumed-publication BaseLock bridge",
            )

    def validate_merge_prepared_admission_boundary(
        self, relative: str, text: str
    ) -> None:
        compact_text = _compact_cpp_code(text)

        if relative not in MERGE_PREPARED_RECOVERED_SEAM_ALLOWLIST:
            for identifier in (
                MERGE_PREPARED_RECOVERED_SUBJECT_ENUM,
                MERGE_PREPARED_RECOVERED_PHASE_ENUM,
                MERGE_PREPARED_RECOVERED_STOP_HOOK,
            ):
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "recovered MergePrepared stop seam is source-private to "
                        f"WaveStore and its one test: {identifier}",
                    )

        if relative == MERGE_PREPARED_ADMISSION_INTERFACE_FILE:
            project_includes = {
                match.group(1)
                for match in re.finditer(
                    r"(?m)^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", text
                )
                if match.group(1).startswith("gnfs/")
                or match.group(1).startswith("distributed_sieve_")
            }
            expected_project_includes = {
                "gnfs/sieve/distributed_sieve_protocol.hpp",
                "gnfs/util/process.hpp",
            }
            if project_includes != expected_project_includes:
                self.fail(
                    relative,
                    1,
                    "common MergePrepared admission may depend only on protocol "
                    "and process project headers",
                )

            class_span = _class_definition_body_span(
                text, MERGE_PREPARED_ADMISSION_IDENTIFIER
            )
            if class_span is None:
                self.fail(relative, 1, "common MergePrepared admission class is missing")
                return
            class_source = text[class_span[0] : class_span[1]]
            class_body = _compact_cpp_code(class_source)
            required_fragments = (
                "private:usingOriginValidatorV1=bool(*)("
                "constvoid*lifetime_anchor,constMergePreparedV1*stable_record,"
                "std::uint64_tcreator_process_id)noexcept;",
                "explicitDistributedSieveMergePreparedAdmissionV1("
                "std::shared_ptr<constvoid>lifetime_anchor,"
                "constMergePreparedV1*stable_record,"
                "std::uint64_tcreator_process_id,"
                "OriginValidatorV1origin_validator)noexcept",
                "std::shared_ptr<constvoid>lifetime_anchor_;"
                "constMergePreparedV1*record_=nullptr;"
                "std::uint64_tcreator_process_id_=0;"
                "OriginValidatorV1origin_validator_=nullptr;",
                "friendclassDistributedSieveMergeWriterAuthorityV1;",
                "friendclass::gnfs::sieve::distributed_sieve_resume_detail::"
                "DistributedSieveWaveStore;",
            )
            if (
                any(class_body.count(fragment) != 1 for fragment in required_fragments)
                or class_body.count("friendclass") != 2
            ):
                self.fail(
                    relative,
                    text.count("\n", 0, class_span[0]) + 1,
                    "common MergePrepared admission must retain one private "
                    "type-erased anchor and exactly the fresh/WaveStore mint friends",
                )
            valid_body, valid_line_offset, valid_errors = (
                find_function_definition_body(class_source, "valid")
            )
            for line, error in valid_errors:
                self.fail(
                    relative,
                    text.count("\n", 0, class_span[0]) + line,
                    error,
                )
            if valid_body is None or _compact_cpp_code(valid_body) != (
                MERGE_PREPARED_ADMISSION_VALID_BODY
            ):
                self.fail(
                    relative,
                    text.count("\n", 0, class_span[0]) + valid_line_offset + 1,
                    "common MergePrepared admission valid() must gate the shared "
                    "anchor, stable record, creator PID, and private origin validator",
                )
            return

        if relative == MERGE_WRITER_AUTHORITY_INTERFACE_FILE:
            if (
                compact_text.count(
                    '#include"distributed_sieve_merge_prepared_admission_internal.hpp"'
                )
                != 1
                or _class_definition_body_span(
                    text, MERGE_PREPARED_ADMISSION_IDENTIFIER
                )
                is not None
                or compact_text.count(
                    "validate_prepared_admission_origin("
                    "constvoid*lifetime_anchor,constMergePreparedV1*stable_record,"
                    "std::uint64_tcreator_process_id)noexcept;"
                )
                != 1
            ):
                self.fail(
                    relative,
                    1,
                    "fresh writer authority must import the common admission and "
                    "declare only its private origin validator",
                )
            return

        if relative == MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE:
            publish_body, publish_line_offset, publish_errors = (
                find_function_definition_body(
                    text, MERGE_PREPARED_ADMISSION_FRESH_PUBLISH_FUNCTION
                )
            )
            for line, error in publish_errors:
                self.fail(relative, line, error)
            if (
                publish_body is None
                or _compact_cpp_code(publish_body).count(
                    MERGE_PREPARED_ADMISSION_FRESH_PUBLISH_FRAGMENT
                )
                != 1
                or len(
                    find_code_identifier_uses(
                        text, MERGE_PREPARED_ADMISSION_IDENTIFIER
                    )
                )
                != 2
            ):
                self.fail(
                    relative,
                    publish_line_offset + 1,
                    "fresh publication must mint exactly one common admission by "
                    "moving the unique writer State into a shared lifetime anchor",
                )
            validator_body, validator_line_offset, validator_errors = (
                find_function_definition_body(
                    text, MERGE_PREPARED_ADMISSION_FRESH_VALIDATOR
                )
            )
            for line, error in validator_errors:
                self.fail(relative, line, error)
            if validator_body is None or _compact_cpp_code(validator_body) != _compact_cpp_code(
                MERGE_PREPARED_ADMISSION_FRESH_VALIDATOR_BODY
            ):
                self.fail(
                    relative,
                    validator_line_offset + 1,
                    "fresh common admission validator must preserve every prior "
                    "writer, record, payload, process, and adopted-input validity check",
                )
            return

        if relative == MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE:
            subject_span = _enum_class_definition_body_span(
                text, MERGE_PREPARED_RECOVERED_SUBJECT_ENUM
            )
            phase_span = _enum_class_definition_body_span(
                text, MERGE_PREPARED_RECOVERED_PHASE_ENUM
            )
            hooks_span = _class_definition_body_span(
                text, "DistributedSieveMergePreparedResumeTestHooksV1"
            )
            hooks_body = (
                "" if hooks_span is None else _compact_cpp_code(text[hooks_span[0] : hooks_span[1]])
            )
            hook_alias = (
                "usingStopBeforeRecoveredAggregateRevalidation=bool(*)("
                "DistributedSieveRecoveredPreparedPublicationSubjectV1subject,"
                "std::size_tmanifest_slot,"
                "DistributedSieveRecoveredPreparedAggregatePhaseV1phase,"
                "void*context)noexcept;"
            )
            hook_member = (
                "StopBeforeRecoveredAggregateRevalidation"
                "stop_before_recovered_aggregate_revalidation=nullptr;"
            )
            if (
                subject_span is None
                or _compact_cpp_code(text[subject_span[0] : subject_span[1]])
                != MERGE_PREPARED_RECOVERED_SUBJECT_ENUM_BODY
                or phase_span is None
                or _compact_cpp_code(text[phase_span[0] : phase_span[1]])
                != MERGE_PREPARED_RECOVERED_PHASE_ENUM_BODY
                or hooks_span is None
                or hooks_body.count(hook_alias) != 1
                or hooks_body.count(hook_member) != 1
                or len(
                    find_code_identifier_uses(
                        text, MERGE_PREPARED_RECOVERED_STOP_HOOK
                    )
                )
                != 1
            ):
                self.fail(
                    relative,
                    1,
                    "recovered MergePrepared test seam must expose only Target/Worker, "
                    "the exact three phases, real manifest slot, and one callback",
                )

            result_span = _class_definition_body_span(
                text, MERGE_PREPARED_ADMISSION_OPEN_RESULT
            )
            if result_span is None:
                self.fail(relative, 1, "WaveStore OpenResult is missing")
                return
            result_source = text[result_span[0] : result_span[1]]
            result_body = _compact_cpp_code(result_source)
            bool_body, bool_line_offset, bool_errors = find_function_definition_body(
                result_source, "operator bool"
            )
            for line, error in bool_errors:
                self.fail(
                    relative,
                    text.count("\n", 0, result_span[0]) + line,
                    error,
                )
            if (
                len(
                    find_code_identifier_uses(
                        text, MERGE_PREPARED_ADMISSION_IDENTIFIER
                    )
                )
                != 2
                or result_body.count("std::unique_ptr<DistributedSieveWaveStore>store;")
                != 1
                or result_body.count(
                    "std::optional<"
                    "distributed_sieve_merge_writer_authority_detail::"
                    "DistributedSieveMergePreparedAdmissionV1>prepared_admission;"
                )
                != 1
                or bool_body is None
                or _compact_cpp_code(bool_body)
                != MERGE_PREPARED_ADMISSION_OPEN_RESULT_BOOL_BODY
            ):
                self.fail(
                    relative,
                    text.count("\n", 0, result_span[0]) + bool_line_offset + 1,
                    "successful WaveStore OpenResult must enforce exact "
                    "store/prepared-admission XOR with admission validity",
                )
            return

        if relative != MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE:
            return

        open_body, open_line_offset, open_errors = find_function_definition_body(
            text, MERGE_PREPARED_ADMISSION_OPEN_FUNCTION
        )
        for line, error in open_errors:
            self.fail(relative, line, error)
        if open_body is None:
            return
        compact_open = _compact_cpp_code(open_body)
        claim_offset = compact_open.find(MERGE_PREPARED_ADMISSION_COLD_CLAIM_FRAGMENT)
        classifier_offset = compact_open.find(
            MERGE_PREPARED_ADMISSION_COLD_CLASSIFIER_FRAGMENT
        )
        recovered_owner_offset = compact_open.find(
            "recovered_prepared_state->store=std::move(store);"
        )
        mint_offset = compact_open.find(
            MERGE_PREPARED_ADMISSION_RECOVERED_MINT_FRAGMENT
        )
        success_offset = compact_open.find(
            MERGE_PREPARED_ADMISSION_RECOVERED_SUCCESS_FRAGMENT
        )
        if not (
            0 <= claim_offset < classifier_offset < recovered_owner_offset < mint_offset
            < success_offset
        ):
            self.fail(
                relative,
                open_line_offset + 1,
                "terminal cold-open must claim the coordinator before its final "
                "classifier, retain the store in the recovered anchor, and only "
                "then mint/return the admission",
            )
        if (
            compact_open.count(MERGE_PREPARED_ADMISSION_COLD_CLAIM_FRAGMENT) != 1
            or compact_open.count(MERGE_PREPARED_ADMISSION_COLD_CLASSIFIER_FRAGMENT)
            != 1
            or compact_open.count(MERGE_PREPARED_ADMISSION_RECOVERED_MINT_FRAGMENT)
            != 1
            or compact_open.count(MERGE_PREPARED_ADMISSION_RECOVERED_SUCCESS_FRAGMENT)
            != 1
            or len(find_code_identifier_uses(text, MERGE_PREPARED_ADMISSION_IDENTIFIER))
            != 1
            or len(find_code_identifier_uses(text, "PreparedAdmission")) != 3
            or (
                claim_offset >= 0
                and "return{std::move(store),std::nullopt"
                in compact_open[claim_offset:]
            )
        ):
            self.fail(
                relative,
                open_line_offset + 1,
                "WaveStore must be the sole recovered admission mint and terminal "
                "success must return admission-only",
            )
        trusted_reader_call = (
            "private_lease::adopt_consumed_canonical_private_handoff_reader_v1("
            "retained.permit,"
            "MergePreparedAdmissionRevalidatorAuthorityV1::bind("
            "revalidate_merge_prepared_admission_for_relation,"
            "std::addressof(context)))"
        )
        if compact_open.count(trusted_reader_call) != 1:
            self.fail(
                relative,
                open_line_offset + 1,
                "WaveStore recovered adoption must be the sole production lvalue "
                "reader call and bind only its trusted aggregate revalidator",
            )

        callback_body, callback_line_offset, callback_errors = (
            find_function_definition_body(
                text, "revalidate_merge_prepared_admission_for_relation"
            )
        )
        for line, error in callback_errors:
            self.fail(relative, line, error)
        compact_callback = "" if callback_body is None else _compact_cpp_code(callback_body)
        if (
            callback_body is None
            or compact_callback.count(
                MERGE_PREPARED_RECOVERED_PHASE_COUNTER_FRAGMENT
            )
            != 1
            or compact_callback.count(MERGE_PREPARED_RECOVERED_STOP_HOOK_FRAGMENT)
            != 1
            or len(
                find_code_identifier_uses(
                    text, MERGE_PREPARED_RECOVERED_STOP_HOOK
                )
            )
            != 2
        ):
            self.fail(
                relative,
                callback_line_offset + 1,
                "recovered aggregate callback must offer exactly three ordered "
                "phases; true injects interrupted, while false immediately runs "
                "the production authority/projection revalidation",
            )
        if (
            compact_open.count(MERGE_PREPARED_RECOVERED_COUNT_COMMIT_FRAGMENT)
            != 1
            or compact_open.count(MERGE_PREPARED_RECOVERED_TARGET_CALL_FRAGMENT)
            != 1
            or compact_open.count(MERGE_PREPARED_RECOVERED_WORKER_CALL_FRAGMENT)
            != 1
            or compact_open.count(MERGE_PREPARED_RECOVERED_WORKER_SLOT_FRAGMENT)
            != 1
        ):
            self.fail(
                relative,
                open_line_offset + 1,
                "each recovered target/worker publication must complete all three "
                "callbacks using Target/NO_INDEX or Worker/real manifest_slot "
                "before permit commit",
            )

    def validate_worker_writer_identifier_exception_boundary(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_WRITER_AUTHORITY_EXCEPTION_FILE:
            return

        identifier = WORKER_WRITER_AUTHORITY_IDENTIFIER
        uses = find_code_identifier_uses(text, identifier)
        forward_declarations = list(
            re.finditer(rf"\bclass\s+{re.escape(identifier)}\s*;", text)
        )
        qualified_friend_declarations = list(
            re.finditer(
                rf"\bfriend\s+class\s+::gnfs::sieve::"
                rf"distributed_sieve_worker_entry_detail::\s*"
                rf"{re.escape(identifier)}\s*;",
                text,
            )
        )
        allowed_offsets = {
            match.start() + match.group(0).rfind(identifier)
            for match in (*forward_declarations, *qualified_friend_declarations)
        }
        if (
            len(uses) != 2
            or len(forward_declarations) != 1
            or len(qualified_friend_declarations) != 1
            or {use.offset for use in uses} != allowed_offsets
        ):
            line = uses[0].line if uses else 1
            self.fail(
                relative,
                line,
                "relation-store worker authority exception must be exactly one "
                "forward declaration and one qualified friend declaration",
            )

    def validate_worker_handoff_publication_boundary(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_WRITER_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_definition_body(
            text, WORKER_HANDOFF_PUBLICATION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        for identifier in (
            WORKER_HANDOFF_TYPED_BUILDER_IDENTIFIER,
            RAW_PRIVATE_HANDOFF_PUBLISHER_IDENTIFIER,
        ):
            all_uses = find_code_identifier_uses(text, identifier)
            all_calls = find_call_identifier_uses(text, identifier)
            body_calls = find_call_identifier_uses(body, identifier)
            for use in find_non_call_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"{identifier} authority must be used only as a direct call",
                )
            if len(all_calls) != 1 or len(body_calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{WORKER_HANDOFF_PUBLICATION_FUNCTION} must contain the only "
                    f"direct {identifier} call, found {len(all_calls)} file calls "
                    f"and {len(body_calls)} function calls",
                )
            if len(all_uses) != len(all_calls):
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{WORKER_HANDOFF_PUBLICATION_FUNCTION} forbids indirect "
                    f"{identifier} authority",
                )

    def validate_merge_prepared_macro_alias_boundary(
        self, relative: str, text: str
    ) -> None:
        closed_identifiers = set(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_MACRO_IDENTIFIERS
        )
        logical = _logical_preprocessor_text(text)
        for pragma in re.finditer(
            r"(?m)^[ \t]*(?:#|%:)[ \t]*pragma[ \t]+"
            r"(?:push_macro|pop_macro)\b",
            logical,
        ):
            self.fail(
                relative,
                logical.count("\n", 0, pragma.start()) + 1,
                "push_macro/pop_macro pragmas cannot reach the "
                "MergePrepared protected interval",
            )
        for pragma_identifier in ("_Pragma", "__pragma"):
            for use in find_code_identifier_uses(text, pragma_identifier):
                self.fail(
                    relative,
                    use.line,
                    "pragma operators cannot reach the MergePrepared "
                    f"protected interval: {pragma_identifier}",
                )
        for directive, identifier, replacement, line in (
            _preprocessor_macro_records(text)
        ):
            if identifier in closed_identifiers:
                self.fail(
                    relative,
                    line,
                    "MergePrepared closed authority identifier cannot be a "
                    f"preprocessor macro target: #{directive} {identifier}",
                )
            elif identifier in self.merge_prepared_protected_tokens:
                self.fail(
                    relative,
                    line,
                    "preprocessor macro target collides with a token in the "
                    f"MergePrepared protected interval: {identifier}",
                )
            else:
                if directive != "define":
                    continue
                mentioned = tuple(
                    closed
                    for closed in closed_identifiers
                    if find_code_identifier_uses(replacement, closed)
                )
                if not mentioned:
                    continue
                self.fail(
                    relative,
                    line,
                    "preprocessor macro replacement cannot mention "
                    "MergePrepared closed authority identifiers: "
                    + ", ".join(sorted(mentioned)),
                )

    def validate_private_handoff_publication_resume_boundary(
        self, relative: str, text: str
    ) -> None:
        if (
            relative
            not in PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_USE_SITE_ALLOWLIST
        ):
            for identifier in (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_USE_SITE_IDENTIFIERS
            ):
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "MergePrepared publication-resume WaveStore identifier "
                        f"is not allowlisted: {identifier}",
                    )

        narrow_direct_calls = (
            PRIVATE_HANDOFF_PUBLICATION_RESUME_NARROW_TEST_DIRECT_CALL_IDENTIFIERS.get(
                relative
            )
        )
        if narrow_direct_calls is not None:
            for identifier in narrow_direct_calls:
                uses = find_code_identifier_uses(text, identifier)
                calls = find_call_identifier_uses(text, identifier)
                for use in find_non_call_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "narrow resume test authority is direct-call-only; "
                        f"aliases and function-pointer references are forbidden: "
                        f"{identifier}",
                    )
                if len(uses) != len(calls):
                    self.fail(
                        relative,
                        1,
                        "narrow resume test authority must remain direct-call-only; "
                        f"{identifier} has "
                        f"{len(uses)} identifiers and {len(calls)} calls",
                    )
            forbidden_identifiers = (
                set(PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_IDENTIFIERS)
                - set(narrow_direct_calls)
            ) | {
                PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER,
                PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER,
                PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_IDENTIFIER,
            }
            for identifier in sorted(forbidden_identifiers):
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "narrow resume test use site forbids authority: "
                        f"{identifier}",
                    )
            return

        if relative not in {
            PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
            PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        }:
            for use in find_code_identifier_uses(
                text, PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER
            ):
                self.fail(
                    relative,
                    use.line,
                    "shared preactive rollback executor escaped the closed "
                    "generic-plus-typed call-site allowlist",
                )

        if (
            relative
            == PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_INTERFACE_FILE
        ):
            enum_span = _enum_class_definition_body_span(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_OBSERVATION_ENUM
            )
            if (
                enum_span is None
                or _compact_cpp_code(text[enum_span[0] : enum_span[1]])
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_OBSERVATION_ENUM_BODY
            ):
                self.fail(
                    relative,
                    1 if enum_span is None else text.count("\n", 0, enum_span[0]) + 1,
                    "WaveStore worker-handoff resume observation enum must remain "
                    "the exact ordered durable-boundary mirror",
                )
            hooks_span = _class_definition_body_span(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_TEST_HOOKS
            )
            if (
                hooks_span is None
                or _compact_cpp_code(text[hooks_span[0] : hooks_span[1]])
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_TEST_HOOKS_BODY
            ):
                self.fail(
                    relative,
                    1
                    if hooks_span is None
                    else text.count("\n", 0, hooks_span[0]) + 1,
                    "WaveStore worker-handoff resume test hooks must remain the "
                    "exact closed test-only seam",
                )
            merge_enum_span = _enum_class_definition_body_span(
                text,
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_OBSERVATION_ENUM,
            )
            if (
                merge_enum_span is None
                or _compact_cpp_code(
                    text[merge_enum_span[0] : merge_enum_span[1]]
                )
                != PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_OBSERVATION_ENUM_BODY
            ):
                self.fail(
                    relative,
                    1
                    if merge_enum_span is None
                    else text.count("\n", 0, merge_enum_span[0]) + 1,
                    "WaveStore MergePrepared resume observation enum must remain "
                    "the exact ordered one-for-one durable-boundary mirror",
                )
            merge_hooks_span = _class_definition_body_span(
                text, PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_TEST_HOOKS
            )
            if (
                merge_hooks_span is None
                or _compact_cpp_code(
                    text[merge_hooks_span[0] : merge_hooks_span[1]]
                )
                != PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_TEST_HOOKS_BODY
            ):
                self.fail(
                    relative,
                    1
                    if merge_hooks_span is None
                    else text.count("\n", 0, merge_hooks_span[0]) + 1,
                    "WaveStore MergePrepared resume test hooks must remain the "
                    "exact closed stop/fail test-only seam",
                )
            return

        if relative == PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FILE:
            body, body_line_offset, body_errors = find_function_definition_body(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FUNCTION
            )
            for line, error in body_errors:
                self.fail(relative, line, error)
            if body is not None:
                compact_body = _compact_cpp_code(body)
                if (
                    compact_body.count("private_handoff_rollback_path") != 3
                    or compact_body.count(
                        PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_ROLLBACK_BLOCKER
                    )
                    != 1
                ):
                    self.fail(
                        relative,
                        body_line_offset + 1,
                        "private-handoff adoption must reject the exact rollback "
                        "tombstone after lock binding and before directory adoption",
                    )
            return

        if relative == PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE:
            protected_helpers = (
                (
                    PRIVATE_LEASE_PREACTIVE_SCANNER_IDENTIFIER,
                    PRIVATE_LEASE_PREACTIVE_SCANNER_DEFINITION_SHAPE,
                    PRIVATE_LEASE_PREACTIVE_SCANNER_BODY,
                    4,
                    "preactive rollback directory scanner must retain the exact "
                    "owner/index/data-only allowlist",
                ),
                (
                    PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER,
                    PRIVATE_LEASE_PREACTIVE_ROLLBACK_DEFINITION_SHAPE,
                    PRIVATE_LEASE_PREACTIVE_ROLLBACK_BODY,
                    3,
                    "shared preactive rollback executor must retain its exact "
                    "capability, identity, quarantine, re-scan, and ordered-delete "
                    "contract",
                ),
            )
            for (
                identifier,
                expected_shape,
                expected_body,
                expected_use_count,
                error,
            ) in protected_helpers:
                body, body_line_offset, body_errors = find_function_definition_body(
                    text, identifier
                )
                for line, body_error in body_errors:
                    self.fail(relative, line, body_error)
                uses = find_code_identifier_uses(text, identifier)
                calls = find_call_identifier_uses(text, identifier)
                definitions = []
                for use in uses:
                    terminator = _function_declarator_terminator(
                        text, use, identifier
                    )
                    if (
                        terminator is not None
                        and terminator < len(text)
                        and text[terminator] == "{"
                    ):
                        definitions.append((use, terminator))
                valid = (
                    body is not None
                    and len(uses) == expected_use_count
                    and len(calls) == expected_use_count
                    and len(definitions) == 1
                    and not _contains_conditional_preprocessor_directive(body)
                    and _compact_cpp_code(body)
                    == _compact_cpp_code(expected_body)
                )
                if len(definitions) == 1:
                    definition_use, terminator = definitions[0]
                    scope = _active_brace_stack(text, definition_use.offset)
                    start = _statement_start_at_scope(
                        text, definition_use.offset, scope
                    )
                    valid = (
                        valid
                        and _compact_cpp_code(text[start:terminator])
                        == _compact_cpp_code(expected_shape)
                    )
                if not valid:
                    self.fail(
                        relative,
                        body_line_offset + 1 if body is not None else 1,
                        error,
                    )

            generic_body, generic_line_offset, generic_errors = (
                find_function_definition_body(
                    text, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
            )
            for line, generic_error in generic_errors:
                self.fail(relative, line, generic_error)
            if generic_body is not None:
                generic_tokens = _compact_cpp_tokens(generic_body)
                generic_uses = find_code_identifier_uses(
                    text, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
                generic_calls = find_call_identifier_uses(
                    text, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
                generic_definitions = []
                for generic_use in generic_uses:
                    terminator = _function_declarator_terminator(
                        text,
                        generic_use,
                        PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER,
                    )
                    if (
                        terminator is not None
                        and terminator < len(text)
                        and text[terminator] == "{"
                    ):
                        generic_definitions.append((generic_use, terminator))
                generic_shape_matches = False
                if len(generic_definitions) == 1:
                    definition_use, terminator = generic_definitions[0]
                    definition_scope = _active_brace_stack(
                        text, definition_use.offset
                    )
                    definition_start = _statement_start_at_scope(
                        text, definition_use.offset, definition_scope
                    )
                    generic_shape_matches = (
                        _compact_cpp_tokens(text[definition_start:terminator])
                        == _compact_cpp_tokens(
                            PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPE
                        )
                    )
                generic_rollback_uses = find_code_identifier_uses(
                    generic_body, PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER
                )
                generic_rollback_calls = find_call_identifier_uses(
                    generic_body, PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER
                )
                generic_scanner_calls = find_call_identifier_uses(
                    generic_body, PRIVATE_LEASE_PREACTIVE_SCANNER_IDENTIFIER
                )
                if (
                    len(generic_uses) != 1
                    or len(generic_calls) != 1
                    or len(generic_definitions) != 1
                    or not generic_shape_matches
                    or generic_tokens
                    != _compact_cpp_tokens(PRIVATE_LEASE_GENERIC_RECOVERY_BODY)
                    or len(generic_rollback_uses) != 2
                    or len(generic_rollback_calls) != 2
                    or len(generic_scanner_calls) != 1
                    or generic_tokens.count(
                        PRIVATE_LEASE_GENERIC_PREACTIVE_SCAN_FRAGMENT
                    )
                    != 1
                    or any(
                        generic_tokens.count(fragment) != 1
                        for fragment in (
                            PRIVATE_LEASE_GENERIC_PREACTIVE_ROLLBACK_FRAGMENTS
                        )
                    )
                    or _contains_conditional_preprocessor_directive(generic_body)
                ):
                    self.fail(
                        relative,
                        generic_line_offset + 1,
                        "generic private-lease recovery core must retain its exact "
                        "validation, rollback, and marker-tail contract",
                    )
            else:
                self.fail(
                    relative,
                    1,
                    "generic private-lease recovery core must retain its exact "
                    "validation, rollback, and marker-tail contract",
                )
            return

        if (
            relative
            not in PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_ALLOWLIST
        ):
            for use in find_code_identifier_uses(
                text,
                PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER,
            ):
                self.fail(
                    relative,
                    use.line,
                    "worker-handoff typed-validator mint authority is not allowlisted",
                )
        if (
            relative
            not in PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_ALLOWLIST
        ):
            for use in find_code_identifier_uses(
                text,
                PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_IDENTIFIER,
            ):
                self.fail(
                    relative,
                    use.line,
                    "test-only private-handoff typed-validator mint authority is not "
                    "allowlisted",
                )

        auxiliary_use_counts = (
            PRIVATE_HANDOFF_PUBLICATION_RESUME_AUXILIARY_USE_SITE_COUNTS.get(
                relative
            )
        )
        if auxiliary_use_counts is not None:
            for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_IDENTIFIERS:
                uses = find_code_identifier_uses(text, identifier)
                expected = auxiliary_use_counts.get(identifier, 0)
                if len(uses) != expected:
                    self.fail(
                        relative,
                        uses[0].line if uses else 1,
                        "private-handoff publication resume auxiliary use site "
                        f"must contain exactly {expected} {identifier} identifiers, "
                        f"found {len(uses)}",
                    )
            return

        if relative not in PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_ALLOWLIST:
            present_identifiers = (
                identifier
                for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_IDENTIFIERS
                if identifier in text
            )
            for identifier in present_identifiers:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "private-handoff publication resume use site is not "
                        f"allowlisted: {identifier}",
                    )
            return

        if relative == PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE:
            enum_name = "PrivateHandoffPublicationResumeObservationPointV1"
            enum_span = _enum_class_definition_body_span(text, enum_name)
            if (
                enum_span is None
                or _compact_cpp_code(text[enum_span[0] : enum_span[1]])
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_OBSERVATION_ENUM_BODY
            ):
                self.fail(
                    relative,
                    1 if enum_span is None else text.count("\n", 0, enum_span[0]) + 1,
                    "relation private-handoff resume observation enum must remain "
                    "the exact ordered durable-boundary authority",
                )
            hooks_name = "PrivateHandoffPublicationResumeTestHooksV1"
            hooks_span = _class_definition_body_span(text, hooks_name)
            if (
                hooks_span is None
                or _compact_cpp_code(text[hooks_span[0] : hooks_span[1]])
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_TEST_HOOKS_BODY
            ):
                self.fail(
                    relative,
                    1
                    if hooks_span is None
                    else text.count("\n", 0, hooks_span[0]) + 1,
                    "relation private-handoff resume test hooks must remain the "
                    "exact closed test-only seam",
                )

            authority_identifier = (
                PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER
            )
            authority_uses = find_code_identifier_uses(text, authority_identifier)
            authority_forward_declarations = list(
                re.finditer(
                    rf"\bclass\s+{re.escape(authority_identifier)}\s*;",
                    text,
                )
            )
            authority_friend_declarations = list(
                re.finditer(
                    rf"\bfriend\s+class\s+gnfs::sieve::"
                    rf"distributed_sieve_resume_detail::\s*"
                    rf"{re.escape(authority_identifier)}\s*;",
                    text,
                )
            )
            authority_allowed_offsets = {
                match.start() + match.group(0).rfind(authority_identifier)
                for match in (
                    *authority_forward_declarations,
                    *authority_friend_declarations,
                )
            }
            if (
                len(authority_uses) != 2
                or len(authority_forward_declarations) != 1
                or len(authority_friend_declarations) != 1
                or {use.offset for use in authority_uses}
                != authority_allowed_offsets
            ):
                self.fail(
                    relative,
                    authority_uses[0].line if authority_uses else 1,
                    "relation resume interface must expose worker typed-validator "
                    "mint authority as exactly one forward declaration and one "
                    "qualified friend declaration",
                )

            class_spans: dict[str, tuple[int, int]] = {}
            for class_name in PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_SCOPES:
                span = _class_definition_body_span(text, class_name)
                if span is None:
                    self.fail(
                        relative,
                        1,
                        "private-handoff resume interface must contain exactly one "
                        f"{class_name} definition",
                    )
                    continue
                class_spans[class_name] = span

            for identifier, expected_count in (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_DECLARATION_COUNTS.items()
            ):
                uses = find_code_identifier_uses(text, identifier)
                calls = find_call_identifier_uses(text, identifier)
                if len(uses) != expected_count or len(calls) != expected_count:
                    self.fail(
                        relative,
                        1,
                        "private-handoff resume interface must contain exactly "
                        f"{expected_count} declaration-shaped {identifier} identifiers, "
                        f"found {len(uses)} identifiers and {len(calls)} call-shaped uses",
                    )

                observed_shapes: list[str] = []
                for use in uses:
                    terminator = _function_declarator_terminator(text, use, identifier)
                    if (
                        terminator is None
                        or terminator >= len(text)
                        or text[terminator] != ";"
                    ):
                        self.fail(
                            relative,
                            use.line,
                            "private-handoff resume interface permits declarations only; "
                            f"inline {identifier} definitions and aliases are forbidden",
                        )
                        continue
                    scope = _active_brace_stack(text, use.offset)
                    start = _statement_start_at_scope(text, use.offset, scope)
                    observed_shapes.append(
                        _compact_cpp_code(text[start : terminator + 1])
                    )
                expected_shapes = (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_DECLARATION_SHAPES[
                        identifier
                    ]
                )
                if sorted(observed_shapes) != sorted(expected_shapes):
                    self.fail(
                        relative,
                        uses[0].line if uses else 1,
                        "private-handoff resume interface declaration shape changed for "
                        f"{identifier}",
                    )

            for class_name, expected_scope_counts in (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_SCOPES.items()
            ):
                span = class_spans.get(class_name)
                if span is None:
                    continue
                body = text[span[0] : span[1]]
                friend_count = len(find_code_identifier_uses(body, "friend"))
                expected_friend_count = (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_FRIEND_COUNTS[
                        class_name
                    ]
                )
                observed_friend_classes = tuple(
                    sorted(
                        re.findall(
                            r"friendclass([A-Za-z_][A-Za-z0-9_:]*);",
                            _compact_cpp_code(body),
                        )
                    )
                )
                expected_friend_classes = tuple(
                    sorted(
                        PRIVATE_HANDOFF_PUBLICATION_RESUME_INTERFACE_CLASS_FRIEND_CLASSES[
                            class_name
                        ]
                    )
                )
                if (
                    friend_count != expected_friend_count
                    or observed_friend_classes != expected_friend_classes
                ):
                    self.fail(
                        relative,
                        text.count("\n", 0, span[0]) + 1,
                        f"{class_name} friend authority must remain exactly closed; "
                        f"found {friend_count} friend declarations and "
                        f"{observed_friend_classes}",
                    )
                for identifier, expected_count in expected_scope_counts.items():
                    observed_count = len(find_code_identifier_uses(body, identifier))
                    if observed_count != expected_count:
                        self.fail(
                            relative,
                            text.count("\n", 0, span[0]) + 1,
                            f"{class_name} must contain exactly {expected_count} "
                            f"{identifier} friend declarations, found {observed_count}",
                        )
            return

        if relative == PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE:
            rollback_uses = find_code_identifier_uses(
                text, PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER
            )
            rollback_calls = find_call_identifier_uses(
                text, PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER
            )
            if len(rollback_uses) != 1 or len(rollback_calls) != 1:
                self.fail(
                    relative,
                    rollback_uses[0].line if rollback_uses else 1,
                    "shared preactive rollback executor escaped the closed "
                    "generic-plus-typed call-site allowlist",
                )
            for identifier, expected_count in (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_IMPLEMENTATION_DEFINITION_COUNTS.items()
            ):
                body, _, body_errors = find_function_definition_body(text, identifier)
                for line, error in body_errors:
                    self.fail(relative, line, error)
                uses = find_code_identifier_uses(text, identifier)
                calls = find_call_identifier_uses(text, identifier)
                if len(uses) != expected_count or len(calls) != expected_count:
                    self.fail(
                        relative,
                        1,
                        "private-handoff resume implementation must contain only the "
                        f"exact {identifier} definition, found {len(uses)} identifiers "
                        f"and {len(calls)} call-shaped uses",
                    )
                if body is None or len(uses) != 1:
                    continue
                use = uses[0]
                terminator = _function_declarator_terminator(text, use, identifier)
                if (
                    terminator is None
                    or terminator >= len(text)
                    or text[terminator] != "{"
                ):
                    self.fail(
                        relative,
                        use.line,
                        f"{identifier} must remain one out-of-line definition",
                    )
                    continue
                scope = _active_brace_stack(text, use.offset)
                start = _statement_start_at_scope(text, use.offset, scope)
                observed_shape = _compact_cpp_code(text[start:terminator])
                expected_shape = (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_IMPLEMENTATION_DEFINITION_SHAPES[
                        identifier
                    ]
                )
                if observed_shape != expected_shape:
                    self.fail(
                        relative,
                        use.line,
                        "private-handoff resume implementation definition shape changed "
                        f"for {identifier}",
                    )

                compact_body = _compact_cpp_code(body)
                if identifier == PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER:
                    if (
                        compact_body.count("std::make_unique<") != 1
                        or compact_body.count("std::make_shared<") != 1
                        or compact_body.count(
                            "claim.transfer_to_permit();"
                        )
                        != 1
                        or compact_body.count("state->lock=std::move(lock);") != 1
                        or compact_body.count(
                            PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_EXCEPTION_SAFETY_FRAGMENT
                        )
                        != 1
                    ):
                        self.fail(
                            relative,
                            use.line,
                            "private-handoff resume acquisition must finish all "
                            "throwing State construction before noexcept lock "
                            "adoption and action-claim transfer",
                        )
                elif identifier == PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER:
                    validator_member_uses = (
                        "validator.validate_",
                        "validator.context_",
                        "validator.creator_process_id_",
                    )
                    if (
                        not compact_body.startswith(
                            PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_CONSUME_PREFIX
                        )
                        or any(
                            compact_body.count(member) != 1
                            for member in validator_member_uses
                        )
                    ):
                        self.fail(
                            relative,
                            use.line,
                            "private-handoff typed validator must be consumed exactly "
                            "once at validation entry before every early return",
                        )

            adapter_span = _class_definition_body_span(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_ADAPTER
            )
            adapter_uses = find_code_identifier_uses(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_ADAPTER
            )
            if (
                adapter_span is None
                or len(adapter_uses) != 3
                or _compact_cpp_code(text[adapter_span[0] : adapter_span[1]])
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_ADAPTER_BODY
            ):
                self.fail(
                    relative,
                    adapter_uses[0].line if adapter_uses else 1,
                    "private-handoff lease-recovery adapter state must remain "
                    "exactly closed",
                )

            lease_bridge_functions = (
                (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_MAP_FUNCTION,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_MAP_BODY,
                    2,
                    2,
                ),
                (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_STAGE_FUNCTION,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_STAGE_BODY,
                    2,
                    2,
                ),
                (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_OBSERVER_FUNCTION,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_OBSERVER_BODY,
                    2,
                    1,
                ),
            )
            for function_name, expected_body, expected_uses, expected_calls in (
                lease_bridge_functions
            ):
                bridge_body, bridge_line_offset, bridge_errors = (
                    find_function_definition_body(text, function_name)
                )
                for line, error in bridge_errors:
                    self.fail(relative, line, error)
                bridge_uses = find_code_identifier_uses(text, function_name)
                bridge_calls = find_call_identifier_uses(text, function_name)
                if (
                    bridge_body is None
                    or len(bridge_uses) != expected_uses
                    or len(bridge_calls) != expected_calls
                    or _compact_cpp_code(bridge_body) != expected_body
                ):
                    self.fail(
                        relative,
                        bridge_line_offset + 1,
                        f"{function_name} must remain the exact seven-point "
                        "lease-recovery adapter with unknown points interrupting "
                        "fail closed",
                    )

            rollback_body, rollback_line_offset, rollback_errors = (
                find_function_definition_body(
                    text, PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER
                )
            )
            for line, error in rollback_errors:
                self.fail(relative, line, error)
            rollback_uses = find_code_identifier_uses(
                text, PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER
            )
            rollback_calls = find_call_identifier_uses(
                text, PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER
            )
            rollback_shape = None
            if rollback_uses:
                rollback_use = rollback_uses[0]
                rollback_terminator = _function_declarator_terminator(
                    text,
                    rollback_use,
                    PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER,
                )
                if (
                    rollback_terminator is not None
                    and rollback_terminator < len(text)
                    and text[rollback_terminator] == "{"
                ):
                    rollback_scope = _active_brace_stack(text, rollback_use.offset)
                    rollback_start = _statement_start_at_scope(
                        text, rollback_use.offset, rollback_scope
                    )
                    rollback_shape = _compact_cpp_code(
                        text[rollback_start:rollback_terminator]
                    )
            if (
                rollback_body is None
                or len(rollback_uses) != 2
                or len(rollback_calls) != 2
                or rollback_shape
                != _compact_cpp_code(
                    PRIVATE_HANDOFF_ROLLBACK_RECOVERY_DEFINITION_SHAPE
                )
                or _compact_cpp_code(rollback_body)
                != _compact_cpp_code(PRIVATE_HANDOFF_ROLLBACK_RECOVERY_BODY)
            ):
                self.fail(
                    relative,
                    rollback_line_offset + 1,
                    "dedicated private-handoff rollback executor must retain its "
                    "exact typed-only signature, body, and single call site",
                )

            generic_uses = find_code_identifier_uses(
                text, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
            )
            generic_calls = find_call_identifier_uses(
                text, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
            )
            compact_implementation = _compact_cpp_code(text)
            if (
                len(generic_uses) != 2
                or len(generic_calls) != 2
                or any(
                    compact_implementation.count(fragment) != 1
                    for fragment in PRIVATE_LEASE_GENERIC_RECOVERY_CALL_FRAGMENTS
                )
            ):
                self.fail(
                    relative,
                    generic_uses[0].line if generic_uses else 1,
                    "ordinary private-lease recovery must remain on the exact two "
                    "generic executor call sites",
                )
            for scope_name, ordered_fragments in (
                PRIVATE_LEASE_GENERIC_RECOVERY_SCOPES.items()
            ):
                scope_body, scope_line_offset, scope_errors = (
                    find_function_definition_body(text, scope_name)
                )
                for line, error in scope_errors:
                    self.fail(relative, line, error)
                if scope_body is None:
                    continue
                scope_compact = _compact_cpp_tokens(scope_body)
                scope_generic_uses = find_code_identifier_uses(
                    scope_body, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
                scope_generic_calls = find_call_identifier_uses(
                    scope_body, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
                positions = [
                    scope_compact.find(fragment) for fragment in ordered_fragments
                ]
                scope_uses = find_code_identifier_uses(text, scope_name)
                scope_definitions = []
                for scope_use in scope_uses:
                    terminator = _function_declarator_terminator(
                        text, scope_use, scope_name
                    )
                    if (
                        terminator is not None
                        and terminator < len(text)
                        and text[terminator] == "{"
                    ):
                        scope_definitions.append((scope_use, terminator))
                definition_matches = False
                if len(scope_definitions) == 1:
                    definition_use, terminator = scope_definitions[0]
                    definition_scope = _active_brace_stack(
                        text, definition_use.offset
                    )
                    definition_start = _statement_start_at_scope(
                        text, definition_use.offset, definition_scope
                    )
                    definition_matches = (
                        _compact_cpp_tokens(text[definition_start:terminator])
                        == _compact_cpp_tokens(
                            PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPES[
                                scope_name
                            ]
                        )
                    )
                if (
                    len(scope_generic_uses) != 1
                    or len(scope_generic_calls) != 1
                    or len(scope_definitions) != 1
                    or not definition_matches
                    or any(
                        scope_compact.count(fragment) != 1
                        for fragment in ordered_fragments
                    )
                    or positions != sorted(positions)
                    or any(position < 0 for position in positions)
                    or _contains_conditional_preprocessor_directive(scope_body)
                    or scope_compact
                    != _compact_cpp_tokens(
                        PRIVATE_LEASE_GENERIC_RECOVERY_BODIES[scope_name]
                    )
                ):
                    self.fail(
                        relative,
                        scope_line_offset + 1,
                        f"{scope_name} must retain cleanup-union admission, handoff "
                        "preflight, and one unconditional exact generic executor "
                        "return in that order",
                    )

            reconcile_body, reconcile_line_offset, reconcile_errors = (
                find_function_definition_body(
                    text,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER,
                )
            )
            for line, error in reconcile_errors:
                self.fail(relative, line, error)
            if reconcile_body is not None:
                reconcile_compact = _compact_cpp_code(reconcile_body)
                reconcile_rollback_calls = find_call_identifier_uses(
                    reconcile_body, PRIVATE_HANDOFF_ROLLBACK_RECOVERY_IDENTIFIER
                )
                reconcile_generic_uses = find_code_identifier_uses(
                    reconcile_body, PRIVATE_LEASE_GENERIC_RECOVERY_IDENTIFIER
                )
                if (
                    len(reconcile_rollback_calls) != 1
                    or reconcile_generic_uses
                    or reconcile_compact.count(
                        PRIVATE_HANDOFF_ROLLBACK_RECOVERY_TYPED_CALL_FRAGMENT
                    )
                    != 1
                ):
                    self.fail(
                        relative,
                        reconcile_line_offset + 1,
                        "typed private-handoff rollback must call only the dedicated "
                        "generation-bound executor with the retained permit inputs",
                    )
                if (
                    reconcile_compact.count(
                        PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_BINDING_FRAGMENT
                    )
                    != 1
                    or
                    reconcile_compact.count(
                        PRIVATE_HANDOFF_PUBLICATION_RESUME_LEASE_UNKNOWN_FAILURE_FRAGMENT
                    )
                    != 1
                ):
                    self.fail(
                        relative,
                        reconcile_line_offset + 1,
                        "private-handoff resume must bind the exact fresh-capture "
                        "lease context and return exact_failure before unknown "
                        "nested observation/interruption results",
                    )
                if (
                    reconcile_compact.count(
                        PRIVATE_HANDOFF_PUBLICATION_RESUME_FINAL_ABSENCE_FRAGMENT
                    )
                    != 1
                ):
                    self.fail(
                        relative,
                        reconcile_line_offset + 1,
                        "private-handoff rollback completion must prove exact "
                        "NoTransaction absence before returning success",
                    )
            return

        if relative == PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE:
            return

        if relative != PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE:
            return

        wave_store_text = text
        protected_tokens = _merge_prepared_protected_code_tokens(
            wave_store_text
        )
        if not protected_tokens:
            self.fail(
                relative,
                1,
                "MergePrepared protected token inventory is empty",
            )
        else:
            self.merge_prepared_protected_tokens = protected_tokens
        guard_matches = _merge_prepared_macro_guard_matches(text)
        if len(guard_matches) != 1:
            self.fail(
                relative,
                1,
                "production WaveStore must contain exactly one exact "
                "MergePrepared preprocessor macro guard",
            )
        else:
            guard = guard_matches[0]
            expected_callback_prefix = (
                "\n\n"
                + PRIVATE_HANDOFF_PUBLICATION_MERGE_CALLBACK_DECLARATION
            )
            if not text[guard.end() :].startswith(expected_callback_prefix):
                self.fail(
                    relative,
                    text.count("\n", 0, guard.start()) + 1,
                    "MergePrepared preprocessor macro guard must be immediately "
                    "adjacent to the closed typed callback",
                )
            logical_tail = _logical_preprocessor_text(text[guard.end() :])
            if re.search(
                r"(?m)^[ \t]*(?:#|%:)[ \t]*include\b", logical_tail
            ):
                self.fail(
                    relative,
                    text.count("\n", 0, guard.end()) + 1,
                    "production WaveStore must not include headers after the "
                    "MergePrepared preprocessor macro guard",
                )
            definition_spans = {
                identifier: _function_definition_spans(
                    wave_store_text, identifier
                )
                for identifier in (
                    PRIVATE_HANDOFF_PUBLICATION_MERGE_PROTECTED_DEFINITION_IDENTIFIERS
                )
            }
            if any(len(spans) != 1 for spans in definition_spans.values()):
                self.fail(
                    relative,
                    1,
                    "MergePrepared protected definitions must each have exactly "
                    "one source definition",
                )
            else:
                ordered_spans = [
                    definition_spans[identifier][0]
                    for identifier in (
                        PRIVATE_HANDOFF_PUBLICATION_MERGE_PROTECTED_DEFINITION_IDENTIFIERS
                    )
                ]
                if [span[0] for span in ordered_spans] != sorted(
                    span[0] for span in ordered_spans
                ):
                    self.fail(
                        relative,
                        1,
                        "MergePrepared protected definitions must retain their "
                        "closed source order",
                    )
                protected_start = ordered_spans[0][0]
                protected_end = ordered_spans[-1][1]
                protected_offsets = (
                    guard.start(),
                    *(span[0] for span in ordered_spans),
                )
                if any(
                    _preprocessor_conditional_stack_at(
                        wave_store_text, offset
                    )
                    != ("if!defined(_WIN32)",)
                    for offset in protected_offsets
                ):
                    self.fail(
                        relative,
                        1,
                        "MergePrepared guard and protected definitions must be "
                        "in the exact active POSIX preprocessing scope",
                    )
                if not (
                    protected_start < guard.start() < guard.end() < protected_end
                ):
                    self.fail(
                        relative,
                        1,
                        "MergePrepared macro guard must remain inside the closed "
                        "protected definition interval",
                    )
                else:
                    protected = wave_store_text[
                        protected_start:protected_end
                    ]
                    guard_start = guard.start() - protected_start
                    guard_end = guard.end() - protected_start
                    protected_without_guard = (
                        protected[:guard_start]
                        + "".join(
                            character
                            if character in {"\n", "\r"}
                            else " "
                            for character in protected[guard_start:guard_end]
                        )
                        + protected[guard_end:]
                    )
                    if _preprocessor_directives(protected_without_guard):
                        self.fail(
                            relative,
                            text.count("\n", 0, protected_start) + 1,
                            "MergePrepared protected definition interval must "
                            "contain no preprocessing directives outside the "
                            "exact macro guard",
                        )
            masked_guard = "".join(
                character if character in {"\n", "\r"} else " "
                for character in text[guard.start() : guard.end()]
            )
            text = (
                text[: guard.start()]
                + masked_guard
                + text[guard.end() :]
            )

        for directive, _, line in _preprocessor_macro_directives(text):
            self.fail(
                relative,
                line,
                "production WaveStore must not contain preprocessor macro "
                "definitions or undefinitions that can alias closed authority "
                f"(#{directive})",
            )

        compact_text = _compact_cpp_code(text)
        closed_bindings = _compact_cpp_code(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDINGS_SOURCE
        )
        if compact_text.count(closed_bindings) != 1:
            self.fail(
                relative,
                1,
                "MergePrepared typed callback callees must retain the exact "
                "closed function-pointer bindings",
            )
        for identifier in (
            PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDING_IDENTIFIERS
        ):
            uses = find_code_identifier_uses(text, identifier)
            calls = find_call_identifier_uses(text, identifier)
            non_calls = find_non_call_identifier_uses(text, identifier)
            if len(uses) != 2 or len(calls) != 1 or len(non_calls) != 1:
                self.fail(
                    relative,
                    uses[0].line if uses else 1,
                    "MergePrepared closed function-pointer binding must have "
                    f"one definition and one direct call: {identifier}",
                )
        for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS:
            mirror_assertion = (
                "static_assert(static_cast<std::size_t>("
                "DistributedSieveWorkerHandoffResumeObservationPointV1::"
                f"{point})==static_cast<std::size_t>("
                "private_lease::"
                "PrivateHandoffPublicationResumeObservationPointV1::"
                f"{point}));"
            )
            if compact_text.count(mirror_assertion) != 1:
                self.fail(
                    relative,
                    1,
                    "WaveStore worker-handoff resume observation mirror must "
                    f"statically bind exact ordinal {point}",
                )
        if (
            compact_text.count(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_WAVE_MIRROR_ASSERTION
            )
            != 1
        ):
            self.fail(
                relative,
                1,
                "WaveStore MergePrepared resume observation mirror must retain "
                "the exact one-for-one ordered relation mapping",
            )

        for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS:
            uses = find_code_identifier_uses(text, identifier)
            calls = find_call_identifier_uses(text, identifier)
            expected = PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_DIRECT_CALL_COUNTS[
                identifier
            ]
            for use in find_non_call_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"{identifier} must be used only as a direct call; aliases "
                    "and function-pointer references are forbidden",
                )
            if len(uses) != expected or len(calls) != expected:
                self.fail(
                    relative,
                    1,
                    f"production WaveStore must contain exactly {expected} direct "
                    f"{identifier} call, found {len(uses)} identifiers and "
                    f"{len(calls)} calls",
                )

        authority_identifier = (
            PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_IDENTIFIER
        )
        authority_uses = find_code_identifier_uses(text, authority_identifier)
        authority_span = _class_definition_body_span(text, authority_identifier)
        if len(authority_uses) != 4 or authority_span is None:
            self.fail(
                relative,
                authority_uses[0].line if authority_uses else 1,
                "production WaveStore must contain exactly one typed-validator "
                "authority definition and three bound uses",
            )
        elif (
            _compact_cpp_code(text[authority_span[0] : authority_span[1]])
            != PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_BODY
        ):
            self.fail(
                relative,
                text.count("\n", 0, authority_span[0]) + 1,
                "worker typed-validator authority definition shape changed",
            )

        retained_stack_source = _compact_cpp_code(
            PRIVATE_HANDOFF_PUBLICATION_RETAINED_WORKER_STACK_SOURCE
        )
        capture_result_source = _compact_cpp_code(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_CAPTURE_RESULT_SOURCE
        )
        aggregate_span = _class_definition_body_span(
            text, "MergePreparedPublicationAggregateWitness"
        )
        aggregate_body = (
            ""
            if aggregate_span is None
            else _compact_cpp_code(text[aggregate_span[0] : aggregate_span[1]])
        )
        if (
            compact_text.count(retained_stack_source) != 1
            or compact_text.count(capture_result_source) != 1
            or aggregate_span is None
            or not aggregate_body.endswith(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_RETAINED_SUFFIX
            )
            or aggregate_body.count(
                "RetainedWorkerHandoffPublicationPrefixStackretained_workers;"
            )
            != 1
        ):
            self.fail(
                relative,
                1,
                "MergePrepared retained-worker resource layout must keep the "
                "exact reverse-order stack and witness-before-target member "
                "ordering so target authority releases before worker LIFO release",
            )

        aggregate_revalidation_body, aggregate_revalidation_line_offset, (
            aggregate_revalidation_errors
        ) = find_function_definition_body(
            text,
            PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_REVALIDATION_FUNCTION,
        )
        for line, error in aggregate_revalidation_errors:
            self.fail(relative, line, error)
        if aggregate_revalidation_body is not None:
            compact_aggregate_revalidation = _compact_cpp_code(
                aggregate_revalidation_body
            )
            copy_exception_boundary = _compact_cpp_code(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_COPY_EXCEPTION_SOURCE
            )
            retained_revalidation = _compact_cpp_code(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_RETAINED_REVALIDATION_SOURCE
            )
            exact_revalidation_calls = find_call_identifier_uses(
                aggregate_revalidation_body,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER,
            )
            forbidden_calls = tuple(
                identifier
                for identifier in (
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER,
                )
                if find_code_identifier_uses(
                    aggregate_revalidation_body, identifier
                )
            )
            if (
                compact_aggregate_revalidation.count(copy_exception_boundary)
                != 1
                or compact_aggregate_revalidation.count(retained_revalidation)
                != 1
                or len(exact_revalidation_calls) != 1
                or forbidden_calls
            ):
                self.fail(
                    relative,
                    aggregate_revalidation_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_REVALIDATION_FUNCTION} "
                    "must keep throwing inventory copy/projection inside the "
                    "exact noexcept exception boundary; it must exact-revalidate "
                    "only already-retained worker permits, bind their typed/"
                    "provisional/attempt evidence, and reject every unretained "
                    "canonical handoff before path validation",
                )

        merge_body, merge_body_line_offset, merge_body_errors = (
            find_function_definition_body(
                text, PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION
            )
        )
        for line, error in merge_body_errors:
            self.fail(relative, line, error)
        if merge_body is not None:
            merge_call_counts = {
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER: 2,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER: 2,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER: 2,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER: 0,
            }
            merge_calls: dict[str, CodeIdentifierUse] = {}
            for identifier, expected in merge_call_counts.items():
                uses = find_code_identifier_uses(merge_body, identifier)
                calls = find_call_identifier_uses(merge_body, identifier)
                if len(uses) != expected or len(calls) != expected:
                    self.fail(
                        relative,
                        merge_body_line_offset + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        f"must contain exactly {expected} direct {identifier} call, "
                        f"found {len(uses)} identifiers and {len(calls)} calls",
                    )
                    continue
                if identifier in {
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
                }:
                    merge_calls[identifier] = calls[-1]
                elif identifier == PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER:
                    merge_calls[identifier] = calls[0]
            merge_context_identifier = "MergePreparedTypedValidationContext"
            merge_context_uses = find_code_identifier_uses(
                merge_body, merge_context_identifier
            )
            if len(merge_context_uses) != 1:
                self.fail(
                    relative,
                    merge_body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                    "must construct exactly one MergePrepared typed-validation "
                    "context",
                )
            ordered_identifiers = (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER,
            )
            if (
                len(merge_calls) == len(ordered_identifiers)
                and len(merge_context_uses) == 1
            ):
                acquire_use = merge_calls[
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER
                ]
                validation_use = merge_calls[
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER
                ]
                revalidation_use = merge_calls[
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER
                ]
                context_use = merge_context_uses[0]
                scoped_uses = (
                    acquire_use,
                    context_use,
                    validation_use,
                    revalidation_use,
                )
                scopes = tuple(
                    _active_brace_stack(merge_body, use.offset)
                    for use in scoped_uses
                )
                if len(set(scopes)) != 1:
                    self.fail(
                        relative,
                        merge_body_line_offset + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        "must keep acquisition, typed context, validation, and "
                        "permit revalidation inside one lexical control scope",
                    )
                else:
                    for opening in scopes[0]:
                        forbidden = _forbidden_control_scope_introducer(
                            merge_body, opening, forbid_for=True
                        )
                        if forbidden is None:
                            continue
                        self.fail(
                            relative,
                            merge_body_line_offset
                            + merge_body.count("\n", 0, opening)
                            + 1,
                            f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                            f"forbids {forbidden} control around the retained "
                            "permit chain",
                        )

                ordered_offsets = tuple(use.offset for use in scoped_uses)
                if ordered_offsets != tuple(sorted(ordered_offsets)):
                    self.fail(
                        relative,
                        merge_body_line_offset + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        "must acquire, construct the typed context, type-validate, "
                        "then revalidate the retained publication permit in order",
                    )

                acquire_start = _statement_start_at_scope(
                    merge_body, acquire_use.offset, scopes[0]
                )
                acquire_end = _direct_call_statement_end(
                    merge_body,
                    acquire_use,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
                )
                acquire_statement = (
                    ""
                    if acquire_end is None
                    else _compact_cpp_code(
                        merge_body[acquire_start:acquire_end]
                    )
                )
                if (
                    acquire_statement
                    != PRIVATE_HANDOFF_PUBLICATION_MERGE_ACQUIRE_STATEMENT
                ):
                    self.fail(
                        relative,
                        merge_body_line_offset + acquire_use.line,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        "must bind the exact directory-scoped acquisition directly "
                        "to admission",
                    )

                context_start = _statement_start_at_scope(
                    merge_body, context_use.offset, scopes[1]
                )
                validation_end = _direct_call_statement_end(
                    merge_body,
                    validation_use,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
                )
                typed_validation_fragment = (
                    ""
                    if validation_end is None
                    else _compact_cpp_code(
                        merge_body[context_start:validation_end]
                    )
                )
                if (
                    typed_validation_fragment
                    != PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_VALIDATION_FRAGMENT
                ):
                    self.fail(
                        relative,
                        merge_body_line_offset + context_use.line,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        "must consume admission.observed through the exact "
                        "MergePrepared context and closed validator bind",
                    )

                revalidation_start = _statement_start_at_scope(
                    merge_body, revalidation_use.offset, scopes[3]
                )
                revalidation_end = _direct_call_statement_end(
                    merge_body,
                    revalidation_use,
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_REVALIDATE_IDENTIFIER,
                )
                revalidation_statement = (
                    ""
                    if revalidation_end is None
                    else _compact_cpp_code(
                        merge_body[revalidation_start:revalidation_end]
                    )
                )
                if (
                    revalidation_statement
                    != PRIVATE_HANDOFF_PUBLICATION_MERGE_REVALIDATE_STATEMENT
                ):
                    self.fail(
                        relative,
                        merge_body_line_offset + revalidation_use.line,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                        "must revalidate exactly validation.permit",
                    )

            compact_merge_body = _compact_cpp_code(merge_body)
            retained_capture = _compact_cpp_code(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_CAPTURE_SOURCE
            )
            retained_post_target = _compact_cpp_code(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_POST_TARGET_SOURCE
            )
            retained_capture_offset = compact_merge_body.find(retained_capture)
            target_acquire_offset = compact_merge_body.rfind(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_ACQUIRE_STATEMENT
            )
            retained_post_target_offset = compact_merge_body.find(
                retained_post_target
            )
            aggregate_move_offset = compact_merge_body.find(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_AGGREGATE_MOVE_FRAGMENT
            )
            retained_offsets = (
                retained_capture_offset,
                target_acquire_offset,
                retained_post_target_offset,
                aggregate_move_offset,
            )
            if (
                compact_merge_body.count(retained_capture) != 1
                or compact_merge_body.count(retained_post_target) != 1
                or compact_merge_body.count(
                    PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_AGGREGATE_MOVE_FRAGMENT
                )
                != 1
                or any(offset < 0 for offset in retained_offsets)
                or retained_offsets != tuple(sorted(retained_offsets))
            ):
                self.fail(
                    relative,
                    merge_body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_CLASSIFIER_FUNCTION} "
                    "must sort worker predecessors by stable manifest/attempt "
                    "order, acquire/validate/retain every worker permit before "
                    "the target permit, exact-revalidate retained workers while "
                    "the target is held, and move the stack into the aggregate",
                )

        merge_typed_body, merge_typed_line_offset, merge_typed_errors = (
            find_function_definition_body(
                text, PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION
            )
        )
        for line, error in merge_typed_errors:
            self.fail(relative, line, error)
        if merge_typed_body is not None:
            compact_merge_typed = _compact_cpp_code(merge_typed_body)
            if (
                compact_merge_typed
                != PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_BODY
            ):
                self.fail(
                    relative,
                    merge_typed_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION} "
                    "must remain the exact fail-closed callback body with no "
                    "interposed mutation or control flow",
                )
            for identifier in (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_CALLS
            ):
                uses = find_code_identifier_uses(merge_typed_body, identifier)
                calls = find_call_identifier_uses(merge_typed_body, identifier)
                if len(uses) != 1 or len(calls) != 1:
                    self.fail(
                        relative,
                        merge_typed_line_offset + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION} "
                        f"must contain exactly one direct {identifier} call",
                    )
            for fragment in (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_REQUIRED_FRAGMENTS
            ):
                if compact_merge_typed.count(fragment) != 1:
                    self.fail(
                        relative,
                        merge_typed_line_offset + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION} "
                        "must retain every exact typed guard, dependency binding, "
                        "and witness-transfer fragment",
                    )
            if compact_merge_typed.count("returntrue;") != 1:
                self.fail(
                    relative,
                    merge_typed_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_FUNCTION} "
                    "must contain one sole successful return after witness transfer",
                )

        bridge_function_shapes = (
            (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_BODY,
                2,
                1,
            ),
            (
                PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FACTORY_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_BRIDGE_FACTORY_BODY,
                2,
                2,
            ),
        )
        for function_name, expected_body, expected_uses, expected_calls in (
            bridge_function_shapes
        ):
            bridge_body, bridge_line_offset, bridge_errors = (
                find_function_definition_body(text, function_name)
            )
            for line, error in bridge_errors:
                self.fail(relative, line, error)
            bridge_uses = find_code_identifier_uses(text, function_name)
            bridge_calls = find_call_identifier_uses(text, function_name)
            if (
                bridge_body is None
                or len(bridge_uses) != expected_uses
                or len(bridge_calls) != expected_calls
                or _compact_cpp_code(bridge_body) != expected_body
            ):
                self.fail(
                    relative,
                    bridge_line_offset + 1,
                    f"{function_name} must remain the exact fail-closed "
                    "observation, revalidation, and relation-hook bridge",
                )

        merge_bridge_function_shapes = (
            (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_BODY,
                3,
                3,
            ),
            (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_STOP_BRIDGE_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_STOP_BRIDGE_BODY,
                2,
                1,
            ),
            (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_FAIL_BRIDGE_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_FAIL_BRIDGE_BODY,
                2,
                1,
            ),
            (
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FACTORY_FUNCTION,
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_BRIDGE_FACTORY_BODY,
                2,
                2,
            ),
        )
        for function_name, expected_body, expected_uses, expected_calls in (
            merge_bridge_function_shapes
        ):
            bridge_body, bridge_line_offset, bridge_errors = (
                find_function_definition_body(text, function_name)
            )
            for line, error in bridge_errors:
                self.fail(relative, line, error)
            bridge_uses = find_code_identifier_uses(text, function_name)
            bridge_calls = find_call_identifier_uses(text, function_name)
            if (
                bridge_body is None
                or len(bridge_uses) != expected_uses
                or len(bridge_calls) != expected_calls
                or _compact_cpp_code(bridge_body) != expected_body
            ):
                self.fail(
                    relative,
                    bridge_line_offset + 1,
                    f"{function_name} must remain the exact stop/fail hook-first "
                    "bridge with held Wave authority, aggregate projection, "
                    "latest MergeStarted revalidation, and drift priority",
                )

        typed_body, typed_body_line_offset, typed_body_errors = (
            find_function_definition_body(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_FUNCTION
            )
        )
        for line, error in typed_body_errors:
            self.fail(relative, line, error)
        if typed_body is not None:
            typed_identifier = PRIVATE_HANDOFF_PUBLICATION_TYPED_VALIDATOR_IDENTIFIER
            typed_uses = find_code_identifier_uses(typed_body, typed_identifier)
            typed_calls = find_call_identifier_uses(typed_body, typed_identifier)
            if len(typed_uses) != 1 or len(typed_calls) != 1:
                self.fail(
                    relative,
                    typed_body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_FUNCTION} "
                    f"must contain exactly 1 direct {typed_identifier} call, found "
                    f"{len(typed_uses)} identifiers and {len(typed_calls)} calls",
                )
            else:
                typed_use = typed_calls[0]
                if (
                    _compact_cpp_code(typed_body)
                    != PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_BODY
                ):
                    self.fail(
                        relative,
                        typed_body_line_offset + typed_use.line,
                        f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_TYPED_CALLBACK_FUNCTION} "
                        "must keep the exact typed failure guard, AttemptStarted "
                        "binding, witness transfer, and sole true return in one "
                        "unconditional scope",
                    )

        body, body_line_offset, body_errors = find_function_definition_body(
            text, PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        capture_call_identifiers = (
            PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
            PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
        )
        capture_calls: dict[str, CodeIdentifierUse] = {}
        for identifier in capture_call_identifiers:
            uses = find_code_identifier_uses(body, identifier)
            calls = find_call_identifier_uses(body, identifier)
            if len(uses) != 1 or len(calls) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must "
                    f"contain exactly 1 direct {identifier} call, found "
                    f"{len(uses)} identifiers and {len(calls)} calls",
                )
                continue
            capture_calls[identifier] = calls[0]

        context_identifier = "WorkerHandoffTypedValidationContext"
        context_uses = find_code_identifier_uses(body, context_identifier)
        if len(context_uses) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must "
                "construct exactly one worker typed-validation context",
            )

        if (
            len(capture_calls) == len(capture_call_identifiers)
            and len(context_uses) == 1
        ):
            acquire_use = capture_calls[
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER
            ]
            validation_use = capture_calls[
                PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER
            ]
            context_use = context_uses[0]
            scoped_uses = (acquire_use, context_use, validation_use)
            scopes = tuple(
                _active_brace_stack(body, use.offset) for use in scoped_uses
            )
            if len(set(scopes)) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must keep "
                    "resume acquisition, typed-validator context binding, and relation "
                    "validation inside one lexical control scope",
                )
            else:
                control_scope = scopes[0]
                for opening in control_scope:
                    forbidden = _forbidden_control_scope_introducer(body, opening)
                    if forbidden is None:
                        continue
                    self.fail(
                        relative,
                        body_line_offset + body.count("\n", 0, opening) + 1,
                        f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} forbids "
                        f"{forbidden} control around the resume authority chain",
                    )

            if not (
                acquire_use.offset < context_use.offset < validation_use.offset
            ):
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must "
                    "order resume acquisition, typed-validator context binding, and "
                    "relation validation",
                )

            acquire_start = _statement_start_at_scope(
                body, acquire_use.offset, scopes[0]
            )
            acquire_end = _direct_call_statement_end(
                body,
                acquire_use,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
            )
            acquire_statement = (
                ""
                if acquire_end is None
                else _compact_cpp_code(body[acquire_start:acquire_end])
            )
            if not acquire_statement.startswith(
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_ASSIGNMENT_PREFIX
            ):
                self.fail(
                    relative,
                    body_line_offset + acquire_use.line,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must bind "
                    "the resume acquisition directly to auto admission",
                )

            context_start = _statement_start_at_scope(
                body, context_use.offset, scopes[1]
            )
            validation_end = _direct_call_statement_end(
                body,
                validation_use,
                PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
            )
            typed_validation_fragment = (
                ""
                if validation_end is None
                else _compact_cpp_code(body[context_start:validation_end])
            )
            if (
                typed_validation_fragment
                != PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_TYPED_VALIDATION_FRAGMENT
            ):
                self.fail(
                    relative,
                    body_line_offset + context_use.line,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must pass "
                    "the exact empty typed context through the sole worker validator "
                    "authority bind into relation validation",
                )

            typed_success_fragment = (
                ""
                if validation_end is None
                else _compact_cpp_code(body[validation_end:])
            )
            if not typed_success_fragment.startswith(
                PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_TYPED_SUCCESS_FRAGMENT
            ):
                self.fail(
                    relative,
                    body_line_offset + validation_use.line,
                    f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_CAPTURE_FUNCTION} must "
                    "explicitly reject missing validated permit or typed witness before "
                    "consuming the callback-produced handoff",
                )

        open_body, open_body_line_offset, open_body_errors = (
            find_function_definition_body(
                text, PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION
            )
        )
        for line, error in open_body_errors:
            self.fail(relative, line, error)
        if open_body is None:
            return

        reconciler = PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER
        open_uses = find_code_identifier_uses(open_body, reconciler)
        open_calls = find_call_identifier_uses(open_body, reconciler)
        if len(open_uses) != 3 or len(open_calls) != 3:
            self.fail(
                relative,
                open_body_line_offset + 1,
                f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION} must contain "
                f"all three direct {reconciler} calls, found {len(open_uses)} "
                f"identifiers and {len(open_calls)} calls",
            )

        open_compact = _compact_cpp_code(open_body)
        if (
            open_compact.count(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_OPEN_FRAGMENT
            )
            != 1
        ):
            self.fail(
                relative,
                open_body_line_offset + 1,
                f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION} must perform "
                "the exact bounded single-round MergePrepared reconcile before "
                "the worker loop, including expected disposition and terminal "
                "checks, consumed held permit, and fresh authority/inventory rescan",
            )
        if (
            open_compact.count(
                PRIVATE_HANDOFF_PUBLICATION_MERGE_OPEN_RELEASE_ORDER_FRAGMENT
            )
            != 1
        ):
            self.fail(
                relative,
                open_body_line_offset + 1,
                f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION} must reset "
                "the consumed target permit before resetting the aggregate "
                "worker stack, complete its LIFO release, and only then invoke "
                "the round-release hook",
            )
        release_identifier = (
            PRIVATE_HANDOFF_PUBLICATION_RESUME_ROUND_RELEASE_IDENTIFIER
        )
        all_release_uses = find_code_identifier_uses(text, release_identifier)
        open_release_uses = find_code_identifier_uses(open_body, release_identifier)
        if (
            len(all_release_uses) != 4
            or len(open_release_uses) != 4
            or open_compact.count(
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ROUND_RELEASE_FRAGMENT
            )
            != 1
        ):
            self.fail(
                relative,
                open_body_line_offset + 1,
                f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION} must invoke the "
                "test-only round-release hook exactly once after the retained "
                "LIFO stack is empty, immediately recheck process identity, and "
                "only then advance the resume round",
            )

    def validate_merge_generation_authority_use_site(
        self, relative: str, text: str
    ) -> None:
        if relative in MERGE_GENERATION_AUTHORITY_USE_SITE_ALLOWLIST:
            return
        for identifier in MERGE_GENERATION_AUTHORITY_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "merge-generation reserve/publish authority use site is not "
                    f"allowlisted: {identifier}",
                )

    def validate_merge_coordinator_use_site(
        self, relative: str, text: str
    ) -> None:
        interface_leaf = MERGE_COORDINATOR_INTERFACE_FILE.rsplit("/", 1)[-1]
        coordinator_header_includes = [
            line_number
            for line_number, line in enumerate(text.splitlines(), start=1)
            if re.match(r"^[ \t]*#[ \t]*include\b", line)
            and interface_leaf in line
        ]
        if relative not in MERGE_COORDINATOR_USE_SITE_ALLOWLIST:
            for line_number in coordinator_header_includes:
                self.fail(
                    relative,
                    line_number,
                    "source-private merge-coordinator header include is not allowlisted",
                )
            for identifier in MERGE_COORDINATOR_USE_SITE_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "source-private merge-coordinator use site is not "
                        f"allowlisted: {identifier}",
                    )

        if not relative.startswith(PUBLIC_SIEVE_HEADER_PREFIX):
            return
        for line_number in coordinator_header_includes:
            self.fail(
                relative,
                line_number,
                "source-private merge-coordinator header leaked into a public sieve header",
            )
        for identifier in MERGE_COORDINATOR_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "source-private merge-coordinator API leaked into a public sieve header: "
                    f"{identifier}",
                )

    def validate_merge_coordinator_boundary(
        self, relative: str, text: str
    ) -> None:
        if relative not in MERGE_COORDINATOR_PRODUCTION_FILES:
            return

        for token, use in find_code_identifier_tokens(text):
            lowered = token.lower()
            if token in MERGE_COORDINATOR_FORBIDDEN_EXACT_IDENTIFIERS:
                reason = "filesystem-path or raw removal identifier"
            elif (
                set(lowered.split("_"))
                & MERGE_COORDINATOR_FORBIDDEN_IDENTIFIER_SEGMENTS
            ):
                reason = "cleanup, unlink, or removal identifier"
            elif (
                lowered in {"arm", "armed", "arming", "armable"}
                or lowered.startswith("arm_")
                or lowered.endswith("_arm")
                or "_arm_" in lowered
                or "_armable" in lowered
            ):
                reason = "cleanup-arming identifier"
            elif token == MERGE_COORDINATOR_RECORD_IDENTIFIER:
                reason = "caller-side MergeStarted construction/type"
            else:
                continue
            self.fail(
                relative,
                use.line,
                f"merge coordinator forbids {reason} {token}",
            )

        if relative == MERGE_COORDINATOR_INTERFACE_FILE:
            admission_span = _class_definition_body_span(
                text, MERGE_COORDINATOR_ADMISSION_IDENTIFIER
            )
            if admission_span is None:
                self.fail(
                    relative,
                    1,
                    "merge coordinator must define exactly one move-only "
                    f"{MERGE_COORDINATOR_ADMISSION_IDENTIFIER}",
                )
            else:
                admission_source = text[admission_span[0] : admission_span[1]]
                admission_body = _compact_cpp_tokens(admission_source)
                admission = MERGE_COORDINATOR_ADMISSION_IDENTIFIER
                required_move_only_fragments = (
                    f"{admission}(const{admission}&)=delete;",
                    f"{admission}&operator=(const{admission}&)=delete;",
                    f"{admission}({admission}&&)noexcept=default;",
                    f"{admission}&operator=({admission}&&)=delete;",
                )
                for fragment in required_move_only_fragments:
                    if admission_body.count(fragment) != 1:
                        self.fail(
                            relative,
                            text.count("\n", 0, admission_span[0]) + 1,
                            "merge admission must be a one-owner move-only lifetime "
                            f"root; missing exact fragment {fragment}",
                        )

                worker_result = MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER
                worker_parameter = MERGE_COORDINATOR_WORKER_RESULT_PARAMETER
                constructor_fragment = (
                    f"{admission}({worker_result}&&{worker_parameter}"
                )
                member_fragment = f"{worker_result}{worker_parameter}_;"
                friend_fragment = (
                    f"friend{admission}{MERGE_COORDINATOR_COMPOSITION_FUNCTION}("
                    f"{worker_result}&&{worker_parameter}"
                )
                private_offset = admission_body.find("private:")
                constructor_offset = admission_body.find(constructor_fragment)
                member_offset = admission_body.find(member_fragment)
                friend_offset = admission_body.find(friend_fragment)
                admission_result_uses = find_code_identifier_uses(
                    admission_source, worker_result
                )
                if (
                    private_offset < 0
                    or constructor_offset <= private_offset
                    or member_offset <= private_offset
                    or friend_offset <= private_offset
                    or admission_body.count(constructor_fragment) != 1
                    or admission_body.count(member_fragment) != 1
                    or admission_body.count(friend_fragment) != 1
                    or len(admission_result_uses) != 3
                ):
                    self.fail(
                        relative,
                        text.count("\n", 0, admission_span[0]) + 1,
                        "merge admission must privately consume, retain, and friend "
                        "the complete worker coordinator result exactly once each",
                    )

            all_entry_uses = find_code_identifier_uses(
                text, MERGE_COORDINATOR_COMPOSITION_FUNCTION
            )
            if admission_span is None:
                admission_entry_uses: list[CodeIdentifierUse] = []
                entry_uses = all_entry_uses
            else:
                admission_entry_uses = [
                    use
                    for use in all_entry_uses
                    if admission_span[0] <= use.offset < admission_span[1]
                ]
                entry_uses = [
                    use
                    for use in all_entry_uses
                    if not (admission_span[0] <= use.offset < admission_span[1])
                ]
            if len(entry_uses) != 1 or len(admission_entry_uses) != 1:
                self.fail(
                    relative,
                    1,
                    "merge coordinator interface must contain one private admission "
                    "friend and one namespace-scope declaration for "
                    f"{MERGE_COORDINATOR_COMPOSITION_FUNCTION}",
                )
                return
            entry_use = entry_uses[0]
            parentheses = _call_parentheses(
                text, entry_use, MERGE_COORDINATOR_COMPOSITION_FUNCTION
            )
            terminator = _function_declarator_terminator(
                text, entry_use, MERGE_COORDINATOR_COMPOSITION_FUNCTION
            )
            if (
                parentheses is None
                or terminator is None
                or terminator >= len(text)
                or text[terminator] != ";"
            ):
                self.fail(
                    relative,
                    entry_use.line,
                    "merge coordinator interface entry must be declaration-only",
                )
                return
            opening, closing = parentheses
            parameters = _compact_cpp_tokens(text[opening + 1 : closing])
            required_parameter = (
                f"{MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER}&&"
                f"{MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}"
            )
            worker_result_uses = find_code_identifier_uses(
                text, MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER
            )
            admission_result_use_count = (
                0
                if admission_span is None
                else len(
                    find_code_identifier_uses(
                        text[admission_span[0] : admission_span[1]],
                        MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER,
                    )
                )
            )
            parameter_result_uses = find_code_identifier_uses(
                text[opening + 1 : closing],
                MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER,
            )
            if not (
                parameters == required_parameter
                or parameters.startswith(required_parameter + ",")
            ):
                self.fail(
                    relative,
                    entry_use.line,
                    f"{MERGE_COORDINATOR_COMPOSITION_FUNCTION} must consume the "
                    "complete DistributedSieveWorkerCoordinatorResultV1&& as its "
                    "first parameter",
                )
            if (
                len(parameter_result_uses) != 1
                or len(worker_result_uses) != admission_result_use_count + 1
            ):
                self.fail(
                    relative,
                    entry_use.line,
                    "only the private admission constructor/member/friend and the "
                    "merge entry declaration may name the complete worker result",
                )
            return

        admission_constructor_definition_count = 0
        body, body_line_offset, body_errors = find_function_definition_body(
            text, MERGE_COORDINATOR_COMPOSITION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        entry_uses = find_code_identifier_uses(
            text, MERGE_COORDINATOR_COMPOSITION_FUNCTION
        )
        if len(entry_uses) != 1:
            self.fail(
                relative,
                1,
                "merge coordinator implementation must contain only the one "
                f"{MERGE_COORDINATOR_COMPOSITION_FUNCTION} definition",
            )
        else:
            parentheses = _call_parentheses(
                text, entry_uses[0], MERGE_COORDINATOR_COMPOSITION_FUNCTION
            )
            if parentheses is None:
                self.fail(
                    relative,
                    entry_uses[0].line,
                    "merge coordinator entry definition has no parameter list",
                )
            else:
                opening, closing = parentheses
                parameters = _compact_cpp_tokens(text[opening + 1 : closing])
                required_parameter = (
                    f"{MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER}&&"
                    f"{MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}"
                )
                worker_result_uses = find_code_identifier_uses(
                    text, MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER
                )
                parameter_result_uses = find_code_identifier_uses(
                    text[opening + 1 : closing],
                    MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER,
                )
                compact_text = _compact_cpp_tokens(text)
                admission_constructor_fragment = (
                    f"{MERGE_COORDINATOR_ADMISSION_IDENTIFIER}::"
                    f"{MERGE_COORDINATOR_ADMISSION_IDENTIFIER}("
                    f"{MERGE_COORDINATOR_WORKER_RESULT_IDENTIFIER}&&"
                    f"{MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}"
                )
                admission_constructor_definition_count = compact_text.count(
                    admission_constructor_fragment
                )
                if not (
                    parameters == required_parameter
                    or parameters.startswith(required_parameter + ",")
                ):
                    self.fail(
                        relative,
                        entry_uses[0].line,
                        f"{MERGE_COORDINATOR_COMPOSITION_FUNCTION} definition must "
                        "consume the complete worker coordinator result by rvalue reference",
                    )
                if (
                    admission_constructor_definition_count > 1
                    or len(parameter_result_uses) != 1
                    or len(worker_result_uses)
                    != 1 + admission_constructor_definition_count
                ):
                    self.fail(
                        relative,
                        entry_uses[0].line,
                        "only the merge entry and the optional private admission "
                        "constructor definition may name the complete worker result",
                    )
                admission_member_initializer = (
                    f"{MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}_("
                    f"std::move({MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}))"
                )
                if admission_constructor_definition_count != compact_text.count(
                    admission_member_initializer
                ):
                    self.fail(
                        relative,
                        entry_uses[0].line,
                        "an out-of-line merge admission constructor must move the "
                        "complete worker result directly into worker_result_",
                    )

        call_offsets: dict[str, int] = {}
        for identifier in MERGE_GENERATION_AUTHORITY_USE_SITE_IDENTIFIERS:
            all_uses = find_code_identifier_uses(text, identifier)
            all_calls = find_call_identifier_uses(text, identifier)
            body_uses = find_code_identifier_uses(body, identifier)
            body_calls = find_call_identifier_uses(body, identifier)
            for use in find_non_call_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    f"{identifier} must be used only as a direct call; aliases "
                    "and function-pointer references are forbidden",
                )
            if (
                len(all_uses) != 1
                or len(all_calls) != 1
                or len(body_uses) != 1
                or len(body_calls) != 1
            ):
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{MERGE_COORDINATOR_COMPOSITION_FUNCTION} must contain the "
                    f"only direct {identifier} call, found {len(all_uses)} "
                    f"file identifiers and {len(body_calls)} body calls",
                )
            else:
                call_offsets[identifier] = body_calls[0].offset

        if len(call_offsets) == len(MERGE_GENERATION_AUTHORITY_USE_SITE_IDENTIFIERS):
            if (
                call_offsets[MERGE_COORDINATOR_RESERVE_IDENTIFIER]
                >= call_offsets[MERGE_COORDINATOR_PUBLISH_IDENTIFIER]
            ):
                self.fail(
                    relative,
                    body_line_offset + 1,
                    "merge coordinator must reserve the exact merged generation "
                    "before publishing MergeStarted",
                )

        compact_body = _compact_cpp_tokens(body)
        reserve_fragment = (
            f"{MERGE_COORDINATOR_RESERVE_IDENTIFIER}"
            f"(*{MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}.store,"
        )
        publish_fragment = (
            f"{MERGE_COORDINATOR_PUBLISH_IDENTIFIER}("
            "std::move(*reservation.receipt),"
        )
        complete_move = (
            f"std::move({MERGE_COORDINATOR_WORKER_RESULT_PARAMETER})"
        )
        split_move = (
            f"std::move({MERGE_COORDINATOR_WORKER_RESULT_PARAMETER}."
        )
        if compact_body.count(reserve_fragment) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "merge coordinator must reserve through the retained worker_result.store "
                "without extracting the WaveStore",
            )
        if compact_body.count(publish_fragment) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "merge coordinator must consume the exact reservation when publishing "
                "MergeStarted",
            )
        if compact_body.count(complete_move) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "merge coordinator must transfer the complete worker_result exactly "
                "once into the admission lifetime root",
            )
        compact_text = _compact_cpp_tokens(text)
        if (
            compact_text.count(complete_move) - compact_body.count(complete_move)
            != admission_constructor_definition_count
        ):
            self.fail(
                relative,
                1,
                "complete worker_result consumption outside the merge entry is "
                "allowed only in the private admission constructor",
            )
        if split_move in compact_body:
            self.fail(
                relative,
                body_line_offset + 1,
                "merge coordinator must not split-move store, claims, chunks, or "
                "diagnostics out of worker_result",
            )

        reserve_offset = compact_body.find(reserve_fragment)
        publish_offset = compact_body.find(publish_fragment)
        complete_move_offset = compact_body.find(complete_move)
        if (
            reserve_offset >= 0
            and publish_offset >= 0
            and complete_move_offset >= 0
            and not reserve_offset < publish_offset < complete_move_offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "merge coordinator must order reservation, MergeStarted publication, "
                "then whole-result admission transfer",
            )

    def validate_worker_coordinator_use_site(
        self, relative: str, text: str
    ) -> None:
        coordinator_header_includes = [
            line_number
            for line_number, line in enumerate(text.splitlines(), start=1)
            if re.match(r"^[ \t]*#[ \t]*include\b", line)
            and WORKER_COORDINATOR_INTERFACE_FILE.rsplit("/", 1)[-1] in line
        ]
        if relative not in WORKER_COORDINATOR_USE_SITE_ALLOWLIST:
            for line_number in coordinator_header_includes:
                self.fail(
                    relative,
                    line_number,
                    "source-private worker-coordinator header include is not allowlisted",
                )
            for identifier in WORKER_COORDINATOR_USE_SITE_IDENTIFIERS:
                for use in find_code_identifier_uses(text, identifier):
                    self.fail(
                        relative,
                        use.line,
                        "source-private worker-coordinator use site is not "
                        f"allowlisted: {identifier}",
                    )

        if not relative.startswith(PUBLIC_SIEVE_HEADER_PREFIX):
            return
        for line_number in coordinator_header_includes:
            self.fail(
                relative,
                line_number,
                "source-private worker-coordinator header leaked into a public sieve header",
            )
        for identifier in WORKER_COORDINATOR_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "source-private worker-coordinator API leaked into a public sieve header: "
                    f"{identifier}",
                )

    def validate_worker_coordinator_boundary(
        self, relative: str, text: str
    ) -> None:
        if relative not in WORKER_COORDINATOR_PRODUCTION_FILES:
            return

        for line_number, line in enumerate(text.splitlines(), start=1):
            if (
                re.match(r"^[ \t]*#[ \t]*include\b", line)
                and WORKER_COORDINATOR_LEGACY_PUBLIC_HEADER in line
            ):
                self.fail(
                    relative,
                    line_number,
                    "worker coordinator must not include the legacy public "
                    "distributed-sieve header",
                )

        for token, use in find_code_identifier_tokens(text):
            if token in WORKER_COORDINATOR_FORBIDDEN_IDENTIFIERS:
                reason = "raw process or legacy distributed-sieve identifier"
            elif token in WORKER_COORDINATOR_AUTHORITY_FREE_CLEANUP_FACTS:
                continue
            elif any(
                fragment in token.lower()
                for fragment in WORKER_COORDINATOR_FORBIDDEN_IDENTIFIER_FRAGMENTS
            ):
                reason = "cleanup or unlink identifier"
            else:
                continue
            self.fail(
                relative,
                use.line,
                f"worker coordinator forbids {reason} {token}",
            )

        if relative != WORKER_COORDINATOR_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_definition_body(
            text, WORKER_COORDINATOR_COMPOSITION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        bound_identifier = WORKER_COORDINATOR_BOUND_WORK_IDENTIFIER
        all_bound_uses = find_code_identifier_uses(text, bound_identifier)
        all_bound_calls = find_call_identifier_uses(text, bound_identifier)
        body_bound_uses = find_code_identifier_uses(body, bound_identifier)
        body_bound_calls = find_call_identifier_uses(body, bound_identifier)
        for use in find_non_call_identifier_uses(text, bound_identifier):
            self.fail(
                relative,
                use.line,
                "coordinator bound-work validation must be used only as a direct call",
            )
        if len(all_bound_uses) != 1 or len(all_bound_calls) != 1:
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct "
                f"{bound_identifier} call, found {len(all_bound_uses)} identifiers "
                f"and {len(all_bound_calls)} calls",
            )
        if len(body_bound_uses) != 1 or len(body_bound_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {bound_identifier} call must remain inside "
                f"{WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )

        open_identifier = WORKER_COORDINATOR_ATTEMPT_OPEN_IDENTIFIER
        all_open_uses = find_code_identifier_uses(text, open_identifier)
        all_open_calls = find_call_identifier_uses(text, open_identifier)
        body_open_uses = find_code_identifier_uses(body, open_identifier)
        body_open_calls = find_call_identifier_uses(body, open_identifier)
        for use in find_non_call_identifier_uses(text, open_identifier):
            self.fail(
                relative,
                use.line,
                "worker-attempt retry open authority must be used only as a direct call",
            )
        if len(all_open_uses) != 1 or len(all_open_calls) != 1:
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct "
                f"{open_identifier} call, found {len(all_open_uses)} identifiers "
                f"and {len(all_open_calls)} calls",
            )
        if len(body_open_uses) != 1 or len(body_open_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {open_identifier} call must remain inside "
                f"{WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )

        reconcile_identifier = WORKER_COORDINATOR_ATTEMPT_RECONCILE_IDENTIFIER
        all_reconcile_uses = find_code_identifier_uses(text, reconcile_identifier)
        all_reconcile_calls = find_call_identifier_uses(text, reconcile_identifier)
        body_reconcile_uses = find_code_identifier_uses(body, reconcile_identifier)
        body_reconcile_calls = find_call_identifier_uses(body, reconcile_identifier)
        for use in find_non_call_identifier_uses(text, reconcile_identifier):
            self.fail(
                relative,
                use.line,
                "worker-attempt reconciler must be used only as a direct call",
            )
        if len(all_reconcile_uses) != 1 or len(all_reconcile_calls) != 1:
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct "
                f"{reconcile_identifier} call, found {len(all_reconcile_uses)} identifiers "
                f"and {len(all_reconcile_calls)} calls",
            )
        if len(body_reconcile_uses) != 1 or len(body_reconcile_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {reconcile_identifier} call must remain inside "
                f"{WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )

        terminal_failure_publish_identifier = (
            WORKER_COORDINATOR_TERMINAL_FAILURE_PUBLISH_IDENTIFIER
        )
        all_terminal_failure_publish_uses = find_code_identifier_uses(
            text, terminal_failure_publish_identifier
        )
        all_terminal_failure_publish_calls = find_call_identifier_uses(
            text, terminal_failure_publish_identifier
        )
        body_terminal_failure_publish_uses = find_code_identifier_uses(
            body, terminal_failure_publish_identifier
        )
        body_terminal_failure_publish_calls = find_call_identifier_uses(
            body, terminal_failure_publish_identifier
        )
        for use in find_non_call_identifier_uses(
            text, terminal_failure_publish_identifier
        ):
            self.fail(
                relative,
                use.line,
                "terminal-failure publisher must be used only as a direct call",
            )
        if (
            len(all_terminal_failure_publish_uses) != 1
            or len(all_terminal_failure_publish_calls) != 1
        ):
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct "
                f"{terminal_failure_publish_identifier} call, found "
                f"{len(all_terminal_failure_publish_uses)} identifiers and "
                f"{len(all_terminal_failure_publish_calls)} calls",
            )
        if (
            len(body_terminal_failure_publish_uses) != 1
            or len(body_terminal_failure_publish_calls) != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {terminal_failure_publish_identifier} call must "
                f"remain inside {WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )

        expected_adoption_identifier = (
            WORKER_COORDINATOR_EXPECTED_ADOPTION_IDENTIFIER
        )
        all_expected_adoption_uses = find_code_identifier_uses(
            text, expected_adoption_identifier
        )
        all_expected_adoption_calls = find_call_identifier_uses(
            text, expected_adoption_identifier
        )
        body_expected_adoption_uses = find_code_identifier_uses(
            body, expected_adoption_identifier
        )
        body_expected_adoption_calls = find_call_identifier_uses(
            body, expected_adoption_identifier
        )
        for use in find_non_call_identifier_uses(text, expected_adoption_identifier):
            self.fail(
                relative,
                use.line,
                "terminal-witness adoption must be used only as a direct call",
            )
        if (
            len(all_expected_adoption_uses) != 1
            or len(all_expected_adoption_calls) != 1
        ):
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct "
                f"{expected_adoption_identifier} call, found "
                f"{len(all_expected_adoption_uses)} identifiers and "
                f"{len(all_expected_adoption_calls)} calls",
            )
        if (
            len(body_expected_adoption_uses) != 1
            or len(body_expected_adoption_calls) != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {expected_adoption_identifier} call must remain inside "
                f"{WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )

        ordinary_adoption_identifier = (
            WORKER_COORDINATOR_ORDINARY_ADOPTION_IDENTIFIER
        )
        all_ordinary_adoption_uses = find_code_identifier_uses(
            text, ordinary_adoption_identifier
        )
        all_ordinary_adoption_calls = find_call_identifier_uses(
            text, ordinary_adoption_identifier
        )
        body_ordinary_adoption_uses = find_code_identifier_uses(
            body, ordinary_adoption_identifier
        )
        body_ordinary_adoption_calls = find_call_identifier_uses(
            body, ordinary_adoption_identifier
        )
        for use in find_non_call_identifier_uses(text, ordinary_adoption_identifier):
            self.fail(
                relative,
                use.line,
                "ordinary handoff adoption must be used only as the direct "
                "fallback branch of terminal-witness dispatch",
            )
        if (
            len(all_ordinary_adoption_uses) != 1
            or len(all_ordinary_adoption_calls) != 1
            or len(body_ordinary_adoption_uses) != 1
            or len(body_ordinary_adoption_calls) != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must contain exactly one direct ordinary "
                "handoff adoption fallback inside the coordinator",
            )

        compact_body = _compact_cpp_tokens(body)
        if re.search(
            r"(?:if|while)(?:constexpr)?\((?:false|0)(?:\)|&&)",
            compact_body,
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator forbids constant-dead control flow around "
                "retry, terminal publication, and adoption authority",
            )
        if compact_body.count(WORKER_COORDINATOR_ATTEMPT_OPEN_FRAGMENT) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must open the exact initial worker attempt exactly "
                "once through result.store",
            )
        if compact_body.count(WORKER_COORDINATOR_ATTEMPT_RECONCILE_FRAGMENT) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must consume the exact opened attempt once through "
                "the typed reconciler",
            )
        if (
            compact_body.count(
                WORKER_COORDINATOR_TERMINAL_FAILURE_PUBLISH_FRAGMENT
            )
            != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must consume the exact reconciler terminal-failure "
                "admission once through the typed publisher",
            )
        if compact_body.count(WORKER_COORDINATOR_EXPECTED_ADOPTION_FRAGMENT) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must consume the exact reconciler terminal witness "
                "once through expected same-handle adoption",
            )
        if (
            compact_body.count(WORKER_COORDINATOR_TERMINAL_BINDING_FRAGMENT) != 1
            or compact_body.count(
                WORKER_COORDINATOR_TERMINAL_WITNESS_STORE_FRAGMENT
            )
            != 1
            or compact_body.count("expected_adopted_witnesses[") != 3
            or compact_body.count(
                "expected_adopted_witnesses[manifest_slot]"
            )
            != 1
            or compact_body.count("expected_adopted_witnesses[index]") != 2
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must bind the exact reconciler terminal witness "
                "and preserve the exact one-write/two-read manifest-slot flow",
            )
        if (
            compact_body.count(
                WORKER_COORDINATOR_TERMINAL_ADOPTION_DISPATCH_FRAGMENT
            )
            != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must dispatch witnessed terminal handoffs through "
                "expected adoption and all other handoffs through ordinary adoption",
            )
        if (
            len(body_bound_calls) == 1
            and len(body_open_calls) == 1
            and body_bound_calls[0].offset >= body_open_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must validate bound work before opening a retry attempt",
            )
        if (
            len(body_open_calls) == 1
            and len(body_reconcile_calls) == 1
            and body_open_calls[0].offset >= body_reconcile_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must open the exact attempt before reconciling it",
            )
        if (
            len(body_reconcile_calls) == 1
            and len(body_terminal_failure_publish_calls) == 1
            and body_reconcile_calls[0].offset
            >= body_terminal_failure_publish_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must reconcile retry state before publishing "
                "terminal failure",
            )
        if (
            len(body_reconcile_calls) == 1
            and len(body_expected_adoption_calls) == 1
            and body_reconcile_calls[0].offset
            >= body_expected_adoption_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must capture a terminal witness before expected "
                "same-handle adoption",
            )
        terminal_binding_offset = compact_body.find(
            WORKER_COORDINATOR_TERMINAL_BINDING_FRAGMENT
        )
        terminal_store_offset = compact_body.find(
            WORKER_COORDINATOR_TERMINAL_WITNESS_STORE_FRAGMENT
        )
        expected_adoption_offset = compact_body.find(
            WORKER_COORDINATOR_EXPECTED_ADOPTION_FRAGMENT
        )
        reconcile_fragment_offset = compact_body.find(
            WORKER_COORDINATOR_ATTEMPT_RECONCILE_FRAGMENT
        )
        if not (
            reconcile_fragment_offset >= 0
            and terminal_binding_offset > reconcile_fragment_offset
            and terminal_store_offset > terminal_binding_offset
            and expected_adoption_offset > terminal_store_offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must preserve reconcile -> terminal binding -> "
                "manifest-slot witness -> expected adoption provenance",
            )

        identifier = WORKER_COORDINATOR_SEALED_LAUNCHER_IDENTIFIER
        all_uses = find_code_identifier_uses(text, identifier)
        all_calls = find_call_identifier_uses(text, identifier)
        body_uses = find_code_identifier_uses(body, identifier)
        body_calls = find_call_identifier_uses(body, identifier)
        for use in find_non_call_identifier_uses(text, identifier):
            self.fail(
                relative,
                use.line,
                "sealed WaveStore launcher authority must be used only as a direct call",
            )
        if len(all_uses) != 1 or len(all_calls) != 1:
            self.fail(
                relative,
                1,
                "worker coordinator must contain exactly one direct sealed "
                f"WaveStore {identifier} call, found {len(all_uses)} identifiers "
                f"and {len(all_calls)} calls",
            )
        if len(body_uses) != 1 or len(body_calls) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                f"the only direct {identifier} call must remain inside "
                f"{WORKER_COORDINATOR_COMPOSITION_FUNCTION}",
            )
        if compact_body.count(WORKER_COORDINATOR_SEALED_LAUNCHER_FRAGMENT) != 1:
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must call the sealed WaveStore launcher exactly "
                "once through result.store",
            )
        if (
            len(body_bound_calls) == 1
            and len(body_calls) == 1
            and body_bound_calls[0].offset >= body_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must validate bound work before invoking the "
                "sealed WaveStore launcher",
            )
        if (
            len(body_reconcile_calls) == 1
            and len(body_calls) == 1
            and body_reconcile_calls[0].offset >= body_calls[0].offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must reconcile retry state before invoking the "
                "sealed WaveStore launcher",
            )
        if (
            len(body_terminal_failure_publish_calls) == 1
            and any(
                body_terminal_failure_publish_calls[0].offset >= call.offset
                for call in body_calls
            )
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker coordinator must publish terminal failure before invoking "
                "any sealed WaveStore launcher",
            )

    def validate_worker_attempt_terminal_transition_boundary(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_definition_body(
            text, WORKER_ATTEMPT_TERMINAL_TRANSITION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        helper_uses = find_code_identifier_uses(
            text, WORKER_ATTEMPT_TERMINAL_TRANSITION_HELPER
        )
        helper_calls = find_call_identifier_uses(
            text, WORKER_ATTEMPT_TERMINAL_TRANSITION_HELPER
        )
        body_helper_uses = find_code_identifier_uses(
            body, WORKER_ATTEMPT_TERMINAL_TRANSITION_HELPER
        )
        body_helper_calls = find_call_identifier_uses(
            body, WORKER_ATTEMPT_TERMINAL_TRANSITION_HELPER
        )
        if (
            len(helper_uses) != 2
            or len(helper_calls) != 2
            or len(body_helper_uses) != 1
            or len(body_helper_calls) != 1
        ):
            self.fail(
                relative,
                1,
                "exact worker-attempt terminal-transition helper must have one "
                "definition and one direct call inside the claim boundary",
            )

        refresh_uses = find_code_identifier_uses(
            body, WORKER_ATTEMPT_TERMINAL_REFRESH_IDENTIFIER
        )
        refresh_calls = find_call_identifier_uses(
            body, WORKER_ATTEMPT_TERMINAL_REFRESH_IDENTIFIER
        )
        if len(refresh_uses) != 3 or len(refresh_calls) != 2:
            self.fail(
                relative,
                body_line_offset + 1,
                "terminal-transition refresh must remain one local adjudicator with "
                "exactly two direct call sites",
            )

        compact_body = _compact_cpp_tokens(body)
        if re.search(
            r"(?:if|while)(?:constexpr)?\((?:false|0)(?:\)|&&)",
            compact_body,
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker-attempt claim forbids constant-dead control flow around "
                "held-target adjudication and capability transfer",
            )
        required_once = (
            (
                "boolterminal_transition_refreshed=false",
                "terminal-transition single-use state",
            ),
            (
                WORKER_ATTEMPT_TERMINAL_HELPER_CALL_FRAGMENT,
                "terminal-transition exact helper operands",
            ),
            (
                WORKER_ATTEMPT_TERMINAL_REFRESH_GUARD_FRAGMENT,
                "terminal-transition single-use guard",
            ),
            (
                WORKER_ATTEMPT_TERMINAL_REFRESH_SET_FRAGMENT,
                "terminal-transition single-use commit",
            ),
            (
                WORKER_ATTEMPT_IMMEDIATE_REFRESH_FRAGMENT,
                "pre-lock transition adjudication",
            ),
            (
                WORKER_ATTEMPT_TARGET_CREATE_FRAGMENT,
                "new target-lock acquisition",
            ),
            (
                WORKER_ATTEMPT_TARGET_OPEN_FRAGMENT,
                "existing target-lock acquisition",
            ),
            (
                WORKER_ATTEMPT_HELD_REFRESH_FRAGMENT,
                "first held-target transition adjudication",
            ),
            (
                WORKER_ATTEMPT_HELD_CAPTURE_FRAGMENT,
                "first held-target inventory witness",
            ),
            (
                WORKER_ATTEMPT_HELD_CONFIRM_CAPTURE_FRAGMENT,
                "confirmed held-target inventory witness",
            ),
            (
                WORKER_ATTEMPT_HELD_CONFIRM_MATCH_FRAGMENT,
                "confirmed held-target exact match",
            ),
            (
                WORKER_ATTEMPT_AFTER_TARGET_HOOK_FRAGMENT,
                "post-held-target test seam",
            ),
            (
                WORKER_ATTEMPT_TARGET_TRANSFER_FRAGMENT,
                "validated target-lock capability transfer",
            ),
        )
        for fragment, description in required_once:
            if compact_body.count(fragment) != 1:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{description} must appear exactly once",
                )
        if compact_body.count("terminal_transition_refreshed") != 3:
            self.fail(
                relative,
                body_line_offset + 1,
                "terminal-transition single-use state may appear only in its "
                "declaration, guard, and successful commit",
            )
        if (
            compact_body.count("claim.base_lock_at_=") != 1
            or compact_body.count("std::move(target)") != 1
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker-attempt claim must transfer only the exact validated "
                "target lock exactly once",
            )

        immediate_offset = compact_body.find(
            WORKER_ATTEMPT_IMMEDIATE_REFRESH_FRAGMENT
        )
        create_offset = compact_body.find(WORKER_ATTEMPT_TARGET_CREATE_FRAGMENT)
        open_offset = compact_body.find(WORKER_ATTEMPT_TARGET_OPEN_FRAGMENT)
        held_offset = compact_body.find(WORKER_ATTEMPT_HELD_CAPTURE_FRAGMENT)
        held_refresh_offset = compact_body.find(
            WORKER_ATTEMPT_HELD_REFRESH_FRAGMENT
        )
        hook_offset = compact_body.find(
            WORKER_ATTEMPT_AFTER_TARGET_HOOK_FRAGMENT
        )
        post_hook_revalidation_offset = compact_body.find(
            "revalidate_higher_priority_bindings()", hook_offset
        )
        confirmed_offset = compact_body.find(
            WORKER_ATTEMPT_HELD_CONFIRM_CAPTURE_FRAGMENT
        )
        confirmed_match_offset = compact_body.find(
            WORKER_ATTEMPT_HELD_CONFIRM_MATCH_FRAGMENT
        )
        closed_successor_offset = compact_body.find(
            "constautorevalidate_closed_successor="
        )
        transfer_offset = compact_body.find(WORKER_ATTEMPT_TARGET_TRANSFER_FRAGMENT)
        if not (
            immediate_offset >= 0
            and create_offset > immediate_offset
            and open_offset > immediate_offset
            and held_offset > max(create_offset, open_offset)
            and held_refresh_offset > held_offset
            and hook_offset > held_refresh_offset
            and post_hook_revalidation_offset > hook_offset
            and confirmed_offset > post_hook_revalidation_offset
            and confirmed_match_offset > confirmed_offset
            and closed_successor_offset > confirmed_match_offset
            and transfer_offset > closed_successor_offset
        ):
            self.fail(
                relative,
                body_line_offset + 1,
                "worker-attempt terminal transition must order pre-lock adjudication, "
                "target acquisition, first held witness, post-held hook, authority "
                "revalidation, exact confirmation, closed successor, and transfer",
            )

    def validate_worker_launcher_use_site(self, relative: str, text: str) -> None:
        if relative in WORKER_LAUNCHER_USE_SITE_ALLOWLIST:
            return
        for identifier in WORKER_LAUNCHER_USE_SITE_IDENTIFIERS:
            for use in find_code_identifier_uses(text, identifier):
                self.fail(
                    relative,
                    use.line,
                    "receipt-gated worker-launcher use site is not "
                    f"allowlisted: {identifier}",
                )

    def validate_worker_executor_composition_body(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_EXECUTOR_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_body(
            text, WORKER_EXECUTOR_COMPOSITION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        call_offsets: dict[str, int] = {}
        for identifier, expected in WORKER_EXECUTOR_COMPOSITION_USE_COUNTS.items():
            all_uses = find_code_identifier_uses(text, identifier)
            body_uses = find_code_identifier_uses(body, identifier)
            body_calls = find_call_identifier_uses(body, identifier)
            if len(all_uses) != len(body_uses):
                self.fail(
                    relative,
                    1,
                    f"all {identifier} executor authority must remain inside "
                    f"{WORKER_EXECUTOR_COMPOSITION_FUNCTION}",
                )
            if len(body_uses) != expected or len(body_calls) != expected:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{WORKER_EXECUTOR_COMPOSITION_FUNCTION} must contain exactly "
                    f"{expected} direct {identifier} call, found "
                    f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                )
            elif expected == 1:
                call_offsets[identifier] = body_calls[0].offset

        if len(call_offsets) == len(WORKER_EXECUTOR_COMPOSITION_CALL_ORDER):
            observed_order = tuple(
                identifier
                for identifier, _ in sorted(
                    call_offsets.items(), key=lambda item: item[1]
                )
            )
            if observed_order != WORKER_EXECUTOR_COMPOSITION_CALL_ORDER:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{WORKER_EXECUTOR_COMPOSITION_FUNCTION} must order runtime "
                    "rehydration, chunk preparation, entry-to-writer consumption, "
                    "and typed handoff publication",
                )

        forbidden_exact = set(
            WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_IDENTIFIERS
        )
        for token, use in find_code_identifier_tokens(text):
            if token in forbidden_exact:
                reason = "forbidden executor identifier"
            elif any(
                token.startswith(prefix)
                for prefix in WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_PREFIXES
            ):
                reason = "legacy distributed-sieve runner identifier"
            elif any(
                fragment in token.lower()
                for fragment in WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_FRAGMENTS
            ):
                reason = "cleanup identifier"
            else:
                continue
            self.fail(
                relative,
                use.line,
                f"{WORKER_EXECUTOR_COMPOSITION_FUNCTION} forbids {reason} {token}",
            )

    def validate_worker_launcher_composition_body(
        self, relative: str, text: str
    ) -> None:
        if relative != WORKER_LAUNCHER_IMPLEMENTATION_FILE:
            return

        body, body_line_offset, body_errors = find_function_body(
            text, WORKER_LAUNCHER_COMPOSITION_FUNCTION
        )
        for line, error in body_errors:
            self.fail(relative, line, error)
        if body is None:
            return

        for identifier, expected in WORKER_LAUNCHER_COMPOSITION_USE_COUNTS.items():
            all_uses = find_code_identifier_uses(text, identifier)
            body_uses = find_code_identifier_uses(body, identifier)
            if len(all_uses) != len(body_uses):
                self.fail(
                    relative,
                    1,
                    f"all {identifier} launcher authority must remain inside "
                    f"{WORKER_LAUNCHER_COMPOSITION_FUNCTION}",
                )

            if identifier in WORKER_LAUNCHER_COMPOSITION_DIRECT_CALL_IDENTIFIERS:
                body_calls = find_call_identifier_uses(body, identifier)
                if len(body_uses) != expected or len(body_calls) != expected:
                    self.fail(
                        relative,
                        body_line_offset + 1,
                        f"{WORKER_LAUNCHER_COMPOSITION_FUNCTION} must contain exactly "
                        f"{expected} direct {identifier} call, found "
                        f"{len(body_uses)} identifiers and {len(body_calls)} calls",
                    )
            elif len(body_uses) != expected:
                self.fail(
                    relative,
                    body_line_offset + 1,
                    f"{WORKER_LAUNCHER_COMPOSITION_FUNCTION} must contain exactly "
                    f"{expected} {identifier} identifiers, found {len(body_uses)}",
                )

    def classify(
        self, relative: str, call: GetenvCall, categories: dict[str, str]
    ) -> None:
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
                self.fail(
                    relative, call.line, f"unclassified GNFS environment read {flag}"
                )
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
                ClassifiedRead(
                    category=category, name=flag, relative=relative, line=call.line
                )
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
        if (
            len(observed) == len(expected)
            and set(observed) == set(expected)
            and observed != expected
        ):
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
        for relative in sorted(WORKER_COORDINATOR_PRODUCTION_FILES):
            if not (self.root / relative).is_file():
                self.fail(
                    relative,
                    1,
                    "worker-coordinator production inventory file is missing",
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
            self.validate_worker_executor_composition_body(relative, text)
            self.validate_worker_launcher_composition_body(relative, text)
            self.validate_merge_coordinator_boundary(relative, text)
            self.validate_worker_coordinator_boundary(relative, text)
            self.validate_worker_attempt_terminal_transition_boundary(relative, text)
            self.validate_work_package_residue_inspection_body(relative, text)
            self.validate_work_package_residue_reconciliation_body(relative, text)
            if relative == EXECUTION_POLICY_ENVIRONMENT_ADAPTER:
                self.validate_environment_adapter(text, calls)
            for call in calls:
                self.classify(relative, call, categories)
            for use in find_code_identifier_uses(text, "random_device"):
                self.classify_random_device(relative, use)

        try:
            pipeline_text = (self.root / LEGACY_PIPELINE_FILE).read_text(
                encoding="utf-8"
            )
        except (OSError, UnicodeError) as exc:
            self.fail(LEGACY_PIPELINE_FILE, 1, f"cannot read source: {exc}")
        else:
            self.validate_legacy_pipeline_boundary(pipeline_text)

        bound_work_files = self.bound_work_source_files()
        wave_store_path = (
            self.root / PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE
        )
        try:
            wave_store_text = wave_store_path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            self.fail(
                PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
                1,
                f"cannot read MergePrepared protected token inventory: {exc}",
            )
        else:
            self.merge_prepared_protected_tokens = (
                _merge_prepared_protected_code_tokens(wave_store_text)
            )
            if not self.merge_prepared_protected_tokens:
                self.fail(
                    PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
                    1,
                    "MergePrepared protected token inventory is empty",
                )

        for relative, path in bound_work_files:
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                self.fail(relative, 1, f"cannot read bound-work use site: {exc}")
                continue
            self.validate_bound_work_use_site(relative, text)
            self.validate_work_package_carrier_use_site(relative, text)
            self.validate_work_package_residue_inspector_use_site(relative, text)
            self.validate_work_package_residue_reconciler_use_site(relative, text)
            self.validate_work_package_residue_reconciler_definition_boundary(
                relative, text
            )
            self.validate_work_package_fixed_leaf_use_site(relative, text)
            self.validate_production_raw_unlinkat_authority(relative, text)
            self.validate_work_package_carrier_unlink_authority(relative, text)
            self.validate_wave_store_private_lease_unlink_authority(relative, text)
            self.validate_raw_work_package_fixed_leaf_unlink(relative, text)
            self.validate_work_package_reader_use_site(relative, text)
            self.validate_worker_process_fixed_capability_use_site(relative, text)
            self.validate_worker_entry_use_site(relative, text)
            self.validate_worker_writer_use_site(relative, text)
            self.validate_borrowed_base_lock_bridge(relative, text)
            self.validate_consumed_canonical_adoption_bridge(relative, text)
            self.validate_merge_prepared_admission_boundary(relative, text)
            self.validate_worker_writer_identifier_exception_boundary(
                relative, text
            )
            self.validate_worker_handoff_publication_boundary(relative, text)
            self.validate_merge_prepared_macro_alias_boundary(relative, text)
            self.validate_private_handoff_publication_resume_boundary(
                relative, text
            )
            self.validate_merge_generation_authority_use_site(relative, text)
            self.validate_merge_coordinator_use_site(relative, text)
            self.validate_worker_coordinator_use_site(relative, text)
            self.validate_worker_launcher_use_site(relative, text)

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

        for (
            relative,
            identifier,
        ), expected in WORKER_PROCESS_REQUIRED_DIRECT_CALLS.items():
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

    snippet = r"""
// getenv("GNFS_NOT_A_REAL_READ")
const char* text = "getenv(\"GNFS_NOT_A_REAL_READ\")";
const char* semantic = std::getenv("GNFS_LATTICE_LLL");
const char* conservative = std::getenv /* inventory trivia */ ("GNFS_ECM_BATCH_INV");
const char* injected = std::getenv(
    ("GNFS_DISTRIBUTED_SIEVE_FAIL_ATTEMPT_" + std::to_string(chunk_id)).c_str());
"""
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

    alias_snippet = r"""
auto getter = std::getenv;
const char* value = getter("GNFS_LATTICE_LLL");
const char* direct = std::getenv("GNFS_LATTICE_LLL");
"""
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
        r"""
// posix_spawn(); waitpid(); fork(); environ;
const char* text = "posix_spawn(); waitpid(); fork(); environ;";
const auto spawn_result = ::posix_spawn(&child, path, &actions, &attributes, argv, envp);
const auto observed = ::waitpid(child, &status, 0);
""",
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
        "const auto child = ::fork();\n" "char** inherited_environment = environ;\n",
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

    alternate_execution_snippet = "".join(
        f"const auto alternate_{index} = &::{identifier};\n"
        for index, identifier in enumerate(
            ALTERNATE_PROCESS_EXECUTION_IDENTIFIERS
        )
    )
    alternate_process_checks = Checks(Path("."))
    alternate_process_checks.validate_worker_process_transport_boundary(
        "include/gnfs/sieve/other.hpp",
        "const auto a = ::_Fork();\n"
        "const auto b = ::vfork();\n"
        "const auto c = ::posix_spawnp(&pid, name, actions, attrs, argv, envp);\n"
        "const auto d = ::waitid(P_PID, pid, &status, WEXITED);\n"
        "const auto e = ::wait3(&status, 0, nullptr);\n"
        "const auto f = ::wait4(pid, &status, 0, nullptr);\n"
        + alternate_execution_snippet,
    )
    expect(
        len(alternate_process_checks.errors)
        == 6 + len(ALTERNATE_PROCESS_EXECUTION_IDENTIFIERS)
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
            "is only allowed in" in error and wrong_file_entry.relative in error
            for error in wrong_file_literal_checks.errors
        )
        and any(
            wrong_file_entry.name in error and "found 0" in error
            for error in wrong_file_literal_checks.errors
        ),
        f"wrong-file legacy execution-policy literal read was not rejected: "
        f"{wrong_file_literal_checks.errors}",
    )

    identifier_snippet = r"""
// std::random_device ignored_comment;
const char* ignored_text = "random_device";
std::random_device actual;
int random_device_suffix = 0;
"""
    identifier_uses = find_code_identifier_uses(identifier_snippet, "random_device")
    expect(
        len(identifier_uses) == 1 and identifier_uses[0].line == 4,
        f"random_device code-token scan failed: {identifier_uses}",
    )

    expect(
        WORKER_EXECUTOR_DURABLE_FILES
        == {
            "src/sieve/distributed_sieve_worker_runtime.cpp",
            "src/sieve/distributed_sieve_worker_runtime_internal.hpp",
            "src/sieve/distributed_sieve_worker_chunk.cpp",
            "src/sieve/distributed_sieve_worker_chunk_internal.hpp",
            WORKER_EXECUTOR_IMPLEMENTATION_FILE,
            WORKER_EXECUTOR_INTERFACE_FILE,
        }
        and WORKER_EXECUTOR_DURABLE_FILES <= DURABLE_ENVIRONMENT_FREE_FILES,
        "worker executor runtime/chunk/execution durable inventory is not exact",
    )
    expect(
        BOUND_WORK_USE_SITE_ALLOWLIST & WORKER_EXECUTOR_DURABLE_FILES
        == WORKER_EXECUTOR_BOUND_WORK_USE_SITE_FILES,
        "worker executor bound-work allowlist is not exactly the runtime/chunk boundary",
    )
    expect(
        MERGE_STREAM_WRITER_FILES <= DURABLE_ENVIRONMENT_FREE_FILES,
        "streaming merge writer files must remain in the durable environment-free inventory",
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
        durable_checks.errors[-1] == f"{durable_relative}:37: "
        "durable protocol/execution-policy implementation must not use random_device",
        "durable-path random_device ban is not enforced",
    )

    merge_stream_checks = Checks(Path("."))
    merge_stream_checks.validate_durable_ambient_api_uses(
        "src/sieve/distributed_sieve_merge_writer_internal.cpp",
        r"""
auto all = reader.read_all();
auto range = reader.read_range(0, 1);
OOCPrivateLease lease;
OOCCleanupTransaction cleanup;
OOCPrivateHandoffPayloadBuilderV1 builder;
auto evidence = capture_finalized_corpus_evidence(writer);
writer.finalize_and_publish_private_handoff(payload);
writer.finalize_and_publish_private_handoff_built(callback);
writer.abort_and_remove_owned_fresh_artifacts_noexcept();
writer.remove_owned_artifacts_noexcept();
""",
    )
    expect(
        len(merge_stream_checks.errors)
        == len(MERGE_STREAM_WRITER_FORBIDDEN_IDENTIFIERS)
        and all(
            any(identifier in error for error in merge_stream_checks.errors)
            for identifier in MERGE_STREAM_WRITER_FORBIDDEN_IDENTIFIERS
        ),
        f"streaming merge authority/full-corpus bans are not closed: "
        f"{merge_stream_checks.errors}",
    )

    mapper_relative = sorted(DURABLE_PURE_RUNTIME_MAPPER_FILES)[0]
    mapper_api_snippet = r"""
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
"""
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
        and any("ambient API from_env" in error for error in mapper_api_checks.errors)
        and any(
            "legacy runtime API LatticeSieve" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "legacy runtime API compute_lattice_basis_with_skewness" in error
            for error in mapper_api_checks.errors
        )
        and any(
            "must not call legacy runtime API lattice_basis_parallel_threads" in error
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
        and "legacy runtime API brent_pollard_enabled" in provider_api_checks.errors[0],
        "distributed seed-provider indirect ambient API ban is not enforced",
    )

    bound_work_checks = Checks(Path("."))
    bound_work_relative = "src/sieve/distributed_sieve_bound_work.cpp"
    bound_work_checks.validate_durable_ambient_api_uses(
        bound_work_relative,
        r"""
DistributedSieveWaveStore store;
DistributedSieveWorkerAttemptStartReceipt receipt;
RelationCollector collector;
auto pid = process_id();
auto rows = run_distributed_sieve(config);
auto fork_pointer = &fork;
auto descriptor = open(path, flags);
std::ofstream output(path);
auto now = std::chrono::steady_clock::now();
""",
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
        r"""
auto bound = bind_distributed_sieve_work_v1(identity, frozen, context, factor_base);
auto rows = run_distributed_sieve(config, context, factor_base, bound.sieve_parameters);
""",
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
        and any(
            relative == "src/linalg/metal_spmv.mm"
            for relative, _ in repository_inventory
        ),
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
        r"""
DistributedSieveWorkerWorkPackageFileV1* token = nullptr;
auto result = create_distributed_sieve_worker_work_package_file_v1(request, identity);
""",
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

    residue_inspector_production_snippet = r"""
DistributedSieveWorkerWorkPackageResidueInspectionRequestV1 request;
DistributedSieveWorkerWorkPackageResidueWitnessV1 witness;
DistributedSieveWorkerWorkPackageResidueInspectionResultV1 result;
auto inspected = inspect_distributed_sieve_worker_work_package_residue_v1(request);
"""
    residue_inspector_with_ops_snippet = r"""
auto injected =
    inspect_distributed_sieve_worker_work_package_residue_v1_with_ops(request, ops);
"""
    residue_inspector_use_site_snippet = (
        residue_inspector_production_snippet + residue_inspector_with_ops_snippet
    )
    residue_inspector_use_site_checks = Checks(Path("."))
    residue_inspector_use_site_checks.validate_work_package_residue_inspector_use_site(
        "src/sieve/untrusted_residue_recovery.cpp",
        residue_inspector_use_site_snippet,
    )
    expect(
        len(residue_inspector_use_site_checks.errors) == 5
        and all(
            "named work-package residue inspector is not allowlisted" in error
            for error in residue_inspector_use_site_checks.errors
        ),
        "named work-package residue inspector use-site gate is not enforced: "
        f"{residue_inspector_use_site_checks.errors}",
    )
    for relative in sorted(WORK_PACKAGE_RESIDUE_INSPECTOR_USE_SITE_ALLOWLIST):
        allowed_residue_inspector_checks = Checks(Path("."))
        allowed_residue_inspector_checks.validate_work_package_residue_inspector_use_site(
            relative, residue_inspector_production_snippet
        )
        expect(
            not allowed_residue_inspector_checks.errors,
            "allowlisted named work-package residue inspector use was rejected in "
            f"{relative}: {allowed_residue_inspector_checks.errors}",
        )
    for relative in sorted(WORK_PACKAGE_RESIDUE_INSPECTOR_WITH_OPS_ALLOWLIST):
        allowed_residue_with_ops_checks = Checks(Path("."))
        allowed_residue_with_ops_checks.validate_work_package_residue_inspector_use_site(
            relative, residue_inspector_with_ops_snippet
        )
        expect(
            not allowed_residue_with_ops_checks.errors,
            "allowlisted test-only work-package residue inspector use was rejected in "
            f"{relative}: {allowed_residue_with_ops_checks.errors}",
        )
    wave_store_with_ops_checks = Checks(Path("."))
    wave_store_with_ops_checks.validate_work_package_residue_inspector_use_site(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        residue_inspector_with_ops_snippet,
    )
    expect(
        len(wave_store_with_ops_checks.errors) == 1
        and "test-only named work-package residue inspector is not allowlisted"
        in wave_store_with_ops_checks.errors[0],
        "WaveStore gained the test-only residue inspector: "
        f"{wave_store_with_ops_checks.errors}",
    )

    valid_residue_inspection_composition = r"""
auto validate_private_lease_attempt_inventory() {
    auto inspected =
        inspect_distributed_sieve_worker_work_package_residue_v1(request);
    return inspected;
}
auto reconcile_worker_attempt_started() {
    auto expanded =
        inspect_distributed_sieve_worker_work_package_residue_v1(request);
    return expanded;
}
auto capture_manifest_bound_inventory_witness() {
    return validate_private_lease_attempt_inventory();
}
"""
    exact_residue_inspection_checks = Checks(Path("."))
    exact_residue_inspection_checks.validate_work_package_residue_inspection_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        valid_residue_inspection_composition,
    )
    expect(
        not exact_residue_inspection_checks.errors,
        "exact WaveStore residue-inspection body was rejected: "
        f"{exact_residue_inspection_checks.errors}",
    )

    outside_residue_inspection = (
        valid_residue_inspection_composition
        + r"""
auto bypass =
    inspect_distributed_sieve_worker_work_package_residue_v1(request);
"""
    )
    outside_residue_inspection_checks = Checks(Path("."))
    outside_residue_inspection_checks.validate_work_package_residue_inspection_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        outside_residue_inspection,
    )
    expect(
        len(outside_residue_inspection_checks.errors) == 1
        and "uses must remain inside "
        f"{' and '.join(WORK_PACKAGE_RESIDUE_INSPECTION_FUNCTIONS)}"
        in outside_residue_inspection_checks.errors[0],
        "same-file outside-function residue-inspector bypass was not rejected: "
        f"{outside_residue_inspection_checks.errors}",
    )

    addressed_residue_inspection = (
        valid_residue_inspection_composition
        + r"""
auto inspector_alias =
    &inspect_distributed_sieve_worker_work_package_residue_v1;
"""
    )
    addressed_residue_inspection_checks = Checks(Path("."))
    addressed_residue_inspection_checks.validate_work_package_residue_inspection_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        addressed_residue_inspection,
    )
    expect(
        len(addressed_residue_inspection_checks.errors) == 1
        and "uses must remain inside "
        f"{' and '.join(WORK_PACKAGE_RESIDUE_INSPECTION_FUNCTIONS)}"
        in addressed_residue_inspection_checks.errors[0],
        "same-file residue-inspector address bypass was not rejected: "
        f"{addressed_residue_inspection_checks.errors}",
    )

    for function_name, return_line in (
        ("validate_private_lease_attempt_inventory", "    return inspected;\n"),
        ("reconcile_worker_attempt_started", "    return expanded;\n"),
    ):
        duplicate_residue_inspection = valid_residue_inspection_composition.replace(
            return_line,
            "    auto duplicate =\n"
            "        inspect_distributed_sieve_worker_work_package_residue_v1(request);\n"
            + return_line,
            1,
        )
        duplicate_residue_inspection_checks = Checks(Path("."))
        duplicate_residue_inspection_checks.validate_work_package_residue_inspection_body(
            WORKER_LAUNCHER_IMPLEMENTATION_FILE,
            duplicate_residue_inspection,
        )
        expect(
            len(duplicate_residue_inspection_checks.errors) == 1
            and f"{function_name} must contain exactly 1 direct "
            f"{WORK_PACKAGE_RESIDUE_INSPECTION_CALL} call"
            in duplicate_residue_inspection_checks.errors[0],
            f"WaveStore {function_name} residue-inspector call count is not closed: "
            f"{duplicate_residue_inspection_checks.errors}",
        )

    legacy_residue_inspector_checks = Checks(Path("."))
    legacy_residue_inspector_checks.validate_work_package_residue_inspector_use_site(
        WORKER_PROCESS_LEGACY_FILE,
        "auto inspected = "
        "inspect_distributed_sieve_worker_work_package_residue_v1(request);",
    )
    expect(
        len(legacy_residue_inspector_checks.errors) == 1
        and "named work-package residue inspector is not allowlisted"
        in legacy_residue_inspector_checks.errors[0],
        "legacy distributed runner is not isolated from residue inspection",
    )

    residue_reconciler_production_snippet = r"""
DistributedSieveWorkerWorkPackageResidueReconciliationDispositionV1 disposition;
DistributedSieveWorkerWorkPackageResidueReconciliationFaultPointV1 fault_point;
DistributedSieveWorkerWorkPackageResidueReconciliationRequestV1 request;
DistributedSieveWorkerWorkPackageResidueReconciliationResultV1 result;
DistributedSieveWorkerWorkPackageResidueReconciliationTestHooksV1 hooks;
auto reconciled =
    reconcile_distributed_sieve_worker_work_package_residue_v1(request, hooks);
"""
    residue_reconciler_with_ops_snippet = r"""
auto injected =
    reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
        request, ops, hooks);
"""
    residue_reconciler_use_site_checks = Checks(Path("."))
    residue_reconciler_use_site_checks.validate_work_package_residue_reconciler_use_site(
        "src/sieve/untrusted_residue_cleanup.cpp",
        residue_reconciler_production_snippet + residue_reconciler_with_ops_snippet,
    )
    expect(
        len(residue_reconciler_use_site_checks.errors) == 7
        and all(
            "named work-package residue reconciliation authority is not allowlisted"
            in error
            for error in residue_reconciler_use_site_checks.errors[:6]
        )
        and "test-only named work-package residue reconciler is not allowlisted"
        in residue_reconciler_use_site_checks.errors[6],
        "named work-package residue reconciler use-site gate is not enforced: "
        f"{residue_reconciler_use_site_checks.errors}",
    )
    for relative in sorted(WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_ALLOWLIST):
        allowed_residue_reconciler_checks = Checks(Path("."))
        allowed_residue_reconciler_checks.validate_work_package_residue_reconciler_use_site(
            relative, residue_reconciler_production_snippet
        )
        expect(
            not allowed_residue_reconciler_checks.errors,
            "allowlisted named work-package residue reconciler use was rejected in "
            f"{relative}: {allowed_residue_reconciler_checks.errors}",
        )
    expect(
        WORK_PACKAGE_RESIDUE_RECONCILER_USE_SITE_ALLOWLIST
        == {
            "src/sieve/distributed_sieve_worker_work_package_file.cpp",
            "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
            WORKER_LAUNCHER_IMPLEMENTATION_FILE,
            "tests/test_distributed_sieve_worker_work_package_file.cpp",
        },
        "named work-package residue reconciler allowlist is not the exact "
        "carrier/WaveStore/test boundary",
    )
    for relative in sorted(WORK_PACKAGE_RESIDUE_RECONCILER_WITH_OPS_ALLOWLIST):
        allowed_residue_reconciler_with_ops_checks = Checks(Path("."))
        allowed_residue_reconciler_with_ops_checks.validate_work_package_residue_reconciler_use_site(
            relative, residue_reconciler_with_ops_snippet
        )
        expect(
            not allowed_residue_reconciler_with_ops_checks.errors,
            "allowlisted test-only work-package residue reconciler was rejected in "
            f"{relative}: {allowed_residue_reconciler_with_ops_checks.errors}",
        )
    wave_store_reconciler_with_ops_checks = Checks(Path("."))
    wave_store_reconciler_with_ops_checks.validate_work_package_residue_reconciler_use_site(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        residue_reconciler_with_ops_snippet,
    )
    expect(
        len(wave_store_reconciler_with_ops_checks.errors) == 1
        and "test-only named work-package residue reconciler is not allowlisted"
        in wave_store_reconciler_with_ops_checks.errors[0],
        "WaveStore gained the test-only residue reconciler: "
        f"{wave_store_reconciler_with_ops_checks.errors}",
    )

    valid_residue_reconciler_interface = r"""
Result reconcile_distributed_sieve_worker_work_package_residue_v1(
    const Request&, const Hooks&);
Result reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops(
    const Request&, Ops&, const Hooks&);
"""
    valid_residue_reconciler_interface_checks = Checks(Path("."))
    valid_residue_reconciler_interface_checks.validate_work_package_residue_reconciler_definition_boundary(
        WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE,
        valid_residue_reconciler_interface,
    )
    expect(
        not valid_residue_reconciler_interface_checks.errors,
        "exact residue reconciler carrier declarations were rejected: "
        f"{valid_residue_reconciler_interface_checks.errors}",
    )
    inline_residue_reconciler_wrapper_checks = Checks(Path("."))
    inline_residue_reconciler_wrapper_checks.validate_work_package_residue_reconciler_definition_boundary(
        WORK_PACKAGE_RESIDUE_RECONCILER_INTERFACE_FILE,
        valid_residue_reconciler_interface
        + r"""
inline auto cleanup_wrapper() {
    return reconcile_distributed_sieve_worker_work_package_residue_v1(
        request, hooks);
}
""",
    )
    expect(
        len(inline_residue_reconciler_wrapper_checks.errors) == 1
        and "carrier interface must contain exactly 1 declaration-shaped"
        in inline_residue_reconciler_wrapper_checks.errors[0],
        "inline carrier-header residue cleanup wrapper escaped count closure: "
        f"{inline_residue_reconciler_wrapper_checks.errors}",
    )

    valid_residue_reconciler_implementation = r"""
auto reconcile_distributed_sieve_worker_work_package_residue_v1() {
    return production_result;
}
auto reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops() {
    return injected_result;
}
"""
    valid_residue_reconciler_implementation_checks = Checks(Path("."))
    valid_residue_reconciler_implementation_checks.validate_work_package_residue_reconciler_definition_boundary(
        WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE,
        valid_residue_reconciler_implementation,
    )
    expect(
        not valid_residue_reconciler_implementation_checks.errors,
        "exact residue reconciler carrier definitions were rejected: "
        f"{valid_residue_reconciler_implementation_checks.errors}",
    )
    carrier_residue_reconciler_wrapper_checks = Checks(Path("."))
    carrier_residue_reconciler_wrapper_checks.validate_work_package_residue_reconciler_definition_boundary(
        WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE,
        valid_residue_reconciler_implementation
        + r"""
auto cleanup_wrapper() {
    return reconcile_distributed_sieve_worker_work_package_residue_v1();
}
""",
    )
    expect(
        len(carrier_residue_reconciler_wrapper_checks.errors) == 1
        and "carrier implementation must contain only the exact"
        in carrier_residue_reconciler_wrapper_checks.errors[0],
        "carrier implementation residue cleanup wrapper escaped count closure: "
        f"{carrier_residue_reconciler_wrapper_checks.errors}",
    )

    valid_carrier_unlink_authority = r"""
auto unlink_at() {
    return ::unlinkat(directory, leaf, 0);
}
auto run_residue_reconciliation() {
    return ops.unlink_at(directory);
}
auto run_file_creation() {
    return ops.unlink_at(directory);
}
auto reconcile_distributed_sieve_worker_work_package_residue_v1() {
    return run_residue_reconciliation();
}
auto reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops() {
    return run_residue_reconciliation();
}
"""
    valid_carrier_unlink_authority_checks = Checks(Path("."))
    valid_carrier_unlink_authority_checks.validate_work_package_carrier_unlink_authority(
        WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE,
        valid_carrier_unlink_authority,
    )
    expect(
        not valid_carrier_unlink_authority_checks.errors,
        "exact carrier unlink authority was rejected: "
        f"{valid_carrier_unlink_authority_checks.errors}",
    )
    valid_carrier_ops_unlink_interface = r"""
virtual Result unlink_at(Handle directory) noexcept = 0;
"""
    valid_carrier_ops_unlink_interface_checks = Checks(Path("."))
    valid_carrier_ops_unlink_interface_checks.validate_work_package_carrier_unlink_authority(
        WORK_PACKAGE_CARRIER_OPS_INTERFACE_FILE,
        valid_carrier_ops_unlink_interface,
    )
    expect(
        not valid_carrier_ops_unlink_interface_checks.errors,
        "exact carrier ops unlink declaration was rejected: "
        f"{valid_carrier_ops_unlink_interface_checks.errors}",
    )
    carrier_ops_inline_wrapper_checks = Checks(Path("."))
    carrier_ops_inline_wrapper_checks.validate_work_package_carrier_unlink_authority(
        WORK_PACKAGE_CARRIER_OPS_INTERFACE_FILE,
        valid_carrier_ops_unlink_interface
        + r"""
inline auto erase(Ops& ops, Handle directory) {
    return ops.unlink_at(directory);
}
""",
    )
    expect(
        len(carrier_ops_inline_wrapper_checks.errors) == 1
        and "carrier ops interface must contain exactly 1 declaration-shaped"
        in carrier_ops_inline_wrapper_checks.errors[0],
        "inline carrier ops unlink wrapper escaped interface count closure: "
        f"{carrier_ops_inline_wrapper_checks.errors}",
    )
    carrier_ops_external_caller_checks = Checks(Path("."))
    carrier_ops_external_caller_checks.validate_work_package_carrier_unlink_authority(
        "src/sieve/untrusted_residue_cleanup.cpp",
        "auto removed = erase(ops, directory);",
    )
    expect(
        not carrier_ops_external_caller_checks.errors,
        "renamed external caller should be closed by rejecting its inline ops wrapper: "
        f"{carrier_ops_external_caller_checks.errors}",
    )
    for bypass_call, expected_message in (
        (
            "run_residue_reconciliation()",
            "all run_residue_reconciliation calls must remain inside",
        ),
        (
            "ops.unlink_at(directory)",
            "all carrier unlink_at calls must remain inside",
        ),
    ):
        renamed_carrier_cleanup_wrapper = (
            valid_carrier_unlink_authority + "\nauto cleanup_wrapper() {\n"
            f"    return {bypass_call};\n"
            "}\n"
        )
        renamed_carrier_cleanup_wrapper_checks = Checks(Path("."))
        renamed_carrier_cleanup_wrapper_checks.validate_work_package_carrier_unlink_authority(
            WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE,
            renamed_carrier_cleanup_wrapper,
        )
        expect(
            len(renamed_carrier_cleanup_wrapper_checks.errors) == 1
            and expected_message in renamed_carrier_cleanup_wrapper_checks.errors[0],
            "renamed carrier cleanup wrapper escaped the internal authority gate: "
            f"{renamed_carrier_cleanup_wrapper_checks.errors}",
        )

    carrier_test_residue_reconciler_alias_checks = Checks(Path("."))
    carrier_test_residue_reconciler_alias_checks.validate_work_package_residue_reconciler_definition_boundary(
        WORK_PACKAGE_RESIDUE_RECONCILER_TEST_FILE,
        "auto cleanup_alias = "
        "&reconcile_distributed_sieve_worker_work_package_residue_v1_with_ops;",
    )
    expect(
        len(carrier_test_residue_reconciler_alias_checks.errors) == 1
        and "dedicated carrier test must use"
        in carrier_test_residue_reconciler_alias_checks.errors[0],
        "carrier test residue reconciler alias escaped the direct-call rule: "
        f"{carrier_test_residue_reconciler_alias_checks.errors}",
    )

    valid_residue_reconciliation_composition = r"""
auto reconcile_worker_attempt_started() noexcept {
    auto reconciled =
        reconcile_distributed_sieve_worker_work_package_residue_v1(request, hooks);
    return reconciled;
}
"""
    exact_residue_reconciliation_checks = Checks(Path("."))
    exact_residue_reconciliation_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        valid_residue_reconciliation_composition,
    )
    expect(
        not exact_residue_reconciliation_checks.errors,
        "exact WaveStore residue-reconciliation body was rejected: "
        f"{exact_residue_reconciliation_checks.errors}",
    )

    outside_residue_reconciliation = (
        valid_residue_reconciliation_composition
        + r"""
auto launcher_bypass =
    reconcile_distributed_sieve_worker_work_package_residue_v1(request, hooks);
"""
    )
    outside_residue_reconciliation_checks = Checks(Path("."))
    outside_residue_reconciliation_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        outside_residue_reconciliation,
    )
    expect(
        len(outside_residue_reconciliation_checks.errors) == 1
        and "uses must remain inside "
        f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION}"
        in outside_residue_reconciliation_checks.errors[0],
        "same-file launcher residue-cleanup bypass was not rejected: "
        f"{outside_residue_reconciliation_checks.errors}",
    )

    addressed_residue_reconciliation = (
        valid_residue_reconciliation_composition
        + r"""
auto reconciler_alias =
    &reconcile_distributed_sieve_worker_work_package_residue_v1;
"""
    )
    addressed_residue_reconciliation_checks = Checks(Path("."))
    addressed_residue_reconciliation_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        addressed_residue_reconciliation,
    )
    expect(
        len(addressed_residue_reconciliation_checks.errors) == 1
        and "uses must remain inside "
        f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_FUNCTION}"
        in addressed_residue_reconciliation_checks.errors[0],
        "same-file residue-reconciler address alias was not rejected: "
        f"{addressed_residue_reconciliation_checks.errors}",
    )

    duplicate_residue_reconciliation = valid_residue_reconciliation_composition.replace(
        "    return reconciled;\n",
        "    auto duplicate =\n"
        "        reconcile_distributed_sieve_worker_work_package_residue_v1(request, hooks);\n"
        "    return duplicate;\n",
    )
    duplicate_residue_reconciliation_checks = Checks(Path("."))
    duplicate_residue_reconciliation_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        duplicate_residue_reconciliation,
    )
    expect(
        len(duplicate_residue_reconciliation_checks.errors) == 1
        and "must contain exactly 1 direct "
        f"{WORK_PACKAGE_RESIDUE_RECONCILIATION_CALL} call"
        in duplicate_residue_reconciliation_checks.errors[0],
        "WaveStore residue-reconciler call count is not closed: "
        f"{duplicate_residue_reconciliation_checks.errors}",
    )

    for (
        forbidden_unlink
    ) in WORK_PACKAGE_RESIDUE_RECONCILIATION_FORBIDDEN_UNLINK_IDENTIFIERS:
        unlink_bypass = valid_residue_reconciliation_composition.replace(
            "    return reconciled;\n",
            f"    auto cleanup_alias = &{forbidden_unlink};\n"
            "    (void)cleanup_alias;\n"
            "    return reconciled;\n",
        )
        unlink_bypass_checks = Checks(Path("."))
        unlink_bypass_checks.validate_work_package_residue_reconciliation_body(
            WORKER_LAUNCHER_IMPLEMENTATION_FILE,
            unlink_bypass,
        )
        expect(
            len(unlink_bypass_checks.errors) == 1
            and f"must not bypass the carrier through {forbidden_unlink}"
            in unlink_bypass_checks.errors[0],
            f"{forbidden_unlink} alias escaped the exact reconciliation body gate: "
            f"{unlink_bypass_checks.errors}",
        )

    for forbidden_relative in (
        WORKER_PROCESS_LEGACY_FILE,
        LEGACY_PIPELINE_FILE,
        "src/relation/relation_collector.cpp",
    ):
        forbidden_reconciler_checks = Checks(Path("."))
        forbidden_reconciler_checks.validate_work_package_residue_reconciler_use_site(
            forbidden_relative,
            "reconcile_distributed_sieve_worker_work_package_residue_v1(request, hooks);",
        )
        expect(
            len(forbidden_reconciler_checks.errors) == 1
            and "named work-package residue reconciliation authority is not allowlisted"
            in forbidden_reconciler_checks.errors[0],
            f"{forbidden_relative} is not isolated from residue reconciliation: "
            f"{forbidden_reconciler_checks.errors}",
        )

    valid_wave_store_unlink_authority = r"""
auto private_lease_unlink_at(int parent, const char* leaf, int flags) noexcept {
    return ::unlinkat(parent, leaf, flags);
}
void DistributedSieveFdPrivateLeaseRecoveryTarget::unlink_exact_marker() {
    (void)private_lease_unlink_at(parent, leaf, 0);
}
void DistributedSieveFdPrivateLeaseRecoveryTarget::remove_exact_empty_staging_directory() {
    (void)private_lease_unlink_at(parent, staging, AT_REMOVEDIR);
}
"""
    valid_wave_store_unlink_checks = Checks(Path("."))
    valid_wave_store_unlink_checks.validate_wave_store_private_lease_unlink_authority(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        valid_wave_store_unlink_authority,
    )
    expect(
        not valid_wave_store_unlink_checks.errors,
        "exact WaveStore private-lease unlink authority was rejected: "
        f"{valid_wave_store_unlink_checks.errors}",
    )

    helper_fixed_leaf_bypass = (
        valid_wave_store_unlink_authority
        + valid_residue_reconciliation_composition.replace(
            "    return reconciled;\n",
            "    (void)cleanup_worker_package_bypass();\n" "    return reconciled;\n",
        )
        + r"""
auto cleanup_worker_package_bypass() {
    const auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
    return private_lease_unlink_at(directory, leaf.data(), 0);
}
"""
    )
    helper_fixed_leaf_bypass_checks = Checks(Path("."))
    helper_fixed_leaf_bypass_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        helper_fixed_leaf_bypass,
    )
    helper_fixed_leaf_bypass_checks.validate_wave_store_private_lease_unlink_authority(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        helper_fixed_leaf_bypass,
    )
    expect(
        helper_fixed_leaf_bypass_checks.errors
        == [
            f"{WORKER_LAUNCHER_IMPLEMENTATION_FILE}:1: all "
            f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER} calls must remain inside "
            "the exact private-lease recovery bodies"
        ],
        "same-file aliased fixed-leaf helper bypass escaped the WaveStore gate: "
        f"{helper_fixed_leaf_bypass_checks.errors}",
    )

    raw_alias_fixed_leaf_checks = Checks(Path("."))
    raw_alias_fixed_leaf_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/untrusted_residue_cleanup.cpp",
        r"""
auto raw_unlink = &::unlinkat;
const auto leaf = DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1;
auto removed = raw_unlink(directory, leaf.data(), 0);
""",
    )
    expect(
        len(raw_alias_fixed_leaf_checks.errors) == 1
        and "fixed work-package leaf cannot share raw unlink authority"
        in raw_alias_fixed_leaf_checks.errors[0],
        "raw unlinkat alias escaped the fixed-leaf carrier gate: "
        f"{raw_alias_fixed_leaf_checks.errors}",
    )

    raw_helper_fixed_leaf_checks = Checks(Path("."))
    raw_helper_fixed_leaf_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/untrusted_residue_cleanup.cpp",
        r"""
auto erase_leaf(int directory, const char* leaf) {
    return ::unlinkat(directory, leaf, 0);
}
auto removed = erase_leaf(
    directory, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1.data());
""",
    )
    expect(
        len(raw_helper_fixed_leaf_checks.errors) == 1
        and "fixed work-package leaf cannot share raw unlink authority"
        in raw_helper_fixed_leaf_checks.errors[0],
        "raw unlinkat helper escaped the fixed-leaf carrier gate: "
        f"{raw_helper_fixed_leaf_checks.errors}",
    )

    cross_file_raw_helper_checks = Checks(Path("."))
    cross_file_raw_helper_source = r"""
int erase_leaf(int directory, const char* leaf) {
    return ::unlinkat(directory, leaf, 0);
}
"""
    cross_file_raw_helper_checks.validate_work_package_fixed_leaf_use_site(
        "src/sieve/generic_unlink_helper.cpp",
        cross_file_raw_helper_source,
    )
    cross_file_raw_helper_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/generic_unlink_helper.cpp",
        cross_file_raw_helper_source,
    )
    cross_file_raw_helper_checks.validate_production_raw_unlinkat_authority(
        "src/sieve/generic_unlink_helper.cpp",
        cross_file_raw_helper_source,
    )
    expect(
        cross_file_raw_helper_checks.errors
        == [
            "src/sieve/generic_unlink_helper.cpp:3: "
            "production raw unlinkat authority is not allowlisted"
        ],
        "cross-file generic unlink helper escaped the global raw sink gate: "
        f"{cross_file_raw_helper_checks.errors}",
    )
    existing_derived_alias_caller_checks = Checks(Path("."))
    existing_derived_alias_caller = r"""
auto private_lease_unlink_at(int directory, const char* leaf, int flags) {
    return ::unlinkat(directory, leaf, flags);
}
auto read_private_lease_directory_inventory() {
    (void)erase_leaf(directory.get(), child.data());
    return inventory;
}
"""
    existing_derived_alias_caller_checks.validate_production_raw_unlinkat_authority(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        existing_derived_alias_caller,
    )
    expect(
        not existing_derived_alias_caller_checks.errors,
        "derived-alias caller should be closed by rejecting its raw helper TU: "
        f"{existing_derived_alias_caller_checks.errors}",
    )
    allowed_global_raw_unlinkat_snippets = {
        WORK_PACKAGE_RESIDUE_RECONCILER_IMPLEMENTATION_FILE: valid_carrier_unlink_authority,
        WORKER_LAUNCHER_IMPLEMENTATION_FILE: valid_wave_store_unlink_authority,
        "src/util/durable_immutable_record.cpp": r"""
auto remove_exact_at() noexcept override {
    return ::unlinkat(directory, leaf, 0);
}
""",
        "include/gnfs/relation/ooc_cleanup_transaction.hpp": r"""
auto remove_exact_private_handoff_pending() {
    return ::unlinkat(directory, leaf, 0);
}
""",
        "include/gnfs/relation/ooc_relation_store.hpp": r"""
auto remove_path_if_same_identity_at_noexcept() noexcept {
    return ::unlinkat(directory, leaf, 0);
}
""",
    }
    expect(
        set(allowed_global_raw_unlinkat_snippets)
        == set(PRODUCTION_RAW_UNLINKAT_FUNCTIONS),
        "global production raw unlinkat self-test inventory is incomplete",
    )
    for relative, snippet in allowed_global_raw_unlinkat_snippets.items():
        allowed_global_raw_unlinkat_checks = Checks(Path("."))
        allowed_global_raw_unlinkat_checks.validate_production_raw_unlinkat_authority(
            relative,
            snippet,
        )
        expect(
            not allowed_global_raw_unlinkat_checks.errors,
            f"allowlisted global raw unlinkat sink was rejected in {relative}: "
            f"{allowed_global_raw_unlinkat_checks.errors}",
        )
    allowed_file_raw_alias_checks = Checks(Path("."))
    allowed_file_raw_alias_checks.validate_production_raw_unlinkat_authority(
        "src/util/durable_immutable_record.cpp",
        allowed_global_raw_unlinkat_snippets["src/util/durable_immutable_record.cpp"]
        + "\nauto raw_unlink_alias = &::unlinkat;\n",
    )
    expect(
        len(allowed_file_raw_alias_checks.errors) == 1
        and "must own the sole direct production unlinkat call"
        in allowed_file_raw_alias_checks.errors[0],
        "allowlisted production file raw unlinkat alias escaped count closure: "
        f"{allowed_file_raw_alias_checks.errors}",
    )
    cross_file_fixed_leaf_caller_checks = Checks(Path("."))
    cross_file_fixed_leaf_caller = r"""
auto removed = erase_leaf(
    directory, DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1.data());
"""
    cross_file_fixed_leaf_caller_checks.validate_work_package_fixed_leaf_use_site(
        "src/sieve/untrusted_residue_cleanup.cpp",
        cross_file_fixed_leaf_caller,
    )
    cross_file_fixed_leaf_caller_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/untrusted_residue_cleanup.cpp",
        cross_file_fixed_leaf_caller,
    )
    expect(
        cross_file_fixed_leaf_caller_checks.errors
        == [
            "src/sieve/untrusted_residue_cleanup.cpp:3: "
            "fixed work-package leaf identifier is not allowlisted"
        ],
        "cross-file fixed-leaf helper caller escaped the use-site gate: "
        f"{cross_file_fixed_leaf_caller_checks.errors}",
    )

    untrusted_fixed_leaf_literal_checks = Checks(Path("."))
    untrusted_fixed_leaf_literal_checks.validate_work_package_fixed_leaf_use_site(
        "src/sieve/untrusted_residue_cleanup.cpp",
        'const auto leaf = ".gnfs-worker-work-package-v1";',
    )
    expect(
        len(untrusted_fixed_leaf_literal_checks.errors) == 1
        and "fixed work-package leaf literal is not allowlisted"
        in untrusted_fixed_leaf_literal_checks.errors[0],
        "untrusted fixed-leaf literal escaped the use-site gate: "
        f"{untrusted_fixed_leaf_literal_checks.errors}",
    )

    aliased_private_lease_unlink_checks = Checks(Path("."))
    aliased_private_lease_unlink_checks.validate_wave_store_private_lease_unlink_authority(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        valid_wave_store_unlink_authority.replace(
            "    (void)private_lease_unlink_at(parent, leaf, 0);\n",
            "    auto cleanup_alias = &private_lease_unlink_at;\n"
            "    (void)cleanup_alias(parent, leaf, 0);\n",
        ),
    )
    expect(
        len(aliased_private_lease_unlink_checks.errors) == 1
        and "must contain exactly 1 direct private_lease_unlink_at call"
        in aliased_private_lease_unlink_checks.errors[0],
        "WaveStore private-lease unlink address alias escaped the exact-body gate: "
        f"{aliased_private_lease_unlink_checks.errors}",
    )

    file_scope_private_lease_alias = (
        valid_wave_store_unlink_authority
        + r"""
auto cleanup_alias = &private_lease_unlink_at;
"""
        + valid_residue_reconciliation_composition.replace(
            "    return reconciled;\n",
            "    (void)cleanup_alias(directory, leaf, 0);\n" "    return reconciled;\n",
        )
    )
    file_scope_private_lease_alias_checks = Checks(Path("."))
    file_scope_private_lease_alias_checks.validate_work_package_residue_reconciliation_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        file_scope_private_lease_alias,
    )
    file_scope_private_lease_alias_checks.validate_wave_store_private_lease_unlink_authority(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        file_scope_private_lease_alias,
    )
    expect(
        file_scope_private_lease_alias_checks.errors
        == [
            f"{WORKER_LAUNCHER_IMPLEMENTATION_FILE}:1: all "
            f"{WAVE_STORE_PRIVATE_LEASE_UNLINK_HELPER} calls must remain inside "
            "the exact private-lease recovery bodies"
        ],
        "file-scope private-lease unlink alias escaped the WaveStore gate: "
        f"{file_scope_private_lease_alias_checks.errors}",
    )

    raw_unlink_checks = Checks(Path("."))
    raw_unlink_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/untrusted_residue_cleanup.cpp",
        "ops.unlink_at(directory);",
    )
    expect(
        len(raw_unlink_checks.errors) == 1
        and "raw fixed-leaf unlink seam is carrier-only" in raw_unlink_checks.errors[0],
        "raw fixed-leaf unlink seam escaped the carrier: "
        f"{raw_unlink_checks.errors}",
    )
    raw_unlinkat_checks = Checks(Path("."))
    raw_unlinkat_checks.validate_raw_work_package_fixed_leaf_unlink(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        "::unlinkat(directory, "
        "DISTRIBUTED_SIEVE_WORKER_WORK_PACKAGE_FILE_LEAF_V1.data(), 0);",
    )
    expect(
        len(raw_unlinkat_checks.errors) == 1
        and "raw fixed-leaf unlinkat bypass is carrier-only"
        in raw_unlinkat_checks.errors[0],
        "raw fixed-leaf unlinkat bypass escaped the carrier: "
        f"{raw_unlinkat_checks.errors}",
    )
    raw_literal_unlinkat_checks = Checks(Path("."))
    raw_literal_unlinkat_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/relation/relation_collector.cpp",
        '::unlinkat(directory, ".gnfs-worker-work-package-v1", 0);',
    )
    expect(
        len(raw_literal_unlinkat_checks.errors) == 1
        and "raw fixed-leaf unlinkat bypass is carrier-only"
        in raw_literal_unlinkat_checks.errors[0],
        "raw literal fixed-leaf unlinkat bypass escaped the carrier: "
        f"{raw_literal_unlinkat_checks.errors}",
    )
    allowed_raw_unlink_checks = Checks(Path("."))
    allowed_raw_unlink_checks.validate_raw_work_package_fixed_leaf_unlink(
        "src/sieve/distributed_sieve_worker_work_package_file.cpp",
        "ops.unlink_at(directory);",
    )
    expect(
        not allowed_raw_unlink_checks.errors,
        "carrier-owned raw fixed-leaf unlink was rejected: "
        f"{allowed_raw_unlink_checks.errors}",
    )

    fixed_capability_use_site_checks = Checks(Path("."))
    fixed_capability_use_site_checks.validate_worker_process_fixed_capability_use_site(
        "src/sieve/untrusted_launcher.cpp",
        r"""
DistributedSieveWorkerProcessFixedCapabilitySourcesV1 sources;
auto result = spawn_distributed_sieve_worker_process_batch_with_capabilities(
    request, sources);
""",
    )
    expect(
        len(fixed_capability_use_site_checks.errors) == 2
        and all(
            "fixed-capability worker-process API use site is not allowlisted" in error
            for error in fixed_capability_use_site_checks.errors
        ),
        "fixed-capability worker-process repo-wide use-site gate is not enforced",
    )
    allowed_fixed_capability_snippet = r"""
DistributedSieveWorkerProcessFixedCapabilitySourcesV1 sources;
auto result = spawn_distributed_sieve_worker_process_batch_with_capabilities(
    request, sources);
"""
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

    worker_entry_use_site_snippet = r"""
DistributedSieveWorkerEntryAdoptionResultV1 result =
    adopt_distributed_sieve_worker_entry_v1();
DistributedSieveWorkerEntryV1* entry = &*result.entry;
trusted_test::DistributedSieveWorkerEntryTestHooksV1 hooks;
auto hooked = adopt_distributed_sieve_worker_entry_v1_with_hooks(hooks);
"""
    worker_entry_use_site_checks = Checks(Path("."))
    worker_entry_use_site_checks.validate_worker_entry_use_site(
        "src/sieve/untrusted_worker_entry.cpp", worker_entry_use_site_snippet
    )
    expect(
        len(worker_entry_use_site_checks.errors)
        == len(WORKER_ENTRY_USE_SITE_IDENTIFIERS)
        and all(
            "single-use worker-entry capability use site is not allowlisted" in error
            for error in worker_entry_use_site_checks.errors
        ),
        "single-use worker-entry repo-wide use-site gate is not enforced",
    )
    for relative in sorted(WORKER_ENTRY_USE_SITE_ALLOWLIST):
        allowed_worker_entry_checks = Checks(Path("."))
        allowed_worker_entry_checks.validate_worker_entry_use_site(
            relative, worker_entry_use_site_snippet
        )
        expect(
            not allowed_worker_entry_checks.errors,
            f"allowlisted single-use worker-entry use was rejected in "
            f"{relative}: {allowed_worker_entry_checks.errors}",
        )
    expect(
        WORKER_ENTRY_USE_SITE_ALLOWLIST
        == {
            WORKER_ENTRY_IMPLEMENTATION_FILE,
            WORKER_ENTRY_INTERFACE_FILE,
            WORKER_ENTRY_TEST_FILE,
            WORKER_WRITER_IMPLEMENTATION_FILE,
            WORKER_WRITER_INTERFACE_FILE,
            WORKER_WRITER_TEST_FILE,
            WORKER_EXECUTOR_IMPLEMENTATION_FILE,
            WORKER_EXECUTOR_INTERFACE_FILE,
            WORKER_EXECUTOR_TEST_FILE,
            WORKER_COORDINATOR_TEST_FILE,
        },
        "worker-entry allowlist is not the exact entry/writer/executor "
        "implementation, interface, and dedicated entry plus coordinator tests",
    )

    worker_writer_use_site_snippet = r"""
DistributedSieveWorkerWriterAdoptionResultV1 result =
    consume_distributed_sieve_worker_writer_v1(std::move(entry));
DistributedSieveWorkerWriterAuthorityV1* writer = &*result.writer;
trusted_test::DistributedSieveWorkerWriterTestHooksV1 hooks;
trusted_test::DistributedSieveWorkerHandoffTestHooksV1 handoff_hooks;
DistributedSieveWorkerCompletionFactsV1 completion;
DistributedSieveWorkerWriterRollbackV1 rollback;
auto hooked = consume_distributed_sieve_worker_writer_v1_with_hooks(
    std::move(entry), hooks);
auto handoff = writer->finalize_and_publish_handoff(completion);
auto hooked_handoff =
    finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks(
        *writer, completion, handoff_hooks);
"""
    worker_writer_use_site_checks = Checks(Path("."))
    worker_writer_use_site_checks.validate_worker_writer_use_site(
        "src/sieve/untrusted_worker_writer.cpp", worker_writer_use_site_snippet
    )
    expect(
        len(worker_writer_use_site_checks.errors)
        == len(WORKER_WRITER_USE_SITE_IDENTIFIERS)
        and all(
            "single-use worker-writer capability use site is not allowlisted" in error
            for error in worker_writer_use_site_checks.errors
        ),
        "single-use worker-writer repo-wide use-site gate is not enforced",
    )
    for relative in sorted(WORKER_WRITER_USE_SITE_ALLOWLIST):
        allowed_worker_writer_checks = Checks(Path("."))
        allowed_worker_writer_checks.validate_worker_writer_use_site(
            relative, worker_writer_use_site_snippet
        )
        expect(
            not allowed_worker_writer_checks.errors,
            f"allowlisted single-use worker-writer use was rejected in "
            f"{relative}: {allowed_worker_writer_checks.errors}",
        )
    expect(
        WORKER_WRITER_USE_SITE_ALLOWLIST
        == {
            WORKER_ENTRY_IMPLEMENTATION_FILE,
            WORKER_ENTRY_INTERFACE_FILE,
            WORKER_WRITER_IMPLEMENTATION_FILE,
            WORKER_WRITER_INTERFACE_FILE,
            WORKER_WRITER_TEST_FILE,
            WORKER_EXECUTOR_IMPLEMENTATION_FILE,
            WORKER_EXECUTOR_INTERFACE_FILE,
            WORKER_EXECUTOR_TEST_FILE,
        },
        "worker-writer allowlist is not the exact entry conversion, writer, "
        "executor, and dedicated test boundary",
    )

    valid_executor_composition = r"""
DistributedSieveWorkerExecutionResultV1
execute_distributed_sieve_worker_entry_v1(Entry&& entry) noexcept {
    // run_distributed_sieve(), RelationCollector, cleanup(), and getenv()
    // in comments or strings are not code authority.
    const char* ignored = "hardware_concurrency finalize_and_publish_private_handoff";
    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1(entry.identity());
    auto prepared = prepare_distributed_sieve_worker_chunk_v1(
        runtime.polynomial, runtime.factor_base, runtime.bound_work, entry.chunk());
    auto adopted =
        consume_distributed_sieve_worker_writer_v1(std::move(entry));
    auto handoff = adopted.writer->finalize_and_publish_handoff(completion);
    return {handoff};
}
"""
    exact_executor_composition_checks = Checks(Path("."))
    exact_executor_composition_checks.validate_worker_executor_composition_body(
        WORKER_EXECUTOR_IMPLEMENTATION_FILE, valid_executor_composition
    )
    expect(
        WORKER_EXECUTOR_COMPOSITION_USE_COUNTS
        == {
            "rehydrate_distributed_sieve_worker_runtime_v1": 1,
            "prepare_distributed_sieve_worker_chunk_v1": 1,
            "consume_distributed_sieve_worker_writer_v1": 1,
            "finalize_and_publish_handoff": 1,
        }
        and WORKER_EXECUTOR_COMPOSITION_CALL_ORDER
        == tuple(WORKER_EXECUTOR_COMPOSITION_USE_COUNTS)
        and not exact_executor_composition_checks.errors,
        "exact worker-executor composition was rejected: "
        f"{exact_executor_composition_checks.errors}",
    )

    duplicate_executor_call = valid_executor_composition.replace(
        "    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1"
        "(entry.identity());\n",
        "    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1"
        "(entry.identity());\n"
        "    auto duplicate_runtime = rehydrate_distributed_sieve_worker_runtime_v1"
        "(entry.identity());\n",
    )
    duplicate_executor_call_checks = Checks(Path("."))
    duplicate_executor_call_checks.validate_worker_executor_composition_body(
        WORKER_EXECUTOR_IMPLEMENTATION_FILE, duplicate_executor_call
    )
    expect(
        len(duplicate_executor_call_checks.errors) == 1
        and "exactly 1 direct rehydrate_distributed_sieve_worker_runtime_v1 call"
        in duplicate_executor_call_checks.errors[0],
        "worker-executor one-shot runtime rehydration count is not closed: "
        f"{duplicate_executor_call_checks.errors}",
    )

    outside_executor_composition = (
        valid_executor_composition
        + "\nauto escaped_runtime = "
        "rehydrate_distributed_sieve_worker_runtime_v1(identity);\n"
    )
    outside_executor_composition_checks = Checks(Path("."))
    outside_executor_composition_checks.validate_worker_executor_composition_body(
        WORKER_EXECUTOR_IMPLEMENTATION_FILE, outside_executor_composition
    )
    expect(
        len(outside_executor_composition_checks.errors) == 1
        and "executor authority must remain inside "
        f"{WORKER_EXECUTOR_COMPOSITION_FUNCTION}"
        in outside_executor_composition_checks.errors[0],
        "same-file outside-function worker-executor authority escaped: "
        f"{outside_executor_composition_checks.errors}",
    )

    reordered_executor_composition = valid_executor_composition.replace(
        "    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1"
        "(entry.identity());\n"
        "    auto prepared = prepare_distributed_sieve_worker_chunk_v1(\n"
        "        runtime.polynomial, runtime.factor_base, runtime.bound_work, "
        "entry.chunk());\n",
        "    auto prepared = prepare_distributed_sieve_worker_chunk_v1(\n"
        "        runtime.polynomial, runtime.factor_base, runtime.bound_work, "
        "entry.chunk());\n"
        "    auto runtime = rehydrate_distributed_sieve_worker_runtime_v1"
        "(entry.identity());\n",
    )
    reordered_executor_composition_checks = Checks(Path("."))
    reordered_executor_composition_checks.validate_worker_executor_composition_body(
        WORKER_EXECUTOR_IMPLEMENTATION_FILE, reordered_executor_composition
    )
    expect(
        len(reordered_executor_composition_checks.errors) == 1
        and "must order runtime rehydration" in reordered_executor_composition_checks.errors[0],
        "worker-executor authority ordering is not closed: "
        f"{reordered_executor_composition_checks.errors}",
    )

    forbidden_executor_composition = valid_executor_composition.replace(
        "    return {handoff};\n",
        "    RelationCollector collector;\n"
        "    run_distributed_sieve_impl();\n"
        "    cleanup_worker_artifacts();\n"
        "    finalize_and_publish_handoff_impl();\n"
        "    finalize_and_publish_distributed_sieve_worker_handoff_v1_with_hooks();\n"
        "    finalize_and_publish_private_handoff();\n"
        "    finalize_and_publish_private_handoff_built();\n"
        "    auto getter = getenv;\n"
        "    auto host = hardware_concurrency();\n"
        "    return {handoff};\n",
    )
    forbidden_executor_composition_checks = Checks(Path("."))
    forbidden_executor_composition_checks.validate_worker_executor_composition_body(
        WORKER_EXECUTOR_IMPLEMENTATION_FILE, forbidden_executor_composition
    )
    expected_forbidden_executor_tokens = set(
        WORKER_EXECUTOR_COMPOSITION_FORBIDDEN_IDENTIFIERS
    ) | {
        "run_distributed_sieve_impl",
        "cleanup_worker_artifacts",
    }
    expect(
        len(forbidden_executor_composition_checks.errors)
        == len(expected_forbidden_executor_tokens)
        and all(
            any(token in error for error in forbidden_executor_composition_checks.errors)
            for token in expected_forbidden_executor_tokens
        ),
        "worker-executor legacy/cleanup/generic-publish/ambient bans are not closed: "
        f"{forbidden_executor_composition_checks.errors}",
    )

    relation_friend_snippet = r"""
namespace gnfs::sieve::distributed_sieve_worker_entry_detail {
class DistributedSieveWorkerWriterAuthorityV1;
}
class OOCRelationWriter {
    friend class ::gnfs::sieve::distributed_sieve_worker_entry_detail::
        DistributedSieveWorkerWriterAuthorityV1;
};
"""
    relation_friend_checks = Checks(Path("."))
    relation_friend_checks.validate_worker_writer_use_site(
        WORKER_WRITER_AUTHORITY_EXCEPTION_FILE, relation_friend_snippet
    )
    relation_friend_checks.validate_worker_writer_identifier_exception_boundary(
        WORKER_WRITER_AUTHORITY_EXCEPTION_FILE, relation_friend_snippet
    )
    expect(
        not relation_friend_checks.errors,
        "relation-store authority friend exception was rejected",
    )
    relation_friend_overreach_checks = Checks(Path("."))
    relation_friend_overreach_checks.validate_worker_writer_use_site(
        "include/gnfs/relation/ooc_relation_store.hpp",
        "DistributedSieveWorkerCompletionFactsV1 completion;",
    )
    expect(
        len(relation_friend_overreach_checks.errors) == 1
        and "single-use worker-writer capability use site is not allowlisted"
        in relation_friend_overreach_checks.errors[0],
        "relation-store authority friend exception is broader than one identifier",
    )
    relation_friend_same_identifier_overreach_checks = Checks(Path("."))
    relation_friend_same_identifier_overreach_checks.validate_worker_writer_use_site(
        WORKER_WRITER_AUTHORITY_EXCEPTION_FILE,
        relation_friend_snippet
        + "\nvoid escape(DistributedSieveWorkerWriterAuthorityV1* writer);\n",
    )
    relation_friend_same_identifier_overreach_checks.validate_worker_writer_identifier_exception_boundary(
        WORKER_WRITER_AUTHORITY_EXCEPTION_FILE,
        relation_friend_snippet
        + "\nvoid escape(DistributedSieveWorkerWriterAuthorityV1* writer);\n",
    )
    expect(
        len(relation_friend_same_identifier_overreach_checks.errors) == 1
        and "must be exactly one forward declaration and one qualified friend declaration"
        in relation_friend_same_identifier_overreach_checks.errors[0],
        "relation-store authority exception permits a third same-identifier use",
    )
    expect(
        WORKER_WRITER_IDENTIFIER_EXCEPTIONS
        == {
            WORKER_WRITER_AUTHORITY_IDENTIFIER: {
                WORKER_WRITER_AUTHORITY_EXCEPTION_FILE,
            },
        },
        "worker-writer identifier exception is not the exact relation-store friend boundary",
    )

    worker_writer_bridge_snippet = r"""
DistributedSieveWorkerWriterLifetimeGuardV1* guard = nullptr;
OOCInheritedP8WriterMintV1* mint = nullptr;
OOCExactFreshConstructionFailure* failure = nullptr;
OOCExactFreshRollbackDisposition rollback;
AdoptInheritedOpenFileDescription adopted;
ExactPrivateDirectoryBinding binding;
ExactPrivateDirectoryConstructionToken token;
discard_and_close_post_fork_child_noexcept();
discard_inherited_post_fork_child_noexcept();
"""
    worker_writer_bridge_checks = Checks(Path("."))
    worker_writer_bridge_checks.validate_worker_writer_use_site(
        "src/relation/untrusted_worker_writer_bridge.cpp",
        worker_writer_bridge_snippet,
    )
    expect(
        len(worker_writer_bridge_checks.errors)
        == len(WORKER_WRITER_BRIDGE_IDENTIFIERS)
        and all(
            "worker-writer private bridge use site is not allowlisted" in error
            for error in worker_writer_bridge_checks.errors
        ),
        "worker-writer private bridge repo-wide use-site gate is not enforced",
    )
    for relative in sorted(WORKER_WRITER_BRIDGE_ALLOWLIST):
        allowed_worker_writer_bridge_checks = Checks(Path("."))
        allowed_worker_writer_bridge_checks.validate_worker_writer_use_site(
            relative, worker_writer_bridge_snippet
        )
        expect(
            not allowed_worker_writer_bridge_checks.errors,
            f"allowlisted worker-writer private bridge use was rejected in "
            f"{relative}: {allowed_worker_writer_bridge_checks.errors}",
        )
    expect(
        WORKER_WRITER_BRIDGE_ALLOWLIST
        == {
            "include/gnfs/relation/ooc_cleanup_transaction.hpp",
            "include/gnfs/relation/ooc_relation_store.hpp",
            "include/gnfs/util/native_binary_update_file.hpp",
            WORKER_ENTRY_IMPLEMENTATION_FILE,
            WORKER_WRITER_IMPLEMENTATION_FILE,
            WORKER_WRITER_INTERFACE_FILE,
        },
        "worker-writer private bridge allowlist is not exact",
    )
    expect(
        WORKER_WRITER_BRIDGE_IDENTIFIER_EXCEPTIONS
        == {
            "OOCExactFreshConstructionFailure": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "AdoptInheritedOpenFileDescription": {
                "src/relation/ooc_private_handoff_adoption.cpp",
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "ExactPrivateDirectoryBinding": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "ExactPrivateDirectoryConstructionToken": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "discard_inherited_post_fork_child_noexcept": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
        },
        "worker-writer private bridge identifier exception is not exact",
    )
    inherited_lock_exception_checks = Checks(Path("."))
    inherited_lock_exception_checks.validate_worker_writer_use_site(
        "src/relation/ooc_private_handoff_adoption.cpp",
        "AdoptInheritedOpenFileDescription adopted;\n",
    )
    expect(
        not inherited_lock_exception_checks.errors,
        "private-handoff adoption inherited-lock exception was rejected: "
        f"{inherited_lock_exception_checks.errors}",
    )
    inherited_lock_exception_overreach_checks = Checks(Path("."))
    inherited_lock_exception_overreach_checks.validate_worker_writer_use_site(
        "src/relation/ooc_private_handoff_adoption.cpp",
        "AdoptInheritedOpenFileDescription adopted;\n"
        "DistributedSieveWorkerWriterLifetimeGuardV1* guard = nullptr;\n",
    )
    expect(
        len(inherited_lock_exception_overreach_checks.errors) == 1
        and "DistributedSieveWorkerWriterLifetimeGuardV1"
        in inherited_lock_exception_overreach_checks.errors[0],
        "private-handoff adoption inherited-lock exception admitted another "
        "writer bridge: "
        f"{inherited_lock_exception_overreach_checks.errors}",
    )
    for identifier, exception_files in sorted(
        WORKER_WRITER_BRIDGE_IDENTIFIER_EXCEPTIONS.items()
    ):
        for relative in sorted(exception_files):
            allowed_bridge_exception_checks = Checks(Path("."))
            allowed_bridge_exception_checks.validate_worker_writer_use_site(
                relative, f"{identifier} privileged_use;\n"
            )
            expect(
                not allowed_bridge_exception_checks.errors,
                f"allowlisted worker-writer bridge exception was rejected for "
                f"{identifier} in {relative}: "
                f"{allowed_bridge_exception_checks.errors}",
            )

    exact_append_batch_checks = Checks(Path("."))
    exact_append_batch_checks.validate_worker_writer_use_site(
        "src/sieve/untrusted_merge_writer_batch.cpp",
        "writer.begin_exact_append_batch();\n",
    )
    expect(
        len(exact_append_batch_checks.errors) == 1
        and "merge-writer exact append batch use is not allowlisted"
        in exact_append_batch_checks.errors[0],
        "merge-writer exact append batch repo-wide use-site gate is not enforced",
    )
    for relative in sorted(MERGE_WRITER_EXACT_APPEND_BATCH_ALLOWLIST):
        allowed_exact_append_batch_checks = Checks(Path("."))
        allowed_exact_append_batch_checks.validate_worker_writer_use_site(
            relative, "writer.begin_exact_append_batch();\n"
        )
        expect(
            not allowed_exact_append_batch_checks.errors,
            f"allowlisted merge-writer exact append batch use was rejected in "
            f"{relative}: {allowed_exact_append_batch_checks.errors}",
        )
    expect(
        MERGE_WRITER_EXACT_APPEND_BATCH_ALLOWLIST
        == {
            "include/gnfs/relation/ooc_relation_store.hpp",
            MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        },
        "merge-writer exact append batch allowlist is not exact",
    )

    merge_writer_test_hook_snippet = "\n".join(
        f"{identifier} hook_use;"
        for identifier in MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS
    )
    untrusted_merge_writer_test_hook_checks = Checks(Path("."))
    untrusted_merge_writer_test_hook_checks.validate_worker_writer_use_site(
        "src/sieve/untrusted_merge_writer_test_hook.cpp",
        merge_writer_test_hook_snippet,
    )
    expect(
        len(untrusted_merge_writer_test_hook_checks.errors)
        == len(MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS)
        and all(
            "merge-writer trusted-test hook use is not allowlisted" in error
            for error in untrusted_merge_writer_test_hook_checks.errors
        ),
        "merge-writer trusted-test hook repo-wide use-site gate is not enforced",
    )
    for identifier, allowlist in sorted(
        MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS.items()
    ):
        for relative in sorted(allowlist):
            allowed_merge_writer_test_hook_checks = Checks(Path("."))
            allowed_merge_writer_test_hook_checks.validate_worker_writer_use_site(
                relative, f"{identifier} hook_use;\n"
            )
            expect(
                not allowed_merge_writer_test_hook_checks.errors,
                f"allowlisted merge-writer trusted-test hook was rejected for "
                f"{identifier} in {relative}: "
                f"{allowed_merge_writer_test_hook_checks.errors}",
            )
    expect(
        MERGE_WRITER_TEST_HOOK_IDENTIFIER_ALLOWLISTS
        == {
            "DistributedSieveMergeWriterTestHooksV1": {
                MERGE_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
            },
            "stream_distributed_sieve_merge_inputs_v1_with_hooks": {
                MERGE_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "after_output_write": {
                MERGE_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_TEST_FILE,
            },
            "DistributedSieveMergeWriterAdoptionTestHooksV1": {
                MERGE_COORDINATOR_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_TEST_FILE,
            },
            "consume_distributed_sieve_merge_generation_v1_with_hooks": {
                MERGE_COORDINATOR_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_TEST_FILE,
            },
            "DistributedSieveMergePreparedPublicationTestHooksV1": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_TEST_FILE,
            },
            "publish_distributed_sieve_merge_prepared_v1_with_hooks": {
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
                MERGE_WRITER_AUTHORITY_TEST_FILE,
            },
        },
        "merge-writer trusted-test hook identifier allowlists are not exact",
    )
    expect(
        BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_ALLOWLISTS
        == {
            "OOCPrivateHandoffBorrowedBaseLockV1": {
                "src/relation/ooc_private_handoff_adoption.cpp",
                "src/relation/ooc_private_handoff_adoption_internal.hpp",
                "src/sieve/distributed_sieve_wave_store.cpp",
                "include/gnfs/relation/ooc_cleanup_transaction.hpp",
            },
            "adopt_private_handoff_with_borrowed_base_lock_v1": {
                "src/relation/ooc_private_handoff_adoption.cpp",
                "src/relation/ooc_private_handoff_adoption_internal.hpp",
                "src/sieve/distributed_sieve_wave_store.cpp",
            },
            "OOCPrivateHandoffAdoptionBuilderV1": {
                "src/relation/ooc_private_handoff_adoption.cpp",
                "include/gnfs/relation/ooc_cleanup_transaction.hpp",
            },
        },
        "borrowed BaseLock bridge identifier allowlists are not exact",
    )
    expect(
        BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_USE_COUNTS
        == {
            "OOCPrivateHandoffBorrowedBaseLockV1": {
                BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 7,
                BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE: 14,
                BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE: 1,
                BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER: 2,
            },
            "adopt_private_handoff_with_borrowed_base_lock_v1": {
                BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 1,
                BORROWED_BASE_LOCK_BRIDGE_INTERFACE_FILE: 2,
                BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE: 1,
            },
            "OOCPrivateHandoffAdoptionBuilderV1": {
                BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE: 3,
                BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER: 3,
            },
        },
        "borrowed BaseLock bridge identifier use counts are not exact",
    )
    untrusted_borrowed_base_lock_bridge = r"""
OOCPrivateHandoffBorrowedBaseLockV1* borrowed = nullptr;
adopt_private_handoff_with_borrowed_base_lock_v1();
OOCPrivateHandoffAdoptionBuilderV1* builder = nullptr;
"""
    untrusted_borrowed_base_lock_checks = Checks(Path("."))
    untrusted_borrowed_base_lock_checks.validate_borrowed_base_lock_bridge(
        "src/relation/untrusted_borrowed_base_lock.cpp",
        untrusted_borrowed_base_lock_bridge,
    )
    expect(
        len(untrusted_borrowed_base_lock_checks.errors)
        == len(BORROWED_BASE_LOCK_BRIDGE_IDENTIFIER_ALLOWLISTS)
        and all(
            "borrowed BaseLock bridge identifier count is not closed" in error
            for error in untrusted_borrowed_base_lock_checks.errors
        ),
        "borrowed BaseLock bridge repo-wide use-site gate is not enforced: "
        f"{untrusted_borrowed_base_lock_checks.errors}",
    )

    valid_borrowed_base_lock_bridge = r"""
class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1 {};
void ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1::invoke() {}
void ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1::make_receipt() {}

OOCPrivateHandoffBorrowedBaseLockV1::OOCPrivateHandoffBorrowedBaseLockV1(
    int, int, std::string_view, std::array<std::uint64_t, 3>,
    std::uint64_t) noexcept {}
OOCPrivateHandoffBorrowedBaseLockV1::OOCPrivateHandoffBorrowedBaseLockV1(
    OOCPrivateHandoffBorrowedBaseLockV1&& other) noexcept {}

std::shared_ptr<BaseLock> OOCPrivateHandoffBorrowedBaseLockV1::consume(
    const OOCCleanupPaths& paths, AdoptionParentDirectoryHandle& parent) {
#if !defined(__APPLE__)
    fail();
#else
    std::string retained_leaf(lock_leaf_);
    int duplicated = -1;
    do {
        duplicated = ::fcntl(lock_descriptor_, F_DUPFD_CLOEXEC, 0);
    } while (duplicated < 0 && errno == EINTR);
    if (duplicated < 0) {
        fail(OOCCleanupStatus::IoFailure, OOCCleanupStage::None,
             posix_error(errno));
    }
    try {
        auto adopted = std::unique_ptr<BaseLock>(new BaseLock(
            paths.lock_path, duplicated,
            static_cast<int>(parent.native_handle()),
            std::move(retained_leaf), held_parent_identity, lock_identity_,
            BaseLock::AdoptInheritedOpenFileDescription{}));
        duplicated = -1;
        return std::shared_ptr<BaseLock>(std::move(adopted));
    } catch (...) {
        if (duplicated >= 0) {
            (void)::close(duplicated);
        }
        throw;
    }
#endif
}

OOCPrivateHandoffAdoptionResult
adopt_private_handoff_with_borrowed_base_lock_v1(
    const std::filesystem::path& base_path,
    OOCPrivateHandoffBorrowedBaseLockV1&& borrowed,
    OOCPrivateHandoffAdoptionTestHooks hooks) noexcept {
    return adopt_private_handoff_impl(
        base_path, hooks, true,
        [&](const OOCCleanupPaths& paths,
            AdoptionParentDirectoryHandle& parent) {
            return borrowed.consume(paths, parent);
        },
        nullptr, nullptr);
}
"""
    exact_borrowed_base_lock_checks = Checks(Path("."))
    exact_borrowed_base_lock_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        valid_borrowed_base_lock_bridge,
    )
    expect(
        not exact_borrowed_base_lock_checks.errors,
        "exact borrowed BaseLock bridge was rejected: "
        f"{exact_borrowed_base_lock_checks.errors}",
    )
    independent_borrowed_lock_duplicate = valid_borrowed_base_lock_bridge.replace(
        "F_DUPFD_CLOEXEC", "F_DUPFD"
    )
    independent_borrowed_lock_checks = Checks(Path("."))
    independent_borrowed_lock_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        independent_borrowed_lock_duplicate,
    )
    expect(
        any(
            "must duplicate exactly one close-on-exec descriptor" in error
            for error in independent_borrowed_lock_checks.errors
        ),
        "borrowed BaseLock bridge accepted a non-CLOEXEC duplicate: "
        f"{independent_borrowed_lock_checks.errors}",
    )
    unlocking_borrowed_base_lock = valid_borrowed_base_lock_bridge.replace(
        "        duplicated = -1;\n",
        "        (void)::flock(duplicated, LOCK_UN);\n"
        "        duplicated = -1;\n",
    )
    unlocking_borrowed_base_lock_checks = Checks(Path("."))
    unlocking_borrowed_base_lock_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        unlocking_borrowed_base_lock,
    )
    expect(
        any(
            "without flock or LOCK_UN" in error
            for error in unlocking_borrowed_base_lock_checks.errors
        ),
        "borrowed BaseLock bridge accepted explicit unlock authority: "
        f"{unlocking_borrowed_base_lock_checks.errors}",
    )
    unbound_borrowed_base_lock_consumption = valid_borrowed_base_lock_bridge.replace(
        "borrowed.consume(paths, parent)",
        "unbound.consume(paths, parent)",
    )
    unbound_borrowed_base_lock_consumption_checks = Checks(Path("."))
    unbound_borrowed_base_lock_consumption_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        unbound_borrowed_base_lock_consumption,
    )
    expect(
        any(
            "must consume the exact one-shot token once" in error
            for error in unbound_borrowed_base_lock_consumption_checks.errors
        ),
        "borrowed BaseLock adoption accepted an unbound token: "
        f"{unbound_borrowed_base_lock_consumption_checks.errors}",
    )
    dead_borrowed_base_lock_construction = valid_borrowed_base_lock_bridge.replace(
        "    std::string retained_leaf(lock_leaf_);\n",
        "    if (false) {\n"
        "    std::string retained_leaf(lock_leaf_);\n",
    ).replace(
        "    } catch (...) {\n"
        "        if (duplicated >= 0) {\n"
        "            (void)::close(duplicated);\n"
        "        }\n"
        "        throw;\n"
        "    }\n"
        "#endif\n",
        "    } catch (...) {\n"
        "        if (duplicated >= 0) {\n"
        "            (void)::close(duplicated);\n"
        "        }\n"
        "        throw;\n"
        "    }\n"
        "    }\n"
        "    return {};\n"
        "#endif\n",
        1,
    )
    dead_borrowed_base_lock_construction_checks = Checks(Path("."))
    dead_borrowed_base_lock_construction_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_IMPLEMENTATION_FILE,
        dead_borrowed_base_lock_construction,
    )
    expect(
        any(
            "must duplicate exactly one close-on-exec descriptor" in error
            for error in dead_borrowed_base_lock_construction_checks.errors
        ),
        "borrowed BaseLock bridge accepted a dead duplicated-fd construction "
        "plus live unrelated return: "
        f"{dead_borrowed_base_lock_construction_checks.errors}",
    )

    valid_borrowed_base_lock_cleanup_header = r"""
class OOCPrivateHandoffBorrowedBaseLockV1;
namespace ooc_cleanup_detail {
class OOCPrivateHandoffAdoptionBuilderV1;
}
class BaseLock {
private:
    struct AdoptInheritedOpenFileDescription final {};
    BaseLock(int descriptor,
             const std::array<std::uint64_t, 3>& expected_lock_identity,
             AdoptInheritedOpenFileDescription) {
#ifndef _WIN32
        int retained_result = -1;
        do {
            retained_result = ::flock(descriptor, LOCK_EX | LOCK_NB);
        } while (retained_result != 0 && errno == EINTR);
        if (retained_result != 0) {
            fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None,
                 posix_error(errno));
        }
        identity_ = expected_lock_identity;
        descriptor_ = descriptor;
#endif
    }
    friend class OOCPrivateHandoffBorrowedBaseLockV1;
    friend class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1;
    void release_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }
};
class OOCPrivateHandoffAdoptionReceipt {
    friend class ooc_cleanup_detail::OOCPrivateHandoffAdoptionBuilderV1;
};
"""
    exact_borrowed_cleanup_checks = Checks(Path("."))
    exact_borrowed_cleanup_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER,
        valid_borrowed_base_lock_cleanup_header,
    )
    expect(
        not exact_borrowed_cleanup_checks.errors,
        "exact close-only inherited BaseLock boundary was rejected: "
        f"{exact_borrowed_cleanup_checks.errors}",
    )
    unlocking_borrowed_cleanup_header = (
        valid_borrowed_base_lock_cleanup_header.replace(
            "            (void)::close(descriptor_);\n",
            "            (void)::flock(descriptor_, LOCK_UN);\n"
            "            (void)::close(descriptor_);\n",
        )
    )
    unlocking_borrowed_cleanup_checks = Checks(Path("."))
    unlocking_borrowed_cleanup_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER,
        unlocking_borrowed_cleanup_header,
    )
    expect(
        any(
            "retain an exact close-only destructor" in error
            for error in unlocking_borrowed_cleanup_checks.errors
        ),
        "borrowed BaseLock cleanup boundary accepted LOCK_UN: "
        f"{unlocking_borrowed_cleanup_checks.errors}",
    )
    expanded_borrowed_cleanup_header = (
        valid_borrowed_base_lock_cleanup_header
        + "\nOOCPrivateHandoffBorrowedBaseLockV1* escaped = nullptr;\n"
    )
    expanded_borrowed_cleanup_checks = Checks(Path("."))
    expanded_borrowed_cleanup_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_CLEANUP_HEADER,
        expanded_borrowed_cleanup_header,
    )
    expect(
        any(
            "identifier count is not closed" in error
            or "only as one forward declaration and one private friend" in error
            for error in expanded_borrowed_cleanup_checks.errors
        ),
        "public cleanup header accepted an extra borrowed BaseLock token use: "
        f"{expanded_borrowed_cleanup_checks.errors}",
    )

    valid_borrowed_wave_mint = r"""
auto DistributedSievePrivateLeaseBaseLockAt::adopt_exact_private_handoff(
    const std::filesystem::path& base_path) const {
    const bool owned = owned_by_current_process();
    return private_lease::adopt_private_handoff_with_borrowed_base_lock_v1(
        base_path,
        private_lease::OOCPrivateHandoffBorrowedBaseLockV1(
            owned ? root_fd_ : -1, owned ? lock_fd_ : -1, leaf_,
            relation_identity(identity_),
            owned ? creator_process_id_ : 0));
}
"""
    exact_borrowed_wave_mint_checks = Checks(Path("."))
    exact_borrowed_wave_mint_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE,
        valid_borrowed_wave_mint,
    )
    expect(
        not exact_borrowed_wave_mint_checks.errors,
        "exact WaveStore borrowed BaseLock mint was rejected: "
        f"{exact_borrowed_wave_mint_checks.errors}",
    )
    expanded_borrowed_wave_mint = (
        valid_borrowed_wave_mint
        + "\nvoid escaped() { "
        "private_lease::OOCPrivateHandoffBorrowedBaseLockV1* token; }\n"
    )
    expanded_borrowed_wave_mint_checks = Checks(Path("."))
    expanded_borrowed_wave_mint_checks.validate_borrowed_base_lock_bridge(
        BORROWED_BASE_LOCK_BRIDGE_WAVE_STORE_FILE,
        expanded_borrowed_wave_mint,
    )
    expect(
        any(
            "identifier count is not closed" in error
            for error in expanded_borrowed_wave_mint_checks.errors
        ),
        "WaveStore accepted a second borrowed BaseLock mint authority use: "
        f"{expanded_borrowed_wave_mint_checks.errors}",
    )

    consumed_canonical_sources = {
        relative: (
            Path(__file__).resolve().parents[1] / relative
        ).read_text(encoding="utf-8")
        for relative in {
            file
            for expected_by_file in (
                CONSUMED_CANONICAL_ADOPTION_IDENTIFIER_USE_COUNTS.values()
            )
            for file in expected_by_file
        }
    }
    exact_consumed_canonical_checks = Checks(Path("."))
    for relative, source in consumed_canonical_sources.items():
        exact_consumed_canonical_checks.validate_consumed_canonical_adoption_bridge(
            relative, source
        )
    expect(
        not exact_consumed_canonical_checks.errors,
        "exact consumed-canonical relation-only bridge was rejected: "
        f"{exact_consumed_canonical_checks.errors}",
    )

    def expect_consumed_canonical_mutation(
        relative: str,
        old: str,
        new: str,
        expected_error: str,
        description: str,
    ) -> None:
        source = consumed_canonical_sources[relative]
        expect(old in source, f"self-test mutation anchor is missing: {description}")
        mutated = source.replace(old, new, 1)
        mutation_checks = Checks(Path("."))
        mutation_checks.validate_consumed_canonical_adoption_bridge(
            relative, mutated
        )
        expect(
            any(expected_error in error for error in mutation_checks.errors),
            f"consumed-canonical gate accepted {description}: "
            f"{mutation_checks.errors}",
        )

    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_INTERFACE_FILE,
        "PrivateHandoffPublicationValidatedPermitV1&& permit,",
        "PrivateHandoffPublicationValidatedPermitV1& permit,",
        "exact rvalue-only source-private declaration",
        "an lvalue entry signature",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "State::Phase::ConsumedCanonical ||",
        "State::Phase::ConsumedNonTerminal ||",
        "must accept ConsumedCanonical phase only",
        "a nonterminal consumed phase",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "    auto state = std::move(permit.state_);\n",
        "    auto moved_hooks = std::move(hooks);\n"
        "    auto state = std::move(permit.state_);\n",
        "first capability move",
        "an earlier capability move",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "std::shared_ptr<BaseLock>(owner, owner->lock.get())",
        "std::shared_ptr<BaseLock>(std::move(owner->lock))",
        "aliasing BaseLock",
        "moving BaseLock out of State instead of aliasing it",
    )
    for forbidden_statement, expected_error in (
        (
            "auto replacement = std::make_shared<BaseLock>(path);",
            "must not construct a replacement BaseLock",
        ),
        (
            "(void)::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);",
            "must not duplicate, open, or relock BaseLock authority",
        ),
        (
            "(void)::flock(descriptor, LOCK_EX | LOCK_NB);",
            "must not duplicate, open, or relock BaseLock authority",
        ),
        (
            "(void)::open(path, flags);",
            "must not duplicate, open, or relock BaseLock authority",
        ),
        (
            "adopt_private_handoff_with_borrowed_base_lock_v1(path, token);",
            "must not escape through borrowed or path adoption",
        ),
        (
            "OOCCleanupTransaction::adopt_private_handoff(path);",
            "must not escape through borrowed or path adoption",
        ),
    ):
        expect_consumed_canonical_mutation(
            CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
            "    auto state = std::move(permit.state_);\n",
            "    auto state = std::move(permit.state_);\n"
            f"    {forbidden_statement}\n",
            expected_error,
            f"forbidden bridge statement {forbidden_statement}",
        )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "        if (!state->canonical_terminal ||\n",
        "        release_private_cleanup_action(*state->lock);\n"
        "        if (!state->canonical_terminal ||\n",
        "must not release the retained action claim",
        "early action-claim release",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE,
        "    std::shared_ptr<BaseLock> live_lock_;",
        "    std::unique_ptr<BaseLock> live_lock_;",
        "exact private one-shot alias token",
        "a non-alias token lock owner",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE,
        "        if (expected_terminal != nullptr) {\n"
        "            require_publication_terminal_match(*classified.witness, *expected_terminal, *parent,\n"
        "                                               *directory, *lock);\n"
        "        }\n",
        "",
        "immediately after initial canonical classification",
        "a missing initial terminal match",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE,
        "            if (expected_terminal != nullptr) {\n"
        "                require_publication_terminal_match(*current.witness, *expected_terminal, *parent,\n"
        "                                                   *directory, *lock);\n"
        "            }\n",
        "",
        "receipt revalidation closure",
        "a missing receipt-time terminal match",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_IMPLEMENTATION_FILE,
        "            revalidate_before_receipt();\n"
        "        }\n"
        "        const auto pending_handoff_snapshot =",
        "        }\n"
        "        const auto pending_handoff_snapshot =",
        "second terminal-aware revalidation",
        "receipt preparation before the second revalidation",
    )

    public_consumed_canonical_checks = Checks(Path("."))
    public_consumed_canonical_checks.validate_consumed_canonical_adoption_bridge(
        "include/gnfs/relation/escaped_consumed_authority.hpp",
        "class OOCPrivateHandoffConsumedPublicationBaseLockV1;\n",
    )
    expect(
        any(
            "must not leak into public headers" in error
            for error in public_consumed_canonical_checks.errors
        ),
        "public header accepted consumed-canonical adoption authority: "
        f"{public_consumed_canonical_checks.errors}",
    )

    untrusted_consumed_canonical_checks = Checks(Path("."))
    untrusted_consumed_canonical_checks.validate_consumed_canonical_adoption_bridge(
        "src/relation/untrusted_consumed_canonical.cpp",
        "adopt_consumed_canonical_private_handoff_publication_v1("
        "std::move(permit));\n",
    )
    expect(
        any(
            "identifier count is not closed" in error
            for error in untrusted_consumed_canonical_checks.errors
        ),
        "unallowlisted consumed-canonical entry call was accepted: "
        f"{untrusted_consumed_canonical_checks.errors}",
    )

    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "    auto owner = permit.state_;\n",
        "    auto owner = std::move(permit.state_);\n",
        "retain the lvalue permit until the final commit",
        "transactional reader adoption consuming its lvalue permit early",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "    auto retained_revalidator = std::move(revalidator);\n",
        "    auto retained_revalidator = std::move(revalidator);\n"
        "    auto replacement = std::make_shared<BaseLock>(path);\n",
        "must not construct a replacement BaseLock",
        "transactional reader adoption constructing a replacement BaseLock",
    )
    expect_consumed_canonical_mutation(
        CONSUMED_CANONICAL_ADOPTION_RELATION_IMPLEMENTATION_FILE,
        "        require_terminal();\n"
        "        if (!reader->valid()) {\n"
        "            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());\n"
        "        }\n\n"
        "        permit.state_.reset();\n",
        "        permit.state_.reset();\n"
        "        require_terminal();\n"
        "        if (!reader->valid()) {\n"
        "            fail(OOCCleanupStatus::UnexpectedFailure, OOCCleanupStage::None, protocol_error());\n"
        "        }\n\n",
        "may reset the permit only after both terminal checks",
        "transactional reader adoption resetting before its second terminal check",
    )

    merge_prepared_admission_sources = {
        relative: (Path(__file__).resolve().parents[1] / relative).read_text(
            encoding="utf-8"
        )
        for relative in {
            MERGE_PREPARED_ADMISSION_INTERFACE_FILE,
            MERGE_WRITER_AUTHORITY_INTERFACE_FILE,
            MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE,
            MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        }
    }
    exact_merge_prepared_admission_checks = Checks(Path("."))
    for relative, source in merge_prepared_admission_sources.items():
        exact_merge_prepared_admission_checks.validate_merge_prepared_admission_boundary(
            relative, source
        )
    expect(
        not exact_merge_prepared_admission_checks.errors,
        "exact common MergePrepared admission milestone was rejected: "
        f"{exact_merge_prepared_admission_checks.errors}",
    )

    def expect_merge_prepared_admission_mutation(
        relative: str,
        old: str,
        new: str,
        expected_error: str,
        description: str,
    ) -> None:
        source = merge_prepared_admission_sources[relative]
        expect(old in source, f"self-test mutation anchor is missing: {description}")
        mutated = source.replace(old, new, 1)
        mutation_checks = Checks(Path("."))
        mutation_checks.validate_merge_prepared_admission_boundary(relative, mutated)
        expect(
            any(expected_error in error for error in mutation_checks.errors),
            f"common MergePrepared admission gate accepted {description}: "
            f"{mutation_checks.errors}",
        )

    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_INTERFACE_FILE,
        "    friend class DistributedSieveMergeWriterAuthorityV1;\n",
        "    friend class ForgedMergePreparedAdmissionAuthorityV1;\n",
        "exactly the fresh/WaveStore mint friends",
        "a third-party replacement for the fresh mint friend",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
        "        std::shared_ptr<const void> lifetime_anchor(std::move(state));\n",
        "        std::shared_ptr<const void> lifetime_anchor(state.get());\n",
        "moving the unique writer State into a shared lifetime anchor",
        "a non-owning fresh lifetime anchor",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_INTERFACE_FILE,
        "               store_ready != prepared_ready;\n",
        "               store_ready || prepared_ready;\n",
        "store/prepared-admission XOR",
        "a non-XOR successful OpenResult",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        "        auto coordinator = store->claim_worker_coordinator_v1();\n",
        "        auto coordinator = store->peek_worker_coordinator_v1();\n",
        "claim the coordinator before its final classifier",
        "terminal cold-open without coordinator claim",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        "        return {nullptr, std::move(prepared_result), std::move(published.diagnostic)};\n",
        "        return {std::move(store), std::move(prepared_result),\n"
        "                std::move(published.diagnostic)};\n",
        "terminal success must return admission-only",
        "terminal cold-open returning both store and admission",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        "    context->diagnostic =\n"
        "        revalidate_recovered_merge_prepared_projection(*context->store, *context->expected, held);\n",
        "    context->diagnostic = diagnostic(DistributedSieveWaveStoreStatus::ready);\n",
        "false immediately runs the production authority/projection revalidation",
        "a false test hook bypassing production projection revalidation",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        "        context->diagnostic = diagnostic(DistributedSieveWaveStoreStatus::interrupted,\n",
        "        context->diagnostic = diagnostic(DistributedSieveWaveStoreStatus::ready,\n",
        "true injects interrupted",
        "a true test hook reporting success",
    )
    expect_merge_prepared_admission_mutation(
        MERGE_PREPARED_ADMISSION_WAVE_STORE_FILE,
        "                    DistributedSieveRecoveredPreparedPublicationSubjectV1::Worker,\n"
        "                    worker.manifest_slot);\n",
        "                    DistributedSieveRecoveredPreparedPublicationSubjectV1::Worker,\n"
        "                    index);\n",
        "Worker/real manifest_slot",
        "a worker callback using vector order instead of manifest_slot",
    )

    escaped_recovered_seam_checks = Checks(Path("."))
    escaped_recovered_seam_checks.validate_merge_prepared_admission_boundary(
        "src/sieve/untrusted_recovered_hook.cpp",
        "hooks.stop_before_recovered_aggregate_revalidation = callback;\n",
    )
    expect(
        any(
            "source-private to WaveStore and its one test" in error
            for error in escaped_recovered_seam_checks.errors
        ),
        "recovered MergePrepared test seam escaped its source-private allowlist: "
        f"{escaped_recovered_seam_checks.errors}",
    )

    worker_handoff_bridge_snippet = r"""
OOCFinalizedCorpusEvidenceV1 evidence;
OOCPrivateHandoffPayloadV1 payload;
OOCPrivateHandoffPayloadBuilderV1 builder = nullptr;
capture_finalized_corpus_evidence(descriptor);
finalize_and_publish_private_handoff_built(builder, nullptr);
"""
    worker_handoff_bridge_identifier_snippets = {
        "OOCFinalizedCorpusEvidenceV1": "OOCFinalizedCorpusEvidenceV1 evidence;\n",
        "OOCPrivateHandoffPayloadV1": "OOCPrivateHandoffPayloadV1 payload;\n",
        "OOCPrivateHandoffPayloadBuilderV1": (
            "OOCPrivateHandoffPayloadBuilderV1 builder = nullptr;\n"
        ),
        "capture_finalized_corpus_evidence": (
            "capture_finalized_corpus_evidence(descriptor);\n"
        ),
        "finalize_and_publish_private_handoff_built": (
            "finalize_and_publish_private_handoff_built(builder, nullptr);\n"
        ),
    }
    worker_handoff_bridge_checks = Checks(Path("."))
    worker_handoff_bridge_checks.validate_worker_writer_use_site(
        "src/relation/untrusted_worker_handoff_bridge.cpp",
        worker_handoff_bridge_snippet,
    )
    expect(
        len(worker_handoff_bridge_checks.errors)
        == len(WORKER_HANDOFF_BRIDGE_IDENTIFIERS)
        and all(
            "worker-handoff evidence bridge use site is not allowlisted" in error
            for error in worker_handoff_bridge_checks.errors
        ),
        "worker-handoff evidence bridge repo-wide use-site gate is not enforced",
    )
    for identifier, allowlist in sorted(
        WORKER_HANDOFF_BRIDGE_IDENTIFIER_ALLOWLISTS.items()
    ):
        for relative in sorted(allowlist):
            allowed_worker_handoff_bridge_checks = Checks(Path("."))
            allowed_worker_handoff_bridge_checks.validate_worker_writer_use_site(
                relative, worker_handoff_bridge_identifier_snippets[identifier]
            )
            expect(
                not allowed_worker_handoff_bridge_checks.errors,
                f"allowlisted worker-handoff {identifier} use was rejected in "
                f"{relative}: {allowed_worker_handoff_bridge_checks.errors}",
            )
    worker_handoff_privileged_snippet = (
        worker_handoff_bridge_identifier_snippets[
            "OOCPrivateHandoffPayloadBuilderV1"
        ]
        + worker_handoff_bridge_identifier_snippets[
            "finalize_and_publish_private_handoff_built"
        ]
    )
    for relative in sorted(WORKER_HANDOFF_EVIDENCE_ONLY_FILES):
        evidence_only_checks = Checks(Path("."))
        evidence_only_checks.validate_worker_writer_use_site(
            relative, worker_handoff_privileged_snippet
        )
        expect(
            len(evidence_only_checks.errors) == 2
            and all(
                "worker-handoff evidence bridge use site is not allowlisted"
                in error
                for error in evidence_only_checks.errors
            ),
            f"evidence-only merge codec file gained private-handoff publisher "
            f"authority in {relative}: {evidence_only_checks.errors}",
        )
    expect(
        WORKER_HANDOFF_BRIDGE_IDENTIFIER_ALLOWLISTS
        == {
            "OOCFinalizedCorpusEvidenceV1": {
                "include/gnfs/relation/ooc_relation_store.hpp",
                WORKER_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            }
            | WORKER_HANDOFF_EVIDENCE_ONLY_FILES,
            "OOCPrivateHandoffPayloadV1": {
                "include/gnfs/relation/ooc_relation_store.hpp",
                WORKER_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "OOCPrivateHandoffPayloadBuilderV1": {
                "include/gnfs/relation/ooc_relation_store.hpp",
                WORKER_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "capture_finalized_corpus_evidence": {
                "include/gnfs/relation/ooc_relation_store.hpp",
                WORKER_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
            "finalize_and_publish_private_handoff_built": {
                "include/gnfs/relation/ooc_relation_store.hpp",
                WORKER_WRITER_IMPLEMENTATION_FILE,
                MERGE_WRITER_AUTHORITY_IMPLEMENTATION_FILE,
            },
        },
        "worker-handoff evidence bridge identifier allowlists are not exact",
    )

    raw_private_handoff_checks = Checks(Path("."))
    raw_private_handoff_checks.validate_worker_writer_use_site(
        "src/sieve/untrusted_raw_handoff.cpp",
        "writer.finalize_and_publish_private_handoff(1, 1, payload);",
    )
    expect(
        len(raw_private_handoff_checks.errors) == 1
        and "raw private-handoff publisher use is not allowlisted"
        in raw_private_handoff_checks.errors[0],
        "raw private-handoff publisher repo-wide use gate is not enforced",
    )
    raw_private_handoff_alias_checks = Checks(Path("."))
    raw_private_handoff_alias_checks.validate_worker_writer_use_site(
        "src/sieve/untrusted_raw_handoff.cpp",
        "auto raw = &OOCRelationWriter::finalize_and_publish_private_handoff;",
    )
    expect(
        len(raw_private_handoff_alias_checks.errors) == 1
        and "raw private-handoff publisher use is not allowlisted"
        in raw_private_handoff_alias_checks.errors[0],
        "raw private-handoff publisher alias escaped the repo-wide use gate",
    )
    for relative in sorted(RAW_PRIVATE_HANDOFF_PUBLISHER_ALLOWLIST):
        allowed_raw_private_handoff_checks = Checks(Path("."))
        allowed_raw_private_handoff_checks.validate_worker_writer_use_site(
            relative,
            "writer.finalize_and_publish_private_handoff(1, 1, payload);",
        )
        expect(
            not allowed_raw_private_handoff_checks.errors,
            f"allowlisted raw private-handoff publisher was rejected in "
            f"{relative}: {allowed_raw_private_handoff_checks.errors}",
        )
    expect(
        RAW_PRIVATE_HANDOFF_PUBLISHER_ALLOWLIST
        == {
            "include/gnfs/relation/ooc_relation_store.hpp",
            "tests/test_ooc_cleanup_transaction.cpp",
            WORKER_WRITER_IMPLEMENTATION_FILE,
        },
        "raw private-handoff publisher allowlist is not exact",
    )

    worker_handoff_publication_snippet = r"""
WorkerHandoffV1 finalize_and_publish_handoff_impl(Completion completion) {
    if (pending) {
        writer->finalize_and_publish_private_handoff_built(builder, context);
    } else {
        writer->finalize_and_publish_private_handoff(1, 1, payload);
    }
}
"""
    exact_worker_handoff_publication_checks = Checks(Path("."))
    exact_worker_handoff_publication_checks.validate_worker_handoff_publication_boundary(
        WORKER_WRITER_IMPLEMENTATION_FILE, worker_handoff_publication_snippet
    )
    expect(
        not exact_worker_handoff_publication_checks.errors,
        "exact typed-first worker handoff publication boundary was rejected",
    )
    for escaped_identifier in (
        WORKER_HANDOFF_TYPED_BUILDER_IDENTIFIER,
        RAW_PRIVATE_HANDOFF_PUBLISHER_IDENTIFIER,
    ):
        indirect_worker_handoff_checks = Checks(Path("."))
        indirect_worker_handoff_checks.validate_worker_handoff_publication_boundary(
            WORKER_WRITER_IMPLEMENTATION_FILE,
            worker_handoff_publication_snippet
            + f"\nauto escaped = &OOCRelationWriter::{escaped_identifier};\n",
        )
        expect(
            any(
                f"{escaped_identifier} authority must be used only as a direct call"
                in error
                for error in indirect_worker_handoff_checks.errors
            ),
            f"worker handoff {escaped_identifier} indirect authority is not rejected",
        )

    duplicate_raw_worker_handoff_checks = Checks(Path("."))
    duplicate_raw_worker_handoff_checks.validate_worker_handoff_publication_boundary(
        WORKER_WRITER_IMPLEMENTATION_FILE,
        worker_handoff_publication_snippet
        + "\nwriter->finalize_and_publish_private_handoff(1, 1, payload);\n",
    )
    expect(
        any(
            "must contain the only direct finalize_and_publish_private_handoff call"
            in error
            for error in duplicate_raw_worker_handoff_checks.errors
        ),
        "worker handoff raw retry call count is not closed",
    )

    missing_typed_worker_handoff_checks = Checks(Path("."))
    missing_typed_worker_handoff_checks.validate_worker_handoff_publication_boundary(
        WORKER_WRITER_IMPLEMENTATION_FILE,
        r"""
WorkerHandoffV1 finalize_and_publish_handoff_impl(Completion completion) {
    writer->finalize_and_publish_private_handoff(1, 1, payload);
}
""",
    )
    expect(
        any(
            "must contain the only direct finalize_and_publish_private_handoff_built call"
            in error
            for error in missing_typed_worker_handoff_checks.errors
        ),
        "worker handoff typed-builder call count is not closed",
    )

    private_handoff_resume_use_site_snippet = r"""
PrivateHandoffPublicationObservedPermitV1* observed = nullptr;
PrivateHandoffPublicationValidatedPermitV1* validated = nullptr;
auto admission =
    acquire_private_handoff_publication_resume_v1(paths, directory_identity);
"""
    untrusted_private_handoff_resume_checks = Checks(Path("."))
    untrusted_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        "src/sieve/untrusted_private_handoff_resume.cpp",
        private_handoff_resume_use_site_snippet,
    )
    expect(
        len(untrusted_private_handoff_resume_checks.errors) == 3
        and all(
            "private-handoff publication resume use site is not allowlisted"
            in error
            for error in untrusted_private_handoff_resume_checks.errors
        ),
        "private-handoff publication resume repo-wide use-site gate is not "
        f"enforced: {untrusted_private_handoff_resume_checks.errors}",
    )
    expect(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_USE_SITE_ALLOWLIST
        == {
            "src/relation/ooc_private_cleanup_union_internal.hpp",
            "src/relation/ooc_private_cleanup_union.cpp",
            "src/sieve/distributed_sieve_wave_store.cpp",
            "tests/test_ooc_cleanup_transaction.cpp",
        },
        "private-handoff publication resume allowlist is not exact",
    )
    expect(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_AUXILIARY_USE_SITE_COUNTS
        == {
            CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE: {
                "PrivateHandoffPublicationPrefixWitnessV1": 3,
                "PrivateHandoffPublicationValidatedPermitV1": 3,
            },
        },
        "private-handoff publication resume auxiliary use-site counts are not "
        "exact",
    )
    exact_consumed_adoption_resume_auxiliary = r"""
struct PrivateHandoffPublicationPrefixWitnessV1;
class PrivateHandoffPublicationValidatedPermitV1;
std::shared_ptr<const PrivateHandoffPublicationPrefixWitnessV1> retained;
const PrivateHandoffPublicationPrefixWitnessV1* expected = nullptr;
void consume(PrivateHandoffPublicationValidatedPermitV1&& permit);
void read(PrivateHandoffPublicationValidatedPermitV1& permit);
"""
    exact_consumed_adoption_resume_auxiliary_checks = Checks(Path("."))
    exact_consumed_adoption_resume_auxiliary_checks.validate_private_handoff_publication_resume_boundary(
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE,
        exact_consumed_adoption_resume_auxiliary,
    )
    expect(
        not exact_consumed_adoption_resume_auxiliary_checks.errors,
        "exact consumed-adoption resume auxiliary use site was rejected: "
        f"{exact_consumed_adoption_resume_auxiliary_checks.errors}",
    )
    expanded_consumed_adoption_resume_auxiliary_checks = Checks(Path("."))
    expanded_consumed_adoption_resume_auxiliary_checks.validate_private_handoff_publication_resume_boundary(
        CONSUMED_CANONICAL_ADOPTION_INTERFACE_FILE,
        exact_consumed_adoption_resume_auxiliary
        + "\nPrivateHandoffPublicationResumeResultV1 escaped;\n",
    )
    expect(
        any(
            "auxiliary use site must contain exactly 0 "
            "PrivateHandoffPublicationResumeResultV1 identifiers, found 1"
            in error
            for error in expanded_consumed_adoption_resume_auxiliary_checks.errors
        ),
        "consumed-adoption auxiliary header accepted expanded resume authority: "
        f"{expanded_consumed_adoption_resume_auxiliary_checks.errors}",
    )
    expect(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_NARROW_TEST_DIRECT_CALL_IDENTIFIERS
        == {
            PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE: {
                PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER,
            },
        },
        "private-handoff publication resume narrow test mapping is not exact",
    )
    narrow_resume_test_source = r"""
auto first = private_lease::acquire_private_handoff_publication_resume_v1(
    first_paths, first_directory_identity);
auto second = private_lease::acquire_private_handoff_publication_resume_v1(
    second_paths, second_directory_identity);
"""
    narrow_resume_test_checks = Checks(Path("."))
    narrow_resume_test_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE,
        narrow_resume_test_source,
    )
    expect(
        not narrow_resume_test_checks.errors,
        "two direct narrow-test resume acquisitions were rejected: "
        f"{narrow_resume_test_checks.errors}",
    )

    third_narrow_resume_acquire_checks = Checks(Path("."))
    third_narrow_resume_acquire_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE,
        narrow_resume_test_source
        + "\nauto third = private_lease::"
        + PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER
        + "(third_paths, third_directory_identity);\n",
    )
    expect(
        not third_narrow_resume_acquire_checks.errors,
        "third direct narrow-test resume acquisition was count-bound: "
        f"{third_narrow_resume_acquire_checks.errors}",
    )

    aliased_narrow_resume_acquire_checks = Checks(Path("."))
    aliased_narrow_resume_acquire_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE,
        narrow_resume_test_source
        + "\nauto acquire_alias = &private_lease::"
        + PRIVATE_HANDOFF_PUBLICATION_RESUME_ACQUIRE_IDENTIFIER
        + ";\n",
    )
    expect(
        any(
            "narrow resume test authority is direct-call-only; aliases and "
            "function-pointer references are forbidden"
            in error
            for error in aliased_narrow_resume_acquire_checks.errors
        ),
        "narrow-test resume acquisition alias escaped direct-call closure: "
        f"{aliased_narrow_resume_acquire_checks.errors}",
    )

    for escaped_identifier in (
        PRIVATE_HANDOFF_PUBLICATION_RESUME_VALIDATE_IDENTIFIER,
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER,
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_IDENTIFIER,
    ):
        escaped_narrow_resume_authority_checks = Checks(Path("."))
        escaped_narrow_resume_authority_checks.validate_private_handoff_publication_resume_boundary(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_TEST_FILE,
            narrow_resume_test_source
            + f"\nauto escaped = private_lease::{escaped_identifier}(arguments);\n",
        )
        expect(
            any(
                "narrow resume test use site forbids authority: "
                f"{escaped_identifier}"
                in error
                for error in escaped_narrow_resume_authority_checks.errors
            ),
            f"narrow-test {escaped_identifier} authority escaped zero-use closure: "
            f"{escaped_narrow_resume_authority_checks.errors}",
        )

    expect(
        PRIVATE_HANDOFF_PUBLICATION_WORKER_VALIDATOR_AUTHORITY_ALLOWLIST
        == {
            PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
            PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        }
        and PRIVATE_HANDOFF_PUBLICATION_TEST_VALIDATOR_AUTHORITY_ALLOWLIST
        == {
            PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
            PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE,
        },
        "private-handoff production and test validator mint authority allowlists "
        "are not exact",
    )
    untrusted_private_handoff_validator_authority_checks = Checks(Path("."))
    untrusted_private_handoff_validator_authority_checks.validate_private_handoff_publication_resume_boundary(
        "src/sieve/untrusted_private_handoff_validator.cpp",
        "WorkerHandoffTypedValidatorAuthorityV1* production = nullptr;\n"
        "PrivateHandoffPublicationTypedValidatorTestAuthorityV1* test = nullptr;\n",
    )
    expect(
        any(
            "worker-handoff typed-validator mint authority is not allowlisted"
            in error
            for error in untrusted_private_handoff_validator_authority_checks.errors
        )
        and any(
            "test-only private-handoff typed-validator mint authority is not "
            "allowlisted"
            in error
            for error in untrusted_private_handoff_validator_authority_checks.errors
        ),
        "production or test private-handoff validator mint authority escaped its "
        f"closed allowlist: {untrusted_private_handoff_validator_authority_checks.errors}",
    )
    allowed_private_handoff_resume_test_checks = Checks(Path("."))
    allowed_private_handoff_resume_test_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_TEST_FILE,
        private_handoff_resume_use_site_snippet,
    )
    expect(
        not allowed_private_handoff_resume_test_checks.errors,
        "dedicated private-handoff publication resume test use was rejected: "
        f"{allowed_private_handoff_resume_test_checks.errors}",
    )

    valid_private_handoff_resume_interface = r"""
namespace gnfs::sieve::distributed_sieve_resume_detail {
class WorkerHandoffTypedValidatorAuthorityV1;
}
enum class PrivateHandoffPublicationResumeObservationPointV1 : std::uint8_t {
    AfterExpectedPrefixValidated,
    BeforePendingRollbackSourceDirectorySync,
    AfterPendingRollbackSourceDirectoryDurable,
    BeforePendingRollbackDestinationDirectorySync,
    AfterPendingRollbackDestinationDirectoryDurable,
    AfterPendingRollbackPreactiveDirectoryQuarantinedDurable,
    AfterPendingRollbackPreactiveDataRemovedDurable,
    AfterPendingRollbackPreactiveIndexRemovedDurable,
    AfterPendingRollbackOwnerRemovedDurable,
    AfterPendingRollbackLeaseDirectoryRemovedDurable,
    AfterPendingRollbackReservedRemovedDurable,
    AfterPendingRollbackOwnedRemovedDurable,
    BeforePendingRollbackTombstoneRemovalValidated,
    AfterPendingRollbackTombstoneRemovedDurable,
    AfterCanonicalConfirmedDurable,
    AfterReservedRevokedDurable,
    Count,
};
struct PrivateHandoffPublicationResumeTestHooksV1 final {
    using StopAfter = bool (*)(PrivateHandoffPublicationResumeObservationPointV1 point,
                               void* context) noexcept;
    using FailBefore = bool (*)(PrivateHandoffPublicationResumeObservationPointV1 point,
                                void* context) noexcept;
    StopAfter stop_after = nullptr;
    FailBefore fail_before = nullptr;
    void* context = nullptr;
};
class PrivateHandoffPublicationTypedValidatorV1 final {
private:
    int state;
    friend class gnfs::sieve::distributed_sieve_resume_detail::
        WorkerHandoffTypedValidatorAuthorityV1;
    friend class PrivateHandoffPublicationTypedValidatorTestAuthorityV1;
    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
};
class PrivateHandoffPublicationObservedPermitV1 final {
private:
    int state;
    friend PrivateHandoffPublicationResumeAdmissionV1
    acquire_private_handoff_publication_resume_v1(
        const OOCCleanupPaths& paths,
        const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept;
    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
    friend class PrivateHandoffPublicationValidatedPermitV1;
};
class PrivateHandoffPublicationValidatedPermitV1 final {
private:
    int state;
    friend PrivateHandoffPublicationResumeValidationV1
    validate_private_handoff_publication_resume_v1(
        PrivateHandoffPublicationObservedPermitV1&& observed,
        PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
    friend PrivateHandoffPublicationResumeRevalidationV1
    revalidate_private_handoff_publication_resume_v1(
        const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept;
    friend PrivateHandoffPublicationResumeResultV1
    reconcile_private_handoff_publication_for_resume_v1(
        PrivateHandoffPublicationValidatedPermitV1& permit,
        PrivateHandoffPublicationResumeTestHooksV1 hooks) noexcept;
    friend OOCPrivateHandoffAdoptionResult
    adopt_consumed_canonical_private_handoff_publication_v1(
        PrivateHandoffPublicationValidatedPermitV1&& permit,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
    friend PrivateHandoffPublicationReaderAdoptionResultV1
    adopt_consumed_canonical_private_handoff_reader_v1(
        PrivateHandoffPublicationValidatedPermitV1& permit,
        PrivateHandoffPublicationAdoptionRevalidatorV1&& revalidator,
        OOCPrivateHandoffAdoptionTestHooks hooks) noexcept;
};
[[nodiscard]] PrivateHandoffPublicationResumeAdmissionV1
acquire_private_handoff_publication_resume_v1(
    const OOCCleanupPaths& paths,
    const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept;
[[nodiscard]] PrivateHandoffPublicationResumeValidationV1
validate_private_handoff_publication_resume_v1(
    PrivateHandoffPublicationObservedPermitV1&& observed,
    PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept;
[[nodiscard]] PrivateHandoffPublicationResumeRevalidationV1
revalidate_private_handoff_publication_resume_v1(
    const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept;
[[nodiscard]] PrivateHandoffPublicationResumeResultV1
reconcile_private_handoff_publication_for_resume_v1(
    PrivateHandoffPublicationValidatedPermitV1& permit,
    PrivateHandoffPublicationResumeTestHooksV1 hooks = {}) noexcept;
"""
    exact_private_handoff_resume_interface_checks = Checks(Path("."))
    exact_private_handoff_resume_interface_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
        valid_private_handoff_resume_interface,
    )
    expect(
        not exact_private_handoff_resume_interface_checks.errors,
        "exact private-handoff resume interface declarations were rejected: "
        f"{exact_private_handoff_resume_interface_checks.errors}",
    )

    renamed_private_handoff_resume_observation_checks = Checks(Path("."))
    renamed_private_handoff_resume_observation_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
        valid_private_handoff_resume_interface.replace(
            "AfterPendingRollbackLeaseDirectoryRemovedDurable",
            "AfterPendingRollbackFinalDirectoryRemovedDurable",
        ),
    )
    expect(
        any(
            "observation enum must remain the exact ordered durable-boundary "
            "authority"
            in error
            for error in renamed_private_handoff_resume_observation_checks.errors
        ),
        "renamed relation private-handoff observation escaped enum closure: "
        f"{renamed_private_handoff_resume_observation_checks.errors}",
    )

    extra_private_handoff_resume_hook_checks = Checks(Path("."))
    extra_private_handoff_resume_hook_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
        valid_private_handoff_resume_interface.replace(
            "    void* context = nullptr;\n};\n"
            "class PrivateHandoffPublicationTypedValidatorV1",
            "    void* context = nullptr;\n"
            "    bool unchecked = false;\n};\n"
            "class PrivateHandoffPublicationTypedValidatorV1",
        ),
    )
    expect(
        any(
            "resume test hooks must remain the exact closed test-only seam"
            in error
            for error in extra_private_handoff_resume_hook_checks.errors
        ),
        "extra relation private-handoff test-hook state escaped field closure: "
        f"{extra_private_handoff_resume_hook_checks.errors}",
    )

    extra_private_handoff_resume_friend_checks = Checks(Path("."))
    extra_private_handoff_resume_friend_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
        valid_private_handoff_resume_interface.replace(
            "    friend class PrivateHandoffPublicationTypedValidatorTestAuthorityV1;\n",
            "    friend class PrivateHandoffPublicationTypedValidatorTestAuthorityV1;\n"
            "    friend class EscapedTypedValidatorAuthority;\n",
        ),
    )
    expect(
        any(
            "PrivateHandoffPublicationTypedValidatorV1 friend authority must remain "
            "exactly closed"
            in error
            for error in extra_private_handoff_resume_friend_checks.errors
        ),
        "additional relation-header typed-validator friend escaped authority "
        f"closure: {extra_private_handoff_resume_friend_checks.errors}",
    )

    private_handoff_resume_interface_wrapper_checks = Checks(Path("."))
    private_handoff_resume_interface_wrapper_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_INTERFACE_FILE,
        valid_private_handoff_resume_interface
        + r"""
inline auto resume_wrapper(auto&& observed, const auto& projection) {
    return validate_private_handoff_publication_resume_v1(
        std::move(observed), projection);
}
""",
    )
    expect(
        any(
            "interface must contain exactly 4 declaration-shaped "
            "validate_private_handoff_publication_resume_v1 identifiers"
            in error
            for error in private_handoff_resume_interface_wrapper_checks.errors
        ),
        "inline relation-header private-handoff resume wrapper escaped identifier "
        f"count closure: {private_handoff_resume_interface_wrapper_checks.errors}",
    )

    valid_private_handoff_resume_implementation = (
        PRIVATE_HANDOFF_ROLLBACK_RECOVERY_DEFINITION_SHAPE
        + "{\n"
        + PRIVATE_HANDOFF_ROLLBACK_RECOVERY_BODY
        + "}\n"
        + r"""
struct PrivateHandoffLeaseRecoveryObservationAdapterV1 final {
    const PrivateHandoffPublicationResumeTestHooksV1* outer = nullptr;
    const OOCCleanupPaths* paths = nullptr;
    const BaseLock* lock = nullptr;
    const std::array<std::uint64_t, 3>* expected_directory_identity = nullptr;
    const PrivateHandoffPublicationPrefixWitnessV1* initial = nullptr;
    std::optional<OOCCleanupResult> exact_failure;
    bool unknown_point = false;
};
std::optional<PrivateHandoffPublicationResumeObservationPointV1>
map_private_handoff_lease_recovery_observation(
    OOCPrivateLeaseFaultPoint point) noexcept {
    using OuterPoint = PrivateHandoffPublicationResumeObservationPointV1;
    switch (point) {
    case OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveDirectoryQuarantinedDurable;
    case OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveDataRemovedDurable;
    case OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:
        return OuterPoint::AfterPendingRollbackPreactiveIndexRemovedDurable;
    case OOCPrivateLeaseFaultPoint::OwnerRemovedDurable:
        return OuterPoint::AfterPendingRollbackOwnerRemovedDurable;
    case OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:
        return OuterPoint::AfterPendingRollbackLeaseDirectoryRemovedDurable;
    case OOCPrivateLeaseFaultPoint::ReservedRemovedDurable:
        return OuterPoint::AfterPendingRollbackReservedRemovedDurable;
    case OOCPrivateLeaseFaultPoint::OwnedRemovedDurable:
        return OuterPoint::AfterPendingRollbackOwnedRemovedDurable;
    default:
        return std::nullopt;
    }
}
bool private_handoff_lease_recovery_stage_matches(
    OOCPrivateLeaseFaultPoint point,
    const RetainedPrivateHandoffPublicationPrefixV1& current) noexcept {
    const bool final_directory =
        current.generation.final_directory_identity.has_value();
    const bool staging_directory =
        current.generation.staging_directory_identity.has_value();
    const bool owner = current.witness.owner.has_value();
    const bool owned = current.witness.owned.has_value();
    const bool reserved = current.witness.reserved.has_value();
    const bool index = current.rollback_index_present;
    const bool data = current.rollback_data_present;
    switch (point) {
    case OOCPrivateLeaseFaultPoint::PreactiveDirectoryQuarantinedDurable:
        return !final_directory && staging_directory && owner && owned &&
               reserved && index && data;
    case OOCPrivateLeaseFaultPoint::PreactiveDataRemovedDurable:
        return !final_directory && staging_directory && owner && owned &&
               reserved && index && !data;
    case OOCPrivateLeaseFaultPoint::PreactiveIndexRemovedDurable:
        return !final_directory && staging_directory && owner && owned &&
               reserved && !index && !data;
    case OOCPrivateLeaseFaultPoint::OwnerRemovedDurable:
        return !final_directory && staging_directory && !owner && owned &&
               reserved && !index && !data;
    case OOCPrivateLeaseFaultPoint::FinalDirectoryRemovedDurable:
        return !final_directory && !staging_directory && !owner && owned &&
               reserved && !index && !data;
    case OOCPrivateLeaseFaultPoint::ReservedRemovedDurable:
        return !final_directory && !staging_directory && !owner && owned &&
               !reserved && !index && !data;
    case OOCPrivateLeaseFaultPoint::OwnedRemovedDurable:
        return !final_directory && !staging_directory && !owner && !owned &&
               !reserved && !index && !data;
    default:
        return false;
    }
}
bool observe_private_handoff_lease_recovery(
    OOCPrivateLeaseFaultPoint point, void* opaque) noexcept {
    auto& adapter =
        *static_cast<PrivateHandoffLeaseRecoveryObservationAdapterV1*>(opaque);
    const auto mapped = map_private_handoff_lease_recovery_observation(point);
    if (!mapped) {
        adapter.unknown_point = true;
        return true;
    }
    if (adapter.outer != nullptr && adapter.outer->stop_after != nullptr &&
        adapter.outer->stop_after(*mapped, adapter.outer->context)) {
        return true;
    }
    try {
        if (adapter.paths == nullptr || adapter.lock == nullptr ||
            adapter.expected_directory_identity == nullptr ||
            adapter.initial == nullptr) {
            adapter.exact_failure =
                resume_unexpected_result(protocol_error());
            return true;
        }
        auto current = capture_private_handoff_publication_prefix_v1_locked(
            *adapter.paths, *adapter.lock,
            *adapter.expected_directory_identity);
        const auto& initial = *adapter.initial;
        const auto remaining_marker_matches_initial =
            [](const std::optional<
                   PrivateHandoffPublicationLeaseMarkerWitnessV1>& observed,
               const std::optional<
                   PrivateHandoffPublicationLeaseMarkerWitnessV1>& expected) {
                return !observed || (expected && *observed == *expected);
            };
        if (!current.retained ||
            initial.state !=
                PrivateHandoffPublicationPrefixStateV1::PendingRollback ||
            initial.canonical_snapshot || initial.pending_snapshot ||
            !initial.rollback_snapshot ||
            current.retained->witness.state !=
                PrivateHandoffPublicationPrefixStateV1::PendingRollback ||
            current.retained->witness.record != initial.record ||
            current.retained->witness.canonical_snapshot ||
            current.retained->witness.pending_snapshot ||
            !current.retained->witness.rollback_snapshot ||
            current.retained->witness.rollback_snapshot !=
                initial.rollback_snapshot ||
            current.retained->witness.parent_identity !=
                initial.parent_identity ||
            current.retained->witness.lock_identity !=
                initial.lock_identity ||
            current.retained->witness.directory_identity !=
                initial.directory_identity ||
            !remaining_marker_matches_initial(
                current.retained->witness.owner, initial.owner) ||
            !remaining_marker_matches_initial(
                current.retained->witness.owned, initial.owned) ||
            !remaining_marker_matches_initial(
                current.retained->witness.reserved, initial.reserved) ||
            !private_handoff_lease_recovery_stage_matches(
                point, *current.retained)) {
            adapter.exact_failure = current.retained
                                        ? resume_foreign_replacement()
                                        : current.result;
            if (adapter.exact_failure->status ==
                OOCCleanupStatus::NoTransaction) {
                adapter.exact_failure = resume_foreign_replacement();
            }
            return true;
        }
        adapter.lock->require_stable();
        return false;
    } catch (const Failure& failure) {
        adapter.exact_failure = resume_failure_result(failure);
    } catch (const std::bad_alloc&) {
        adapter.exact_failure = resume_unexpected_result(
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::system_error& error) {
        adapter.exact_failure = resume_unexpected_result(error.code());
    } catch (...) {
        adapter.exact_failure = resume_unexpected_result();
    }
    return true;
}
PrivateHandoffPublicationResumeAdmissionV1
acquire_private_handoff_publication_resume_v1(
    const OOCCleanupPaths& paths,
    const std::array<std::uint64_t, 3>& expected_directory_identity) noexcept {
    auto lock = std::make_unique<BaseLock>(paths.lock_path, false);
    auto state = std::make_shared<PrivateHandoffPublicationObservedPermitV1::State>(
        paths, expected_directory_identity, std::move(*captured.retained));
    state->lock = std::move(lock);
    claim.transfer_to_permit();
    return {};
}
PrivateHandoffPublicationResumeValidationV1
validate_private_handoff_publication_resume_v1(
    PrivateHandoffPublicationObservedPermitV1&& observed,
    PrivateHandoffPublicationTypedValidatorV1&& validator) noexcept {
    auto state = std::move(observed.state_);
    const auto typed_validate = std::exchange(validator.validate_, nullptr);
    void* const typed_context = std::exchange(validator.context_, nullptr);
    const auto typed_creator_process_id =
        std::exchange(validator.creator_process_id_, 0);
    try {
        return {};
    } catch (...) {
        return {};
    }
}
PrivateHandoffPublicationResumeRevalidationV1
revalidate_private_handoff_publication_resume_v1(
    const PrivateHandoffPublicationValidatedPermitV1& permit) noexcept {
    return {};
}
"""
        + PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPES[
            "recover_private_lease_locked"
        ]
        + "{\n"
        + PRIVATE_LEASE_GENERIC_RECOVERY_BODIES["recover_private_lease_locked"]
        + "}\n"
        + PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPES[
            "OOCCleanupTransaction::remove_private_lease"
        ]
        + "{\n"
        + PRIVATE_LEASE_GENERIC_RECOVERY_BODIES[
            "OOCCleanupTransaction::remove_private_lease"
        ]
        + "}\n"
        + r"""
PrivateHandoffPublicationResumeResultV1
reconcile_private_handoff_publication_for_resume_v1(
    PrivateHandoffPublicationValidatedPermitV1& permit,
    PrivateHandoffPublicationResumeTestHooksV1 hooks) noexcept {
    PrivateHandoffLeaseRecoveryObservationAdapterV1 lease_adapter{
        .outer = &hooks,
        .paths = &state->paths,
        .lock = &lock,
        .expected_directory_identity =
            &state->expected_directory_identity,
        .initial = &rollback_retained.witness,
    };
    recovered = recover_private_handoff_rollback_generation_locked(
        state->paths, lock, rollback_retained.generation.parent_identity,
        *rollback_retained.generation.owned, rollback_retained.generation.reserved,
        OOCPrivateLeaseTestHooks{
            .stop_after = observe_private_handoff_lease_recovery,
            .context = &lease_adapter,
        });
    if (lease_adapter.exact_failure) {
        return resume_failed(*lease_adapter.exact_failure, expected);
    }
    if (lease_adapter.unknown_point) {
        return resume_failed(
            resume_unexpected_result(protocol_error()), expected);
    }
    if (!recovered.completed()) {
        return {};
    }
    auto absent = capture_private_handoff_publication_prefix_v1_locked(
        state->paths, lock, state->expected_directory_identity);
    if (absent.retained ||
        absent.result.status != OOCCleanupStatus::NoTransaction ||
        absent.result.stage != OOCCleanupStage::None ||
        absent.result.native_error) {
        return resume_failed(
            absent.retained ? resume_foreign_replacement() : absent.result,
            expected);
    }
    return {};
}
"""
    )
    exact_private_handoff_resume_implementation_checks = Checks(Path("."))
    exact_private_handoff_resume_implementation_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation,
    )
    expect(
        not exact_private_handoff_resume_implementation_checks.errors,
        "exact private-handoff resume implementation definitions were rejected: "
        f"{exact_private_handoff_resume_implementation_checks.errors}",
    )

    valid_private_lease_preactive_rollback_core = (
        PRIVATE_LEASE_PREACTIVE_SCANNER_DEFINITION_SHAPE
        + "{\n"
        + PRIVATE_LEASE_PREACTIVE_SCANNER_BODY
        + "}\n"
        + PRIVATE_LEASE_PREACTIVE_ROLLBACK_DEFINITION_SHAPE
        + "{\n"
        + PRIVATE_LEASE_PREACTIVE_ROLLBACK_BODY
        + "}\n"
        + PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPE
        + "{\n"
        + PRIVATE_LEASE_GENERIC_RECOVERY_BODY
        + "}\n"
    )
    exact_private_lease_preactive_rollback_core_checks = Checks(Path("."))
    exact_private_lease_preactive_rollback_core_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core,
    )
    expect(
        not exact_private_lease_preactive_rollback_core_checks.errors,
        "exact shared preactive rollback core was rejected: "
        f"{exact_private_lease_preactive_rollback_core_checks.errors}",
    )

    weakened_private_lease_preactive_scanner_checks = Checks(Path("."))
    weakened_private_lease_preactive_scanner_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core.replace(
            """        fail(OOCCleanupStatus::NamespaceConflict, OOCCleanupStage::None, protocol_error());
    }
    return entries;
""",
            """        continue;
    }
    return entries;
""",
            1,
        ),
    )
    expect(
        any(
            "preactive rollback directory scanner must retain the exact "
            "owner/index/data-only allowlist" in error
            for error in weakened_private_lease_preactive_scanner_checks.errors
        ),
        "preactive scanner accepted an unknown child after allowlist drift: "
        f"{weakened_private_lease_preactive_scanner_checks.errors}",
    )

    tombstone_aware_shared_private_lease_rollback_checks = Checks(Path("."))
    tombstone_aware_shared_private_lease_rollback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core.replace(
            """    if (owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
""",
            """    if (std::filesystem::exists(paths.private_handoff_rollback_path)) {
        return private_lease_completed();
    }
    if (owned.capability != PrivateLeaseCapability::RollbackPreactivePairAndLease) {
""",
            1,
        ),
    )
    expect(
        any(
            "shared preactive rollback executor must retain its exact capability"
            in error
            for error in tombstone_aware_shared_private_lease_rollback_checks.errors
        ),
        "shared preactive rollback accepted a tombstone-conditioned early success: "
        f"{tombstone_aware_shared_private_lease_rollback_checks.errors}",
    )

    unscanned_quarantine_private_lease_rollback_checks = Checks(Path("."))
    unscanned_quarantine_private_lease_rollback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core.replace(
            "inspect_private_lease_preactive_entries(staging_path, paths);",
            "inspect_private_lease_control_entries(staging_path, paths);",
            1,
        ),
    )
    expect(
        any(
            "shared preactive rollback executor must retain its exact capability"
            in error
            for error in unscanned_quarantine_private_lease_rollback_checks.errors
        ),
        "shared preactive rollback lost its post-quarantine preactive re-scan: "
        f"{unscanned_quarantine_private_lease_rollback_checks.errors}",
    )

    relocated_generic_private_lease_rollback_checks = Checks(Path("."))
    relocated_generic_private_lease_rollback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core.replace(
            "rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);",
            "private_lease_completed();",
            2,
        )
        + r"""
#if 0
OOCCleanupResult decoy_generic_private_lease_rollback_one() {
    return rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
}
OOCCleanupResult decoy_generic_private_lease_rollback_two() {
    return rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
}
#endif
""",
    )
    expect(
        any(
            "generic private-lease recovery core must retain its exact "
            "validation, rollback, and marker-tail contract" in error
            for error in relocated_generic_private_lease_rollback_checks.errors
        ),
        "generic rollback calls escaped their real executor into inactive decoys: "
        f"{relocated_generic_private_lease_rollback_checks.errors}",
    )

    early_success_generic_private_lease_core_checks = Checks(Path("."))
    early_success_generic_private_lease_core_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_LEASE_PREACTIVE_ROLLBACK_CORE_FILE,
        valid_private_lease_preactive_rollback_core.replace(
            PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPE
            + "{\n"
            + "\n"
            + "    lock.require_stable();\n",
            PRIVATE_LEASE_GENERIC_RECOVERY_DEFINITION_SHAPE
            + "{\n"
            + "\n"
            + "    return private_lease_completed();\n"
            + "    lock.require_stable();\n",
            1,
        ),
    )
    expect(
        any(
            "generic private-lease recovery core must retain its exact "
            "validation, rollback, and marker-tail contract" in error
            for error in early_success_generic_private_lease_core_checks.errors
        ),
        "generic private-lease core accepted a pre-validation terminal success: "
        f"{early_success_generic_private_lease_core_checks.errors}",
    )

    generic_typed_private_handoff_rollback_checks = Checks(Path("."))
    generic_typed_private_handoff_rollback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "    recovered = recover_private_handoff_rollback_generation_locked(\n",
            "    recovered = recover_owned_private_lease_locked(\n",
            1,
        ),
    )
    expect(
        any(
            "typed private-handoff rollback must call only the dedicated" in error
            for error in generic_typed_private_handoff_rollback_checks.errors
        ),
        "typed rollback escaped onto the generic cleanup-union executor: "
        f"{generic_typed_private_handoff_rollback_checks.errors}",
    )

    escaped_shared_private_lease_rollback_checks = Checks(Path("."))
    escaped_shared_private_lease_rollback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation
        + r"""
OOCCleanupResult escaped_shared_private_lease_rollback() {
    return rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
}
""",
    )
    expect(
        any(
            "shared preactive rollback executor escaped the closed "
            "generic-plus-typed call-site allowlist" in error
            for error in escaped_shared_private_lease_rollback_checks.errors
        ),
        "shared preactive rollback escaped to a fourth production caller: "
        f"{escaped_shared_private_lease_rollback_checks.errors}",
    )

    escaped_file_private_lease_rollback_checks = Checks(Path("."))
    escaped_file_private_lease_rollback_checks.validate_private_handoff_publication_resume_boundary(
        "src/relation/escaped_preactive_rollback.cpp",
        r"""
OOCCleanupResult escaped_file_private_lease_rollback() {
    return rollback_owned_preactive_pair_locked(paths, owned, lock, hooks);
}
""",
    )
    expect(
        any(
            "shared preactive rollback executor escaped the closed "
            "generic-plus-typed call-site allowlist" in error
            for error in escaped_file_private_lease_rollback_checks.errors
        ),
        "shared preactive rollback escaped into a new production file: "
        f"{escaped_file_private_lease_rollback_checks.errors}",
    )

    narrow_ordinary_private_lease_recovery_checks = Checks(Path("."))
    narrow_ordinary_private_lease_recovery_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "        return recover_owned_private_lease_locked("
            "paths, held_lock, parent_identity, *owned,\n"
            "                                                  reserved, hooks);\n",
            "        return recover_private_handoff_rollback_generation_locked("
            "paths, held_lock, parent_identity, *owned,\n"
            "                                                  reserved, hooks);\n",
            1,
        ),
    )
    expect(
        any(
            "ordinary private-lease recovery must remain on the exact two generic" in error
            for error in narrow_ordinary_private_lease_recovery_checks.errors
        ),
        "ordinary cleanup escaped onto the tombstone-authorized narrow executor: "
        f"{narrow_ordinary_private_lease_recovery_checks.errors}",
    )

    relocated_recovery_generic_call = (
        valid_private_handoff_resume_implementation.replace(
            """        return recover_owned_private_lease_locked(paths, held_lock, parent_identity, *owned,
                                                  reserved, hooks);
""",
            "        return private_lease_completed();\n",
            1,
        )
        + r"""
#if 0
OOCCleanupResult unused_recovery_call_site() {
    return recover_owned_private_lease_locked(
        paths, held_lock, parent_identity, *owned, reserved, hooks);
}
#endif
"""
    )
    relocated_recovery_generic_call_checks = Checks(Path("."))
    relocated_recovery_generic_call_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        relocated_recovery_generic_call,
    )
    expect(
        any(
            "recover_private_lease_locked must retain cleanup-union admission" in error
            for error in relocated_recovery_generic_call_checks.errors
        ),
        "recover-private-lease generic call escaped into an unused scope: "
        f"{relocated_recovery_generic_call_checks.errors}",
    )

    relocated_removal_generic_call = (
        valid_private_handoff_resume_implementation.replace(
            """        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
""",
            "        return ooc_cleanup_detail::private_lease_completed();\n",
            1,
        )
        + r"""
OOCCleanupResult unused_removal_call_site() {
    return ooc_cleanup_detail::recover_owned_private_lease_locked(
        paths, retained_lock, generation.parent_identity, *generation.owned,
        generation.reserved, hooks);
}
"""
    )
    relocated_removal_generic_call_checks = Checks(Path("."))
    relocated_removal_generic_call_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        relocated_removal_generic_call,
    )
    expect(
        any(
            "OOCCleanupTransaction::remove_private_lease must retain cleanup-union "
            "admission" in error
            for error in relocated_removal_generic_call_checks.errors
        ),
        "remove-private-lease generic call escaped into an unused scope: "
        f"{relocated_removal_generic_call_checks.errors}",
    )

    inactive_removal_generic_call_checks = Checks(Path("."))
    inactive_removal_generic_call_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
""",
            """#if 0
        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
#endif
        return ooc_cleanup_detail::private_lease_completed();
""",
            1,
        ),
    )
    expect(
        any(
            "OOCCleanupTransaction::remove_private_lease must retain cleanup-union "
            "admission" in error
            for error in inactive_removal_generic_call_checks.errors
        ),
        "inactive remove-private-lease generic return escaped scope closure: "
        f"{inactive_removal_generic_call_checks.errors}",
    )

    spliced_inactive_removal_replacement = (
        "#i"
        + "\\"
        + "\n"
        + "f 0\n"
        + """        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
"""
        + "#en"
        + "\\"
        + "\n"
        + "dif\n"
        + "        return ooc_cleanup_detail::private_lease_completed();\n"
    )
    spliced_inactive_removal_generic_call_checks = Checks(Path("."))
    spliced_inactive_removal_generic_call_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """        return ooc_cleanup_detail::recover_owned_private_lease_locked(
            paths, retained_lock, generation.parent_identity, *generation.owned,
            generation.reserved, hooks);
""",
            spliced_inactive_removal_replacement,
            1,
        ),
    )
    expect(
        any(
            "OOCCleanupTransaction::remove_private_lease must retain cleanup-union "
            "admission" in error
            for error in spliced_inactive_removal_generic_call_checks.errors
        ),
        "phase-2 line-spliced inactive generic return escaped scope closure: "
        f"{spliced_inactive_removal_generic_call_checks.errors}",
    )

    recover_admission_block = """    if (admission.blocked) {
        return *admission.blocked;
    }
"""
    recover_admission_decoy = (
        "    (void)admission.blocked;\n"
        '    const char* decoy = R"GNFS('
        + PRIVATE_LEASE_GENERIC_RECOVERY_SCOPES[
            "recover_private_lease_locked"
        ][0]
        + ')GNFS";\n'
        "    (void)decoy;\n"
    )
    literal_decoy_private_lease_admission_checks = Checks(Path("."))
    literal_decoy_private_lease_admission_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            recover_admission_block,
            recover_admission_decoy,
            1,
        ),
    )
    expect(
        any(
            "recover_private_lease_locked must retain cleanup-union admission"
            in error
            for error in literal_decoy_private_lease_admission_checks.errors
        ),
        "raw-string admission decoy escaped code-token scope closure: "
        f"{literal_decoy_private_lease_admission_checks.errors}",
    )

    early_success_private_lease_recovery_checks = Checks(Path("."))
    early_success_private_lease_recovery_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    auto admission = admit_private_cleanup_action_locked(
""",
            """    return private_lease_completed();
    auto admission = admit_private_cleanup_action_locked(
""",
            1,
        ),
    )
    expect(
        any(
            "recover_private_lease_locked must retain cleanup-union admission"
            in error
            for error in early_success_private_lease_recovery_checks.errors
        ),
        "pre-admission recovery success escaped exact scope closure: "
        f"{early_success_private_lease_recovery_checks.errors}",
    )

    handoff_guard_with_spliced_comment = (
        """    if (handoff.state != OOCPrivateHandoffState::None) {
        // phase-two splice """
        + "\\"
        + "\n"
        + """        return handoff.result;
    }
"""
    )
    spliced_comment_private_lease_guard_checks = Checks(Path("."))
    spliced_comment_private_lease_guard_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    if (handoff.state != OOCPrivateHandoffState::None) {
        return handoff.result;
    }
""",
            handoff_guard_with_spliced_comment,
            1,
        ),
    )
    expect(
        any(
            "recover_private_lease_locked must retain cleanup-union admission"
            in error
            for error in spliced_comment_private_lease_guard_checks.errors
        ),
        "line-comment phase-2 splice escaped protected recovery scope closure: "
        f"{spliced_comment_private_lease_guard_checks.errors}",
    )

    ignored_private_lease_admission_blocker_checks = Checks(Path("."))
    ignored_private_lease_admission_blocker_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    if (admission.blocked) {
        return *admission.blocked;
    }
""",
            "    (void)admission.blocked;\n",
            1,
        ),
    )
    expect(
        any(
            "recover_private_lease_locked must retain cleanup-union admission"
            in error
            for error in ignored_private_lease_admission_blocker_checks.errors
        ),
        "recover-private-lease ignored its cleanup-union blocker: "
        f"{ignored_private_lease_admission_blocker_checks.errors}",
    )

    weakened_private_handoff_rollback_executor_checks = Checks(Path("."))
    weakened_private_handoff_rollback_executor_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "owned.capability != "
            "PrivateLeaseCapability::RollbackPreactivePairAndLease",
            "owned.capability == "
            "PrivateLeaseCapability::RollbackPreactivePairAndLease",
            1,
        ),
    )
    expect(
        any(
            "dedicated private-handoff rollback executor must retain its exact" in error
            for error in weakened_private_handoff_rollback_executor_checks.errors
        ),
        "weakened tombstone rollback executor escaped exact-body closure: "
        f"{weakened_private_handoff_rollback_executor_checks.errors}",
    )

    drifted_private_handoff_rollback_binding_checks = Checks(Path("."))
    drifted_private_handoff_rollback_binding_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "state->paths, lock, rollback_retained.generation.parent_identity,",
            "state->paths, lock, rollback_retained.witness.parent_identity,",
            1,
        ),
    )
    expect(
        any(
            "typed private-handoff rollback must call only the dedicated" in error
            for error in drifted_private_handoff_rollback_binding_checks.errors
        ),
        "typed rollback escaped retained generation identity binding: "
        f"{drifted_private_handoff_rollback_binding_checks.errors}",
    )

    open_private_handoff_lease_adapter_checks = Checks(Path("."))
    open_private_handoff_lease_adapter_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    if (!mapped) {
        adapter.unknown_point = true;
        return true;
    }
""",
            """    if (!mapped) {
        return false;
    }
""",
        ),
    )
    expect(
        any(
            "seven-point lease-recovery adapter with unknown points interrupting "
            "fail closed"
            in error
            for error in open_private_handoff_lease_adapter_checks.errors
        ),
        "unknown nested lease-recovery point escaped fail-closed adapter: "
        f"{open_private_handoff_lease_adapter_checks.errors}",
    )

    forged_private_handoff_lease_capture_checks = Checks(Path("."))
    forged_private_handoff_lease_capture_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """        auto current = capture_private_handoff_publication_prefix_v1_locked(
            *adapter.paths, *adapter.lock,
            *adapter.expected_directory_identity);
""",
            "        auto current = forged_current;\n",
        ),
    )
    expect(
        any(
            "observe_private_handoff_lease_recovery must remain the exact "
            "seven-point lease-recovery adapter"
            in error
            for error in forged_private_handoff_lease_capture_checks.errors
        ),
        "forged post-callback relation prefix escaped fresh-capture sandwich: "
        f"{forged_private_handoff_lease_capture_checks.errors}",
    )

    weakened_private_handoff_lease_stage_checks = Checks(Path("."))
    weakened_private_handoff_lease_stage_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "reserved && index && data;",
            "reserved && index;",
            1,
        ),
    )
    expect(
        any(
            "private_handoff_lease_recovery_stage_matches must remain the exact "
            "seven-point lease-recovery adapter"
            in error
            for error in weakened_private_handoff_lease_stage_checks.errors
        ),
        "weakened point-specific rollback phase escaped seven-point matrix: "
        f"{weakened_private_handoff_lease_stage_checks.errors}",
    )

    late_private_handoff_exact_failure_checks = Checks(Path("."))
    late_private_handoff_exact_failure_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    if (lease_adapter.exact_failure) {
        return resume_failed(*lease_adapter.exact_failure, expected);
    }
    if (lease_adapter.unknown_point) {
        return resume_failed(
            resume_unexpected_result(protocol_error()), expected);
    }
""",
            """    if (lease_adapter.unknown_point) {
        return resume_failed(
            resume_unexpected_result(protocol_error()), expected);
    }
    if (lease_adapter.exact_failure) {
        return resume_failed(*lease_adapter.exact_failure, expected);
    }
""",
        ),
    )
    expect(
        any(
            "return exact_failure before unknown nested observation" in error
            for error in late_private_handoff_exact_failure_checks.errors
        ),
        "generic interruption result dominated exact relation failure: "
        f"{late_private_handoff_exact_failure_checks.errors}",
    )

    inexact_private_handoff_final_absence_checks = Checks(Path("."))
    inexact_private_handoff_final_absence_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            "absent.result.status != OOCCleanupStatus::NoTransaction",
            "absent.result.status != OOCCleanupStatus::HandoffPresent",
        ),
    )
    expect(
        any(
            "must prove exact NoTransaction absence" in error
            for error in inexact_private_handoff_final_absence_checks.errors
        ),
        "non-absent terminal relation prefix escaped exact NoTransaction gate: "
        f"{inexact_private_handoff_final_absence_checks.errors}",
    )

    unsafe_private_handoff_acquire_checks = Checks(Path("."))
    unsafe_private_handoff_acquire_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    auto state = std::make_shared<PrivateHandoffPublicationObservedPermitV1::State>(
        paths, expected_directory_identity, std::move(*captured.retained));
    state->lock = std::move(lock);
    claim.transfer_to_permit();
""",
            """    claim.transfer_to_permit();
    auto state = std::make_shared<PrivateHandoffPublicationObservedPermitV1::State>(
        paths, expected_directory_identity, std::move(*captured.retained));
    state->lock = std::move(lock);
""",
        ),
    )
    expect(
        any(
            "must finish all throwing State construction before noexcept lock "
            "adoption and action-claim transfer"
            in error
            for error in unsafe_private_handoff_acquire_checks.errors
        ),
        "claim transfer before throwing State construction escaped acquisition "
        f"exception-safety closure: {unsafe_private_handoff_acquire_checks.errors}",
    )

    late_private_handoff_validator_consume_checks = Checks(Path("."))
    late_private_handoff_validator_consume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation.replace(
            """    auto state = std::move(observed.state_);
    const auto typed_validate = std::exchange(validator.validate_, nullptr);
""",
            """    if (early_failure) {
        return {};
    }
    auto state = std::move(observed.state_);
    const auto typed_validate = std::exchange(validator.validate_, nullptr);
""",
        ),
    )
    expect(
        any(
            "typed validator must be consumed exactly once at validation entry"
            in error
            for error in late_private_handoff_validator_consume_checks.errors
        ),
        "early return before typed-validator consumption escaped entry closure: "
        f"{late_private_handoff_validator_consume_checks.errors}",
    )

    private_handoff_resume_implementation_wrapper_checks = Checks(Path("."))
    private_handoff_resume_implementation_wrapper_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_RELATION_IMPLEMENTATION_FILE,
        valid_private_handoff_resume_implementation
        + r"""
auto resume_wrapper(auto&& permit, const auto& hooks) {
    return reconcile_private_handoff_publication_for_resume_v1(
        std::move(permit), hooks);
}
""",
    )
    expect(
        any(
            "implementation must contain only the exact "
            "reconcile_private_handoff_publication_for_resume_v1 definition"
            in error
            for error in private_handoff_resume_implementation_wrapper_checks.errors
        ),
        "relation-implementation private-handoff resume wrapper escaped identifier "
        f"count closure: {private_handoff_resume_implementation_wrapper_checks.errors}",
    )

    valid_private_handoff_resume_wave_interface = (
        "enum class DistributedSieveWorkerHandoffResumeObservationPointV1 "
        ": std::uint8_t {\n"
        + "\n".join(
            f"    {point},"
            for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
        )
        + r"""
};
struct DistributedSieveWorkerHandoffResumeTestHooksV1 final {
    using StopAfter = bool (*)(
        DistributedSieveWorkerHandoffResumeObservationPointV1 point,
        void* context) noexcept;
    using AfterRoundLocksReleased = void (*)(void* context) noexcept;
    StopAfter stop_after = nullptr;
    AfterRoundLocksReleased after_round_locks_released = nullptr;
    void* context = nullptr;
};
"""
        + "enum class DistributedSieveMergePreparedResumeObservationPointV1 "
        ": std::uint8_t {\n"
        + "\n".join(
            f"    {point},"
            for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
        )
        + r"""
};
struct DistributedSieveMergePreparedResumeTestHooksV1 final {
    using StopAfter = bool (*)(
        DistributedSieveMergePreparedResumeObservationPointV1 point,
        void* context) noexcept;
    using FailBefore = bool (*)(
        DistributedSieveMergePreparedResumeObservationPointV1 point,
        void* context) noexcept;
    using AfterRoundLocksReleased = void (*)(void* context) noexcept;
    using StopBeforeRecoveredAggregateRevalidation = bool (*)(
        DistributedSieveRecoveredPreparedPublicationSubjectV1 subject,
        std::size_t manifest_slot,
        DistributedSieveRecoveredPreparedAggregatePhaseV1 phase,
        void* context) noexcept;
    StopAfter stop_after = nullptr;
    FailBefore fail_before = nullptr;
    AfterRoundLocksReleased after_round_locks_released = nullptr;
    StopBeforeRecoveredAggregateRevalidation
        stop_before_recovered_aggregate_revalidation = nullptr;
    void* context = nullptr;
};
"""
    )
    exact_private_handoff_resume_wave_interface_checks = Checks(Path("."))
    exact_private_handoff_resume_wave_interface_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_INTERFACE_FILE,
        valid_private_handoff_resume_wave_interface,
    )
    expect(
        not exact_private_handoff_resume_wave_interface_checks.errors,
        "exact WaveStore private-handoff observation/hook interface was rejected: "
        f"{exact_private_handoff_resume_wave_interface_checks.errors}",
    )

    escaped_merge_resume_hook_checks = Checks(Path("."))
    escaped_merge_resume_hook_checks.validate_private_handoff_publication_resume_boundary(
        "src/sieve/untrusted_merge_resume_hook.cpp",
        "DistributedSieveMergePreparedResumeTestHooksV1 escaped_hooks;\n",
    )
    expect(
        any(
            "MergePrepared publication-resume WaveStore identifier is not "
            "allowlisted: DistributedSieveMergePreparedResumeTestHooksV1"
            in error
            for error in escaped_merge_resume_hook_checks.errors
        ),
        "MergePrepared resume hook identifier escaped its internal-header, "
        "WaveStore, and resume-test use-site closure: "
        f"{escaped_merge_resume_hook_checks.errors}",
    )

    extra_private_handoff_resume_wave_hook_checks = Checks(Path("."))
    extra_private_handoff_resume_wave_hook_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_INTERFACE_FILE,
        valid_private_handoff_resume_wave_interface.replace(
            "    void* context = nullptr;\n};",
            "    void* context = nullptr;\n    bool unchecked = false;\n};",
        ),
    )
    expect(
        any(
            "WaveStore worker-handoff resume test hooks must remain the exact "
            "closed test-only seam"
            in error
            for error in extra_private_handoff_resume_wave_hook_checks.errors
        ),
        "extra WaveStore round-release hook state escaped field closure: "
        f"{extra_private_handoff_resume_wave_hook_checks.errors}",
    )

    valid_private_handoff_adoption = r"""
auto adopt_private_handoff_impl() noexcept {
    const auto first = paths.private_handoff_rollback_path;
    parent->require_lock_binding(paths.lock_path.filename(), *lock);
    if (parent->leaf_exists(paths.private_handoff_rollback_path.filename())) {
        return assign(adoption_failure(
            OOCCleanupStatus::NamespaceConflict,
            OOCPrivateHandoffState::TaintedPreserved,
            ooc_cleanup_detail::protocol_error()));
    }
    const auto directory_identity =
        parent->child_directory_identity(paths.private_directory.filename());
    const auto last = paths.private_handoff_rollback_path;
    return ready;
}
"""
    exact_private_handoff_adoption_checks = Checks(Path("."))
    exact_private_handoff_adoption_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FILE,
        valid_private_handoff_adoption,
    )
    expect(
        not exact_private_handoff_adoption_checks.errors,
        "exact private-handoff adoption rollback-tombstone blocker was rejected: "
        f"{exact_private_handoff_adoption_checks.errors}",
    )

    unchecked_private_handoff_adoption_checks = Checks(Path("."))
    unchecked_private_handoff_adoption_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_ADOPTION_FILE,
        valid_private_handoff_adoption.replace(
            """    if (parent->leaf_exists(paths.private_handoff_rollback_path.filename())) {
        return assign(adoption_failure(
            OOCCleanupStatus::NamespaceConflict,
            OOCPrivateHandoffState::TaintedPreserved,
            ooc_cleanup_detail::protocol_error()));
    }
""",
            "",
        ),
    )
    expect(
        any(
            "must reject the exact rollback tombstone after lock binding"
            in error
            for error in unchecked_private_handoff_adoption_checks.errors
        ),
        "unchecked rollback tombstone escaped private-handoff adoption closure: "
        f"{unchecked_private_handoff_adoption_checks.errors}",
    )

    private_handoff_resume_wave_mirror = "\n".join(
        "static_assert(static_cast<std::size_t>("
        "DistributedSieveWorkerHandoffResumeObservationPointV1::"
        f"{point}) == static_cast<std::size_t>("
        "private_lease::PrivateHandoffPublicationResumeObservationPointV1::"
        f"{point}));"
        for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
    )
    private_handoff_merge_resume_wave_mirror = (
        r"""
static_assert([] {
    using MergePoint =
        DistributedSieveMergePreparedResumeObservationPointV1;
    using RelationPoint =
        private_lease::PrivateHandoffPublicationResumeObservationPointV1;
    constexpr std::array wave{
"""
        + "\n".join(
            f"        MergePoint::{point},"
            for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
        )
        + r"""
    };
    constexpr std::array relation{
"""
        + "\n".join(
            f"        RelationPoint::{point},"
            for point in PRIVATE_HANDOFF_PUBLICATION_RESUME_OBSERVATION_POINTS
        )
        + r"""
    };
    for (std::size_t index = 0; index < wave.size(); ++index) {
        if (static_cast<std::size_t>(wave[index]) !=
            static_cast<std::size_t>(relation[index])) {
            return false;
        }
    }
    return true;
}());
"""
    )
    private_handoff_resume_wave_authority_and_callback = (
        private_handoff_resume_wave_mirror
        + private_handoff_merge_resume_wave_mirror
        + PRIVATE_HANDOFF_PUBLICATION_RETAINED_WORKER_STACK_SOURCE
        + r"""
struct MergePreparedPublicationAggregateWitness final {
    DistributedSieveMergeStartedRecordInventoryWitnessV1 start_record;
    RetainedWorkerHandoffPublicationPrefixStack retained_workers;
};
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_CAPTURE_RESULT_SOURCE
        + r"""
auto revalidate_merge_prepared_aggregate_projection() noexcept {
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_COPY_EXCEPTION_SOURCE
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_AGGREGATE_RETAINED_REVALIDATION_SOURCE
        + r"""
    return DistributedSieveWaveStoreDiagnostic{};
}
auto revalidate_recovered_prepared_reader_permit() noexcept {
    return private_lease::revalidate_private_handoff_publication_resume_v1(
        recovered_reader_permit);
}
"""
        + r"""
auto validate_merge_prepared_envelope() noexcept {
    return true;
}
auto validate_merge_prepared_dependency_projection() noexcept {
    return true;
}
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_CLOSED_BINDINGS_SOURCE
        + r"""
class WorkerHandoffTypedValidatorAuthorityV1 final {
public:
    [[nodiscard]] static
        gnfs::relation::ooc_cleanup_detail::PrivateHandoffPublicationTypedValidatorV1
        bind(gnfs::relation::ooc_cleanup_detail::PrivateHandoffPublicationTypedValidatorV1::
                 Validate validate,
             void* context) noexcept {
        return gnfs::relation::ooc_cleanup_detail::PrivateHandoffPublicationTypedValidatorV1(
            validate, context);
    }
};
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE
        + r"""

[[nodiscard]] bool validate_merge_prepared_prefix_type(
    const private_lease::PrivateHandoffPublicationPrefixWitnessV1& prefix,
    void* opaque) noexcept {
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_TYPED_CALLBACK_BODY
        + r"""
}
auto classify_merge_prepared_publication_prefix_v1() noexcept {
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_CAPTURE_SOURCE
        + r"""
    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, relation_identity(expected_directory_identity));
    MergePreparedTypedValidationContext typed_context{
        .attempt = &target_attempt,
        .manifest = &manifest,
        .start_record = &marker_bound_starts.witnesses->back(),
        .worker_attempts = &*worker_attempts.witnesses,
        .private_leases = &private_leases,
        .merge_starts = &*marker_bound_starts.witnesses,
        .root_fd = root_fd,
        .expected_directory_identity = expected_directory_identity,
        .creator_process_id = creator_process_id,
    };
    auto validation =
        private_lease::validate_private_handoff_publication_resume_v1(
            std::move(*admission.observed),
            WorkerHandoffTypedValidatorAuthorityV1::bind(
                validate_merge_prepared_prefix_type, &typed_context));
    const auto revalidated =
        private_lease::revalidate_private_handoff_publication_resume_v1(
            *validation.permit);
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_RETAINED_WORKER_POST_TARGET_SOURCE
        + r"""
    MergePreparedPublicationAggregateWitness aggregate{
        .start_record = start_record,
        .retained_workers = std::move(retained_workers),
    };
    return revalidated;
}
auto revalidate_exact_canonical_merge_started() noexcept {
    return true;
}
[[nodiscard]] bool validate_worker_handoff_prefix_type(
    const private_lease::PrivateHandoffPublicationPrefixWitnessV1& prefix,
    void* opaque) noexcept {
    auto* context = static_cast<WorkerHandoffTypedValidationContext*>(opaque);
    if (context == nullptr || context->root_fd < 0 || context->attempt == nullptr ||
        context->manifest == nullptr || context->attempt_record == nullptr ||
        !context->attempt->worker_attempt_names.has_value() ||
        !context->attempt->worker_coordinate.has_value() ||
        !context->attempt_record->canonical_snapshot.has_value() ||
        context->attempt_record->pending_snapshot.has_value()) {
        return false;
    }
    if (const auto exact = revalidate_exact_canonical_worker_attempt(
            context->root_fd, *context->attempt->worker_attempt_names,
            *context->attempt_record,
            context->creator_process_id);
        exact.status != DistributedSieveWaveStoreStatus::ready) {
        context->diagnostic = exact;
        return false;
    }
    const durable_record::RecordSnapshot* handoff_snapshot = nullptr;
    switch (prefix.state) {
    case private_lease::PrivateHandoffPublicationPrefixStateV1::PendingOnly:
        if (prefix.canonical_snapshot.has_value() ||
            !prefix.pending_snapshot.has_value() ||
            prefix.rollback_snapshot.has_value()) {
            return false;
        }
        handoff_snapshot = &*prefix.pending_snapshot;
        break;
    case private_lease::PrivateHandoffPublicationPrefixStateV1::PendingRollback:
        if (prefix.canonical_snapshot.has_value() ||
            prefix.pending_snapshot.has_value() ||
            !prefix.rollback_snapshot.has_value()) {
            return false;
        }
        handoff_snapshot = &*prefix.rollback_snapshot;
        break;
    case private_lease::PrivateHandoffPublicationPrefixStateV1::Canonical:
        if (!prefix.canonical_snapshot.has_value() ||
            prefix.pending_snapshot.has_value() ||
            prefix.rollback_snapshot.has_value()) {
            return false;
        }
        handoff_snapshot = &*prefix.canonical_snapshot;
        break;
    case private_lease::PrivateHandoffPublicationPrefixStateV1::IdenticalDual:
        if (!prefix.canonical_snapshot.has_value() ||
            !prefix.pending_snapshot.has_value() ||
            prefix.rollback_snapshot.has_value()) {
            return false;
        }
        handoff_snapshot = &*prefix.canonical_snapshot;
        break;
    case private_lease::PrivateHandoffPublicationPrefixStateV1::Count:
        return false;
    }
    const durable_record::RecordSnapshot index_snapshot{
        .identity = prefix.record.index.identity,
        .size = prefix.record.index.extent,
    };
    const durable_record::RecordSnapshot data_snapshot{
        .identity = prefix.record.data.identity,
        .size = prefix.record.data.extent,
    };
    auto typed = validate_worker_handoff_envelope(
        *context->attempt, *context->manifest, context->expected_directory_identity,
        prefix.record, *handoff_snapshot, index_snapshot, data_snapshot,
        context->creator_process_id);
    if (!typed) {
        context->diagnostic = std::move(typed.diagnostic);
        return false;
    }
    const auto& handoff = typed.witness->handoff;
    const auto& started = context->attempt_record->record;
    if (handoff.attempt_started_digest != started.self_digest ||
        handoff.lease != started.lease ||
        handoff.chunk_id != context->attempt->worker_coordinate->chunk_id ||
        handoff.attempt_ordinal !=
            context->attempt->worker_coordinate->attempt_ordinal) {
        context->diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::namespace_conflict,
                       protocol_error());
        return false;
    }
    context->typed_handoff = std::move(*typed.witness);
    return true;
}
[[nodiscard]] bool bridge_worker_handoff_resume_observation(
    private_lease::PrivateHandoffPublicationResumeObservationPointV1 point,
    void* opaque) noexcept {
    auto* context = static_cast<WorkerHandoffResumeBridgeContext*>(opaque);
    if (context == nullptr) {
        return true;
    }
    const auto wave_point =
        static_cast<DistributedSieveWorkerHandoffResumeObservationPointV1>(point);
    const bool user_requested_stop =
        context->user_hooks.stop_after != nullptr &&
        context->user_hooks.stop_after(
            wave_point, context->user_hooks.context);
    if (context->parent_components == nullptr ||
        context->root_leaf == nullptr ||
        context->manifest_bytes == nullptr ||
        context->absolute_root == nullptr ||
        context->manifest == nullptr ||
        context->aggregate == nullptr ||
        context->retained == nullptr ||
        context->attempt_names == nullptr ||
        context->attempt_record == nullptr) {
        context->revalidation_failed = true;
        context->revalidation_diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                       protocol_error());
        return true;
    }
    auto revalidated = validate_held_wave_store_manifest_authority(
        context->parent_fd, *context->parent_components, context->root_fd,
        *context->root_leaf, context->root_identity, context->lock_fd,
        context->lock_identity, *context->manifest_bytes,
        context->manifest_snapshot, context->creator_process_id);
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        revalidated = revalidate_worker_handoff_aggregate_projection(
            context->root_fd, *context->absolute_root, *context->manifest,
            *context->aggregate, *context->retained,
            context->current_attempt_index, context->creator_process_id,
            wave_point);
    }
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        revalidated = revalidate_exact_canonical_worker_attempt(
            context->root_fd, *context->attempt_names,
            *context->attempt_record, context->creator_process_id);
    }
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        return user_requested_stop;
    }
    context->revalidation_failed = true;
    context->revalidation_diagnostic = std::move(revalidated);
    return true;
}
[[nodiscard]] private_lease::PrivateHandoffPublicationResumeTestHooksV1
relation_worker_handoff_resume_hooks(
    WorkerHandoffResumeBridgeContext& context) noexcept {
    return {
        .stop_after = bridge_worker_handoff_resume_observation,
        .fail_before = nullptr,
        .context = &context,
    };
}
[[nodiscard]] bool bridge_merge_prepared_resume_observation(
    private_lease::PrivateHandoffPublicationResumeObservationPointV1 point,
    void* opaque, bool fail_before) noexcept {
    auto* context = static_cast<MergePreparedResumeBridgeContext*>(opaque);
    if (context == nullptr) {
        return true;
    }
    const auto wave_point =
        static_cast<DistributedSieveMergePreparedResumeObservationPointV1>(point);
    const bool user_requested_stop =
        fail_before
            ? context->user_hooks.fail_before != nullptr &&
                  context->user_hooks.fail_before(
                      wave_point, context->user_hooks.context)
            : context->user_hooks.stop_after != nullptr &&
                  context->user_hooks.stop_after(
                      wave_point, context->user_hooks.context);
    if (context->parent_components == nullptr ||
        context->root_leaf == nullptr ||
        context->manifest_bytes == nullptr ||
        context->absolute_root == nullptr ||
        context->manifest == nullptr ||
        context->aggregate == nullptr ||
        context->retained == nullptr) {
        context->revalidation_failed = true;
        context->revalidation_diagnostic =
            diagnostic(DistributedSieveWaveStoreStatus::unexpected_failure,
                       protocol_error());
        return true;
    }
    auto revalidated = validate_held_wave_store_manifest_authority(
        context->parent_fd, *context->parent_components, context->root_fd,
        *context->root_leaf, context->root_identity, context->lock_fd,
        context->lock_identity, *context->manifest_bytes,
        context->manifest_snapshot, context->creator_process_id);
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        revalidated = revalidate_merge_prepared_aggregate_projection(
            context->root_fd, *context->absolute_root, *context->manifest,
            *context->aggregate, *context->retained,
            context->creator_process_id, wave_point);
    }
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        revalidated = revalidate_exact_canonical_merge_started(
            context->root_fd, context->retained->names,
            context->retained->start_record, context->creator_process_id);
    }
    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        return user_requested_stop;
    }
    context->revalidation_failed = true;
    context->revalidation_diagnostic = std::move(revalidated);
    return true;
}
[[nodiscard]] bool bridge_merge_prepared_resume_stop_after(
    private_lease::PrivateHandoffPublicationResumeObservationPointV1 point,
    void* opaque) noexcept {
    return bridge_merge_prepared_resume_observation(point, opaque, false);
}
[[nodiscard]] bool bridge_merge_prepared_resume_fail_before(
    private_lease::PrivateHandoffPublicationResumeObservationPointV1 point,
    void* opaque) noexcept {
    return bridge_merge_prepared_resume_observation(point, opaque, true);
}
[[nodiscard]] private_lease::PrivateHandoffPublicationResumeTestHooksV1
relation_merge_prepared_resume_hooks(
    MergePreparedResumeBridgeContext& context) noexcept {
    return {
        .stop_after = bridge_merge_prepared_resume_stop_after,
        .fail_before = bridge_merge_prepared_resume_fail_before,
        .context = &context,
    };
}
"""
    )
    private_handoff_resume_capture_chain = r"""
    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, directory_identity);
    WorkerHandoffTypedValidationContext typed_context{
        .attempt = &attempt,
        .manifest = &manifest,
        .attempt_record = &candidate.attempt_record,
        .root_fd = root_fd,
        .expected_directory_identity = expected_directory_identity,
        .creator_process_id = creator_process_id,
    };
    auto validation =
        private_lease::validate_private_handoff_publication_resume_v1(
            std::move(*admission.observed),
            WorkerHandoffTypedValidatorAuthorityV1::bind(
                validate_worker_handoff_prefix_type, &typed_context));
    if (!validation.validated() || !validation.permit.has_value() ||
        !typed_context.typed_handoff.has_value()) {
        if (typed_context.diagnostic.status !=
            DistributedSieveWaveStoreStatus::ready) {
            return fail_with(std::move(typed_context.diagnostic));
        }
        return fail_with(worker_handoff_inspection_failure(validation.result));
    }
    const auto typed_handoff = *typed_context.typed_handoff;
    return validation;
"""
    private_handoff_resume_wave_store_suffix = (
        r"""
auto revalidate_held_private_handoff() noexcept {
    return private_lease::revalidate_private_handoff_publication_resume_v1(permit);
}
auto DistributedSieveWaveStore::open() noexcept {
"""
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_RESUME_OPEN_FRAGMENT
        + r"""
    auto adopted = private_lease::reconcile_private_handoff_publication_for_resume_v1(
        adopted_reader_permit);
    while (true) {
    auto result = private_lease::reconcile_private_handoff_publication_for_resume_v1(
        permit, relation_worker_handoff_resume_hooks(resume_bridge));
    while (!recoverable.retained.entries.empty()) {
        recoverable.retained.entries.pop_back();
    }
    if (hooks.worker_handoff_resume.after_round_locks_released != nullptr) {
        hooks.worker_handoff_resume.after_round_locks_released(
            hooks.worker_handoff_resume.context);
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }
    }
    ++resume_round;
    return result;
}
}
"""
    )
    valid_private_handoff_resume_wave_store = (
        "#if !defined(_WIN32)\n"
        + private_handoff_resume_wave_authority_and_callback
        + "\nauto capture_recoverable_worker_handoff_inventory() noexcept {\n"
        + private_handoff_resume_capture_chain
        + "}\n"
        + private_handoff_resume_wave_store_suffix
        + "\n#endif\n"
    )
    exact_private_handoff_resume_checks = Checks(Path("."))
    exact_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store,
    )
    expect(
        not exact_private_handoff_resume_checks.errors,
        "exact WaveStore private-handoff resume composition was rejected: "
        f"{exact_private_handoff_resume_checks.errors}",
    )

    macro_aliased_merge_callback_checks = Checks(Path("."))
    macro_aliased_merge_callback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        "#define validate_merge_prepared_envelope "
        "forged_validate_merge_prepared_envelope\n"
        + valid_private_handoff_resume_wave_store,
    )
    expect(
        any(
            "must not contain preprocessor macro definitions or undefinitions"
            in error
            for error in macro_aliased_merge_callback_checks.errors
        ),
        "preprocessor alias redirected the exact MergePrepared callback: "
        f"{macro_aliased_merge_callback_checks.errors}",
    )

    for forged_header in (
        "#define validate_merge_prepared_envelope "
        "forged_validate_merge_prepared_envelope\n",
        "%:def\\\nine validate_merge_prepared_envelope "
        "forged_validate_merge_prepared_envelope\n",
    ):
        forged_merge_header_checks = Checks(Path("."))
        forged_merge_header_checks.validate_merge_prepared_macro_alias_boundary(
            "include/forged_merge_alias.hpp", forged_header
        )
        expect(
            any(
                "closed authority identifier cannot be a preprocessor macro target"
                in error
                for error in forged_merge_header_checks.errors
            ),
            "repository header macro redirected the exact MergePrepared callback: "
            f"{forged_merge_header_checks.errors}",
        )

    generic_merge_overload_header_checks = Checks(Path("."))
    generic_merge_overload_header_checks.validate_merge_prepared_macro_alias_boundary(
        "include/generic_merge_overload.hpp",
        "#define GNFS_MERGE_ENVELOPE_OVERLOAD "
        "template<class... Args> auto validate_merge_prepared_envelope"
        "(Args&&... args)\n",
    )
    expect(
        any(
            "macro replacement cannot mention MergePrepared closed authority"
            in error
            for error in generic_merge_overload_header_checks.errors
        ),
        "generic macro replacement emitted a MergePrepared overload: "
        f"{generic_merge_overload_header_checks.errors}",
    )

    protected_keyword_macro_checks = Checks(Path("."))
    protected_keyword_macro_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store,
    )
    protected_keyword_macro_checks.validate_merge_prepared_macro_alias_boundary(
        "include/protected_keyword_macro.hpp",
        '#pragma push_macro("if")\n'
        "#define if(...) if(false)\n"
        '#pragma pop_macro("if")\n',
    )
    expect(
        any(
            "push_macro/pop_macro pragmas cannot reach" in error
            for error in protected_keyword_macro_checks.errors
        )
        and any(
            "macro target collides with a token in the MergePrepared protected interval"
            in error
            for error in protected_keyword_macro_checks.errors
        ),
        "keyword macro changed the compiled MergePrepared protected interval: "
        f"{protected_keyword_macro_checks.errors}",
    )

    dynamic_macro_use_wave_store = (
        valid_private_handoff_resume_wave_store.replace(
            "auto validate_merge_prepared_dependency_projection() noexcept {",
            "GNFS_MERGE_PROTECTED_HELPER\n"
            "auto validate_merge_prepared_dependency_projection() noexcept {",
            1,
        )
    )
    dynamic_macro_target_checks = Checks(Path("."))
    dynamic_macro_target_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        dynamic_macro_use_wave_store,
    )
    dynamic_macro_target_checks.validate_merge_prepared_macro_alias_boundary(
        "include/dynamic_merge_helper.hpp",
        "#define GNFS_MERGE_PROTECTED_HELPER static_assert(true);\n",
    )
    expect(
        any(
            "macro target collides with a token in the MergePrepared protected interval"
            in error
            for error in dynamic_macro_target_checks.errors
        ),
        "new macro invocation escaped the dynamic MergePrepared token inventory: "
        f"{dynamic_macro_target_checks.errors}",
    )

    late_merge_header_checks = Checks(Path("."))
    late_merge_header_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE,
            PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE
            + '\n#include "forged_merge_alias.hpp"',
            1,
        ),
    )
    expect(
        any(
            "must not include headers after the MergePrepared preprocessor macro guard"
            in error
            for error in late_merge_header_checks.errors
        ),
        "post-guard include reopened the exact MergePrepared callback: "
        f"{late_merge_header_checks.errors}",
    )

    missing_merge_macro_guard_checks = Checks(Path("."))
    missing_merge_macro_guard_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE + "\n\n",
            "",
            1,
        ),
    )
    expect(
        any(
            "must contain exactly one exact MergePrepared preprocessor macro guard"
            in error
            for error in missing_merge_macro_guard_checks.errors
        ),
        "missing MergePrepared preprocessor guard escaped closure: "
        f"{missing_merge_macro_guard_checks.errors}",
    )

    altered_merge_macro_guard_checks = Checks(Path("."))
    altered_merge_macro_guard_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "    defined(validate_merge_prepared_envelope) || \\\n",
            "",
            1,
        ),
    )
    expect(
        any(
            "must contain exactly one exact MergePrepared preprocessor macro guard"
            in error
            for error in altered_merge_macro_guard_checks.errors
        ),
        "incomplete MergePrepared preprocessor guard escaped closure: "
        f"{altered_merge_macro_guard_checks.errors}",
    )

    inactive_merge_callback = valid_private_handoff_resume_wave_store.replace(
        PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE,
        '#include "merge_callback_factory.hpp"\n#if 0\n'
        + PRIVATE_HANDOFF_PUBLICATION_MERGE_MACRO_GUARD_SOURCE,
        1,
    ).replace(
        "\n}\nauto classify_merge_prepared_publication_prefix_v1() noexcept {",
        "\n}\n#else\nGNFS_MERGE_CALLBACK_FACTORY\n#endif\n"
        "auto classify_merge_prepared_publication_prefix_v1() noexcept {",
        1,
    )
    inactive_merge_callback_checks = Checks(Path("."))
    inactive_merge_callback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        inactive_merge_callback,
    )
    expect(
        any(
            "must be in the exact active POSIX preprocessing scope" in error
            or "must contain no preprocessing directives" in error
            for error in inactive_merge_callback_checks.errors
        ),
        "inactive canonical MergePrepared callback plus macro-generated "
        "alternative escaped preprocessing-scope closure: "
        f"{inactive_merge_callback_checks.errors}",
    )

    redirected_closed_binding_checks = Checks(Path("."))
    redirected_closed_binding_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "&validate_merge_prepared_envelope;",
            "&forged_merge_prepared_envelope;",
            1,
        ),
    )
    expect(
        any(
            "must retain the exact closed function-pointer bindings" in error
            for error in redirected_closed_binding_checks.errors
        ),
        "MergePrepared callback accepted a redirected closed function pointer: "
        f"{redirected_closed_binding_checks.errors}",
    )

    forged_merge_observed_checks = Checks(Path("."))
    forged_merge_observed_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """            std::move(*admission.observed),
            WorkerHandoffTypedValidatorAuthorityV1::bind(
                validate_merge_prepared_prefix_type, &typed_context));""",
            """            std::move(*forged.observed),
            WorkerHandoffTypedValidatorAuthorityV1::bind(
                validate_merge_prepared_prefix_type, &typed_context));""",
        ),
    )
    expect(
        any(
            "must consume admission.observed through the exact MergePrepared "
            "context"
            in error
            for error in forged_merge_observed_checks.errors
        ),
        "forged MergePrepared observed permit escaped classifier dataflow "
        f"closure: {forged_merge_observed_checks.errors}",
    )

    forged_merge_validated_checks = Checks(Path("."))
    forged_merge_validated_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """    const auto revalidated =
        private_lease::revalidate_private_handoff_publication_resume_v1(
            *validation.permit);""",
            """    const auto revalidated =
        private_lease::revalidate_private_handoff_publication_resume_v1(
            *forged.permit);""",
        ),
    )
    expect(
        any(
            "must revalidate exactly validation.permit" in error
            for error in forged_merge_validated_checks.errors
        ),
        "forged MergePrepared validated permit escaped classifier dataflow "
        f"closure: {forged_merge_validated_checks.errors}",
    )

    weakened_merge_typed_callback_checks = Checks(Path("."))
    weakened_merge_typed_callback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "context->typed_prepared=std::move(*typed.witness);returntrue;",
            "returntrue;",
            1,
        ),
    )
    expect(
        any(
            "must retain every exact typed guard, dependency binding, and "
            "witness-transfer fragment"
            in error
            for error in weakened_merge_typed_callback_checks.errors
        ),
        "unconditional MergePrepared typed success escaped callback dataflow "
        f"closure: {weakened_merge_typed_callback_checks.errors}",
    )

    conditional_merge_permit_chain = valid_private_handoff_resume_wave_store.replace(
        """    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, relation_identity(expected_directory_identity));
    MergePreparedTypedValidationContext typed_context{""",
        """    if (false) {
    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, relation_identity(expected_directory_identity));
    MergePreparedTypedValidationContext typed_context{""",
    ).replace(
        """    return revalidated;
}
auto revalidate_exact_canonical_merge_started() noexcept {""",
        """    }
    return revalidated;
}
auto revalidate_exact_canonical_merge_started() noexcept {""",
    )
    conditional_merge_permit_checks = Checks(Path("."))
    conditional_merge_permit_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        conditional_merge_permit_chain,
    )
    expect(
        any(
            "forbids if control around the retained permit chain" in error
            for error in conditional_merge_permit_checks.errors
        ),
        "conditionally unreachable MergePrepared permit chain escaped lexical "
        f"closure: {conditional_merge_permit_checks.errors}",
    )

    looped_merge_permit_chain = valid_private_handoff_resume_wave_store.replace(
        """    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, relation_identity(expected_directory_identity));
    MergePreparedTypedValidationContext typed_context{""",
        """    for (; false;) {
    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, relation_identity(expected_directory_identity));
    MergePreparedTypedValidationContext typed_context{""",
    ).replace(
        """    return revalidated;
}
auto revalidate_exact_canonical_merge_started() noexcept {""",
        """    }
    return revalidated;
}
auto revalidate_exact_canonical_merge_started() noexcept {""",
    )
    looped_merge_permit_checks = Checks(Path("."))
    looped_merge_permit_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        looped_merge_permit_chain,
    )
    expect(
        any(
            "forbids for control around the retained permit chain" in error
            for error in looped_merge_permit_checks.errors
        ),
        "zero-iteration MergePrepared permit chain escaped lexical closure: "
        f"{looped_merge_permit_checks.errors}",
    )

    spliced_merge_typed_callback_checks = Checks(Path("."))
    spliced_merge_typed_callback_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "context->typed_prepared=std::move(*typed.witness);returntrue;",
            "typed.witness->prepared.merge_started_digest={};"
            "context->typed_prepared=std::move(*typed.witness);returntrue;",
            1,
        ),
    )
    expect(
        any(
            "must remain the exact fail-closed callback body" in error
            for error in spliced_merge_typed_callback_checks.errors
        ),
        "interposed MergePrepared witness mutation escaped exact callback "
        f"closure: {spliced_merge_typed_callback_checks.errors}",
    )

    unmirrored_private_handoff_resume_checks = Checks(Path("."))
    unmirrored_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "private_lease::PrivateHandoffPublicationResumeObservationPointV1::"
            "AfterPendingRollbackLeaseDirectoryRemovedDurable));",
            "private_lease::PrivateHandoffPublicationResumeObservationPointV1::"
            "AfterPendingRollbackOwnedRemovedDurable));",
        ),
    )
    expect(
        any(
            "must statically bind exact ordinal "
            "AfterPendingRollbackLeaseDirectoryRemovedDurable"
            in error
            for error in unmirrored_private_handoff_resume_checks.errors
        ),
        "WaveStore/relation observation ordinal drift escaped static mirror closure: "
        f"{unmirrored_private_handoff_resume_checks.errors}",
    )

    early_private_handoff_bridge_return_checks = Checks(Path("."))
    early_private_handoff_bridge_return_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """    if (revalidated.status == DistributedSieveWaveStoreStatus::ready) {
        return user_requested_stop;
    }
    context->revalidation_failed = true;
""",
            """    return user_requested_stop;
    context->revalidation_failed = true;
""",
        ),
    )
    expect(
        any(
            "bridge_worker_handoff_resume_observation must remain the exact "
            "fail-closed observation, revalidation, and relation-hook bridge"
            in error
            for error in early_private_handoff_bridge_return_checks.errors
        ),
        "user stop result before complete WaveStore authority revalidation escaped "
        f"bridge closure: {early_private_handoff_bridge_return_checks.errors}",
    )

    missing_merge_resume_bridge_checks = Checks(Path("."))
    missing_merge_resume_bridge_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "[[nodiscard]] bool bridge_merge_prepared_resume_observation(",
            "[[nodiscard]] bool removed_merge_prepared_resume_observation(",
            1,
        ),
    )
    expect(
        any(
            "bridge_merge_prepared_resume_observation must remain the exact "
            "stop/fail hook-first bridge"
            in error
            for error in missing_merge_resume_bridge_checks.errors
        ),
        "missing MergePrepared resume bridge escaped exact bridge closure: "
        f"{missing_merge_resume_bridge_checks.errors}",
    )

    bypassed_merge_resume_projection_checks = Checks(Path("."))
    bypassed_merge_resume_projection_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "revalidated = revalidate_merge_prepared_aggregate_projection(",
            "revalidated = bypass_merge_prepared_aggregate_projection(",
            1,
        ),
    )
    expect(
        any(
            "bridge_merge_prepared_resume_observation must remain the exact "
            "stop/fail hook-first bridge"
            in error
            for error in bypassed_merge_resume_projection_checks.errors
        ),
        "MergePrepared resume bridge bypassed aggregate projection revalidation: "
        f"{bypassed_merge_resume_projection_checks.errors}",
    )

    unstable_retained_worker_order_checks = Checks(Path("."))
    unstable_retained_worker_order_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "left_attempt.manifest_chunk_order",
            "left.attempt_index",
            1,
        ),
    )
    expect(
        any(
            "must sort worker predecessors by stable manifest/attempt order"
            in error
            for error in unstable_retained_worker_order_checks.errors
        ),
        "unstable retained-worker acquisition order escaped classifier closure: "
        f"{unstable_retained_worker_order_checks.errors}",
    )

    bypassed_retained_worker_permit_checks = Checks(Path("."))
    bypassed_retained_worker_permit_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "retained_worker->permit);",
            "forged_worker_permit);",
            1,
        ),
    )
    expect(
        any(
            "must exact-revalidate only already-retained worker permits"
            in error
            for error in bypassed_retained_worker_permit_checks.errors
        ),
        "aggregate bridge projection bypassed the retained worker permit: "
        f"{bypassed_retained_worker_permit_checks.errors}",
    )

    unguarded_merge_inventory_copy_checks = Checks(Path("."))
    unguarded_merge_inventory_copy_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """try {
    auto expected_inventory = expected.inventory;
    auto current_inventory = *current.inventory;""",
            """auto expected_inventory = expected.inventory;
try {
    auto current_inventory = *current.inventory;""",
            1,
        ),
    )
    expect(
        any(
            "must keep throwing inventory copy/projection inside the exact "
            "noexcept exception boundary"
            in error
            for error in unguarded_merge_inventory_copy_checks.errors
        ),
        "throwing MergePrepared inventory copy escaped the noexcept exception "
        f"boundary: {unguarded_merge_inventory_copy_checks.errors}",
    )

    adopted_unretained_canonical_worker_checks = Checks(Path("."))
    adopted_unretained_canonical_worker_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """    if (canonical_handoff_root_shape ||
        expected.private_leases[index].worker_handoff.has_value() ||""",
            """    if (expected.private_leases[index].worker_handoff.has_value() ||""",
            1,
        ),
    )
    expect(
        any(
            "reject every unretained canonical handoff before path validation"
            in error
            for error in adopted_unretained_canonical_worker_checks.errors
        ),
        "unretained canonical worker escaped fail-closed aggregate projection: "
        f"{adopted_unretained_canonical_worker_checks.errors}",
    )

    forward_retained_worker_release_checks = Checks(Path("."))
    forward_retained_worker_release_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "entries.pop_back();",
            "entries.erase(entries.begin());",
            1,
        ),
    )
    expect(
        any(
            "must keep the exact reverse-order stack" in error
            for error in forward_retained_worker_release_checks.errors
        ),
        "forward retained-worker permit release escaped LIFO resource closure: "
        f"{forward_retained_worker_release_checks.errors}",
    )

    early_retained_worker_stack_release_checks = Checks(Path("."))
    early_retained_worker_stack_release_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "retained.consumed=true;"
            "merge_prepared_prefix.retained.reset();"
            "merge_prepared_prefix.witness.reset();",
            "retained.consumed=true;"
            "merge_prepared_prefix.witness.reset();"
            "merge_prepared_prefix.retained.reset();",
            1,
        ),
    )
    expect(
        any(
            "must reset the consumed target permit before resetting the "
            "aggregate worker stack"
            in error
            for error in early_retained_worker_stack_release_checks.errors
        ),
        "aggregate worker stack released before target permit reset: "
        f"{early_retained_worker_stack_release_checks.errors}",
    )

    early_private_handoff_round_release_checks = Checks(Path("."))
    early_private_handoff_round_release_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """    while (!recoverable.retained.entries.empty()) {
        recoverable.retained.entries.pop_back();
    }
    if (hooks.worker_handoff_resume.after_round_locks_released != nullptr) {
        hooks.worker_handoff_resume.after_round_locks_released(
            hooks.worker_handoff_resume.context);
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }
    }
""",
            """    if (hooks.worker_handoff_resume.after_round_locks_released != nullptr) {
        hooks.worker_handoff_resume.after_round_locks_released(
            hooks.worker_handoff_resume.context);
        if (!process_matches(creator_process_id)) {
            return open_failure(process_mismatch());
        }
    }
    while (!recoverable.retained.entries.empty()) {
        recoverable.retained.entries.pop_back();
    }
""",
        ),
    )
    expect(
        any(
            "must invoke the test-only round-release hook exactly once after "
            "the retained LIFO stack is empty"
            in error
            for error in early_private_handoff_round_release_checks.errors
        ),
        "round-release test seam before LIFO permit release escaped ordering gate: "
        f"{early_private_handoff_round_release_checks.errors}",
    )

    unchecked_private_handoff_resume_wave_store = (
        valid_private_handoff_resume_wave_store.replace(
            """    if (!typed) {
        context->diagnostic = std::move(typed.diagnostic);
        return false;
    }
""",
            """    if (!typed) {
        context->diagnostic = std::move(typed.diagnostic);
    }
""",
        )
    )
    unchecked_private_handoff_resume_checks = Checks(Path("."))
    unchecked_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        unchecked_private_handoff_resume_wave_store,
    )
    expect(
        any(
            "must keep the exact typed failure guard" in error
            for error in unchecked_private_handoff_resume_checks.errors
        ),
        "unchecked WaveStore typed handoff validation escaped the dominance gate: "
        f"{unchecked_private_handoff_resume_checks.errors}",
    )

    forged_private_handoff_validator_checks = Checks(Path("."))
    forged_private_handoff_validator_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """WorkerHandoffTypedValidatorAuthorityV1::bind(
                validate_worker_handoff_prefix_type, &typed_context)""",
            "std::move(forged_validator)",
        ),
    )
    expect(
        any(
            "must pass the exact empty typed context through the sole worker "
            "validator authority bind"
            in error
            for error in forged_private_handoff_validator_checks.errors
        ),
        "forged relation typed-validator argument escaped the capability dataflow "
        f"gate: {forged_private_handoff_validator_checks.errors}",
    )

    forged_private_handoff_receipt_checks = Checks(Path("."))
    forged_private_handoff_receipt_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            "    context->typed_handoff = std::move(*typed.witness);\n",
            "    context->typed_handoff = forged_handoff;\n",
        ),
    )
    expect(
        any(
            "must keep the exact typed failure guard, AttemptStarted binding, "
            "witness transfer"
            in error
            for error in forged_private_handoff_receipt_checks.errors
        ),
        "forged worker-handoff typed receipt escaped callback dataflow closure: "
        f"{forged_private_handoff_receipt_checks.errors}",
    )

    unchecked_private_handoff_success_checks = Checks(Path("."))
    unchecked_private_handoff_success_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store.replace(
            """    if (!validation.validated() || !validation.permit.has_value() ||
        !typed_context.typed_handoff.has_value()) {
        if (typed_context.diagnostic.status !=
            DistributedSieveWaveStoreStatus::ready) {
            return fail_with(std::move(typed_context.diagnostic));
        }
        return fail_with(worker_handoff_inspection_failure(validation.result));
    }
""",
            "",
        ),
    )
    expect(
        any(
            "must explicitly reject missing validated permit or typed witness"
            in error
            for error in unchecked_private_handoff_success_checks.errors
        ),
        "unchecked relation permit/typed witness success escaped the dominance gate: "
        f"{unchecked_private_handoff_success_checks.errors}",
    )

    lambda_private_handoff_resume_wave_store = (
        private_handoff_resume_wave_authority_and_callback
        + "\nauto capture_recoverable_worker_handoff_inventory() noexcept {\n"
        + "    auto deferred = [&] {\n"
        + private_handoff_resume_capture_chain
        + "    };\n    return deferred();\n}\n"
        + private_handoff_resume_wave_store_suffix
    )
    lambda_private_handoff_resume_checks = Checks(Path("."))
    lambda_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        lambda_private_handoff_resume_wave_store,
    )
    expect(
        any(
            "forbids lambda control around the resume authority chain" in error
            for error in lambda_private_handoff_resume_checks.errors
        ),
        "lambda-deferred WaveStore resume authority chain escaped the control-scope "
        f"gate: {lambda_private_handoff_resume_checks.errors}",
    )

    unreachable_private_handoff_resume_wave_store = (
        private_handoff_resume_wave_authority_and_callback
        + "\nauto capture_recoverable_worker_handoff_inventory() noexcept {\n"
        + "    if (false) {\n"
        + private_handoff_resume_capture_chain
        + "    }\n    return conflict();\n}\n"
        + private_handoff_resume_wave_store_suffix
    )
    unreachable_private_handoff_resume_checks = Checks(Path("."))
    unreachable_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        unreachable_private_handoff_resume_wave_store,
    )
    expect(
        any(
            "forbids if control around the resume authority chain" in error
            for error in unreachable_private_handoff_resume_checks.errors
        ),
        "unreachable conditional WaveStore resume authority chain escaped the "
        f"control-scope gate: {unreachable_private_handoff_resume_checks.errors}",
    )

    for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS:
        expected = PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_DIRECT_CALL_COUNTS[
            identifier
        ]
        duplicate_private_handoff_resume_checks = Checks(Path("."))
        duplicate_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
            PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
            valid_private_handoff_resume_wave_store
            + f"\nauto duplicate = private_lease::{identifier}(arguments);\n",
        )
        expect(
            any(
                f"exactly {expected} direct {identifier} call" in error
                and f"found {expected + 1} identifiers and {expected + 1} calls"
                in error
                for error in duplicate_private_handoff_resume_checks.errors
            ),
            f"duplicate WaveStore {identifier} call escaped count closure: "
            f"{duplicate_private_handoff_resume_checks.errors}",
        )

    third_merge_reconcile_checks = Checks(Path("."))
    third_merge_reconcile_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        valid_private_handoff_resume_wave_store
        + "\nauto third_reconcile = private_lease::"
        + PRIVATE_HANDOFF_PUBLICATION_RESUME_RECONCILE_IDENTIFIER
        + "(permit, hooks);\n",
    )
    expect(
        any(
            "production WaveStore must contain exactly 3 direct "
            "reconcile_private_handoff_publication_for_resume_v1 call, "
            "found 4 identifiers and 4 calls"
            in error
            for error in third_merge_reconcile_checks.errors
        ),
        "fourth WaveStore resume reconcile call escaped exact three-call closure: "
        f"{third_merge_reconcile_checks.errors}",
    )

    for identifier in PRIVATE_HANDOFF_PUBLICATION_RESUME_DIRECT_CALL_IDENTIFIERS:
        indirect_private_handoff_resume_checks = Checks(Path("."))
        indirect_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
            PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
            valid_private_handoff_resume_wave_store
            + f"\nauto escaped = &private_lease::{identifier};\n",
        )
        expect(
            any(
                f"{identifier} must be used only as a direct call" in error
                and "function-pointer references are forbidden" in error
                for error in indirect_private_handoff_resume_checks.errors
            ),
            f"indirect WaveStore {identifier} authority escaped the direct-call "
            f"rule: {indirect_private_handoff_resume_checks.errors}",
        )

    reordered_private_handoff_resume_wave_store = (
        valid_private_handoff_resume_wave_store.replace(
            """    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, directory_identity);
    WorkerHandoffTypedValidationContext typed_context{""",
            """    WorkerHandoffTypedValidationContext typed_context{""",
        ).replace(
            """    auto validation =
        private_lease::validate_private_handoff_publication_resume_v1(""",
            """    auto admission =
        private_lease::acquire_private_handoff_publication_resume_v1(
            paths, directory_identity);
    auto validation =
        private_lease::validate_private_handoff_publication_resume_v1(""",
        )
    )
    reordered_private_handoff_resume_checks = Checks(Path("."))
    reordered_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        reordered_private_handoff_resume_wave_store,
    )
    expect(
        any(
            "must order resume acquisition, typed-validator context binding, and "
            "relation validation"
            in error
            for error in reordered_private_handoff_resume_checks.errors
        ),
        "WaveStore typed-validator context before resume acquisition was not "
        f"rejected: {reordered_private_handoff_resume_checks.errors}",
    )

    reconcile_outside_open_private_handoff_resume_wave_store = (
        private_handoff_resume_wave_authority_and_callback
        + "\nauto capture_recoverable_worker_handoff_inventory() noexcept {\n"
        + private_handoff_resume_capture_chain
        + "}\n"
        + r"""
auto revalidate_held_private_handoff() noexcept {
    return private_lease::revalidate_private_handoff_publication_resume_v1(permit);
}
auto DistributedSieveWaveStore::open() noexcept {
    return ready;
}
auto reconcile_helper() noexcept {
    auto first = private_lease::reconcile_private_handoff_publication_for_resume_v1(
        first_permit, hooks);
    auto second = private_lease::reconcile_private_handoff_publication_for_resume_v1(
        second_permit, hooks);
    return private_lease::reconcile_private_handoff_publication_for_resume_v1(
        permit, hooks);
}
"""
    )
    reconcile_outside_open_private_handoff_resume_checks = Checks(Path("."))
    reconcile_outside_open_private_handoff_resume_checks.validate_private_handoff_publication_resume_boundary(
        PRIVATE_HANDOFF_PUBLICATION_RESUME_WAVE_STORE_FILE,
        reconcile_outside_open_private_handoff_resume_wave_store,
    )
    expect(
        any(
            f"{PRIVATE_HANDOFF_PUBLICATION_RESUME_OPEN_FUNCTION} must contain "
            "all three direct "
            "reconcile_private_handoff_publication_for_resume_v1 call"
            in error
            for error in reconcile_outside_open_private_handoff_resume_checks.errors
        ),
        "WaveStore reconcile authority outside open escaped the body boundary: "
        f"{reconcile_outside_open_private_handoff_resume_checks.errors}",
    )

    merge_coordinator_use_site_snippet = r"""
#include "distributed_sieve_merge_coordinator.hpp"
auto admission = begin_or_resume_distributed_sieve_merge_generation_v1(
    std::move(worker_result));
"""
    untrusted_merge_coordinator_checks = Checks(Path("."))
    untrusted_merge_coordinator_checks.validate_merge_coordinator_use_site(
        "src/sieve/untrusted_merge_coordinator.cpp",
        merge_coordinator_use_site_snippet,
    )
    expect(
        any(
            "source-private merge-coordinator header include is not allowlisted"
            in error
            for error in untrusted_merge_coordinator_checks.errors
        )
        and any(
            "source-private merge-coordinator use site is not allowlisted"
            in error
            for error in untrusted_merge_coordinator_checks.errors
        ),
        "merge-coordinator repo-wide source-private use-site gate is not enforced: "
        f"{untrusted_merge_coordinator_checks.errors}",
    )

    public_merge_coordinator_checks = Checks(Path("."))
    public_merge_coordinator_checks.validate_merge_coordinator_use_site(
        "include/gnfs/sieve/merge_coordinator_leak.hpp",
        merge_coordinator_use_site_snippet,
    )
    expect(
        any(
            "merge-coordinator header leaked into a public sieve header" in error
            for error in public_merge_coordinator_checks.errors
        )
        and any(
            "merge-coordinator API leaked into a public sieve header" in error
            for error in public_merge_coordinator_checks.errors
        ),
        "source-private merge-coordinator public-header leak was not rejected: "
        f"{public_merge_coordinator_checks.errors}",
    )

    for relative in sorted(MERGE_COORDINATOR_USE_SITE_ALLOWLIST):
        allowed_merge_coordinator_checks = Checks(Path("."))
        allowed_merge_coordinator_checks.validate_merge_coordinator_use_site(
            relative, merge_coordinator_use_site_snippet
        )
        expect(
            not allowed_merge_coordinator_checks.errors,
            f"allowlisted merge-coordinator use was rejected in {relative}: "
            f"{allowed_merge_coordinator_checks.errors}",
        )

    merge_authority_use_site_snippet = r"""
auto reservation = reserve_distributed_sieve_merge_generation_v1(
    *worker_result.store, ordinal, terminal_inputs);
auto started = publish_merge_started_v1(
    std::move(reservation), terminal_inputs, merge_policy);
"""
    untrusted_merge_authority_checks = Checks(Path("."))
    untrusted_merge_authority_checks.validate_merge_generation_authority_use_site(
        "src/sieve/untrusted_merge_authority.cpp",
        merge_authority_use_site_snippet,
    )
    expect(
        len(untrusted_merge_authority_checks.errors) == 2
        and all(
            "merge-generation reserve/publish authority use site is not allowlisted"
            in error
            for error in untrusted_merge_authority_checks.errors
        ),
        "merge reserve/publish authority escaped its exact use-site allowlist: "
        f"{untrusted_merge_authority_checks.errors}",
    )
    for relative in sorted(MERGE_GENERATION_AUTHORITY_USE_SITE_ALLOWLIST):
        allowed_merge_authority_checks = Checks(Path("."))
        allowed_merge_authority_checks.validate_merge_generation_authority_use_site(
            relative, merge_authority_use_site_snippet
        )
        expect(
            not allowed_merge_authority_checks.errors,
            f"allowlisted merge-generation authority use was rejected in {relative}: "
            f"{allowed_merge_authority_checks.errors}",
        )

    valid_merge_coordinator_interface = r"""
class DistributedSieveMergeGenerationAdmissionV1 final {
public:
    DistributedSieveMergeGenerationAdmissionV1(
        const DistributedSieveMergeGenerationAdmissionV1&) = delete;
    DistributedSieveMergeGenerationAdmissionV1&
    operator=(const DistributedSieveMergeGenerationAdmissionV1&) = delete;
    DistributedSieveMergeGenerationAdmissionV1(
        DistributedSieveMergeGenerationAdmissionV1&&) noexcept = default;
    DistributedSieveMergeGenerationAdmissionV1&
    operator=(DistributedSieveMergeGenerationAdmissionV1&&) = delete;

private:
    explicit DistributedSieveMergeGenerationAdmissionV1(
        DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept;
    DistributedSieveWorkerCoordinatorResultV1 worker_result_;

    friend DistributedSieveMergeGenerationAdmissionV1
    begin_or_resume_distributed_sieve_merge_generation_v1(
        DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept;
};

[[nodiscard]] DistributedSieveMergeGenerationAdmissionV1
begin_or_resume_distributed_sieve_merge_generation_v1(
    DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept;
"""
    valid_merge_interface_checks = Checks(Path("."))
    valid_merge_interface_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_INTERFACE_FILE, valid_merge_coordinator_interface
    )
    expect(
        not valid_merge_interface_checks.errors,
        "valid source-private move-only merge interface was rejected: "
        f"{valid_merge_interface_checks.errors}",
    )

    copyable_merge_interface = valid_merge_coordinator_interface.replace(
        "const DistributedSieveMergeGenerationAdmissionV1&) = delete;",
        "const DistributedSieveMergeGenerationAdmissionV1&) = default;",
        1,
    )
    copyable_merge_interface_checks = Checks(Path("."))
    copyable_merge_interface_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_INTERFACE_FILE, copyable_merge_interface
    )
    expect(
        any(
            "merge admission must be a one-owner move-only lifetime root" in error
            for error in copyable_merge_interface_checks.errors
        ),
        "copyable merge admission was not rejected: "
        f"{copyable_merge_interface_checks.errors}",
    )

    nonretaining_merge_interface = valid_merge_coordinator_interface.replace(
        "    DistributedSieveWorkerCoordinatorResultV1 worker_result_;\n",
        "",
    )
    nonretaining_merge_interface_checks = Checks(Path("."))
    nonretaining_merge_interface_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_INTERFACE_FILE, nonretaining_merge_interface
    )
    expect(
        any(
            "must privately consume, retain, and friend the complete worker "
            "coordinator result"
            in error
            for error in nonretaining_merge_interface_checks.errors
        ),
        "merge admission without whole-result storage was not rejected: "
        f"{nonretaining_merge_interface_checks.errors}",
    )

    lvalue_merge_interface = valid_merge_coordinator_interface.replace(
        "DistributedSieveWorkerCoordinatorResultV1&& worker_result",
        "DistributedSieveWorkerCoordinatorResultV1& worker_result",
    )
    lvalue_merge_interface_checks = Checks(Path("."))
    lvalue_merge_interface_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_INTERFACE_FILE, lvalue_merge_interface
    )
    expect(
        any(
            "must consume the complete DistributedSieveWorkerCoordinatorResultV1&&"
            in error
            for error in lvalue_merge_interface_checks.errors
        ),
        "merge interface accepted a partial/lvalue worker result: "
        f"{lvalue_merge_interface_checks.errors}",
    )

    duplicate_worker_result_consumer_interface = (
        valid_merge_coordinator_interface
        + "\nvoid consume_elsewhere("
        "DistributedSieveWorkerCoordinatorResultV1&& worker_result);\n"
    )
    duplicate_worker_result_consumer_checks = Checks(Path("."))
    duplicate_worker_result_consumer_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_INTERFACE_FILE,
        duplicate_worker_result_consumer_interface,
    )
    expect(
        any(
            "only the private admission constructor/member/friend and the "
            "merge entry declaration may name the complete worker result"
            in error
            for error in duplicate_worker_result_consumer_checks.errors
        ),
        "a second complete worker-result consumer escaped the sole entry boundary: "
        f"{duplicate_worker_result_consumer_checks.errors}",
    )

    valid_merge_coordinator_composition = r"""
DistributedSieveMergeGenerationAdmissionV1
begin_or_resume_distributed_sieve_merge_generation_v1(
    DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept {
    std::size_t duplicates_removed = 0;
    (void)duplicates_removed;
    auto reservation = reserve_distributed_sieve_merge_generation_v1(
        *worker_result.store, ordinal, terminal_inputs, merge_policy);
    auto started = publish_merge_started_v1(
        std::move(*reservation.receipt), terminal_inputs, merge_policy);
    return DistributedSieveMergeGenerationAdmissionV1(
        std::move(worker_result), std::move(started));
}
"""
    valid_merge_composition_checks = Checks(Path("."))
    valid_merge_composition_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE,
        valid_merge_coordinator_composition,
    )
    expect(
        not valid_merge_composition_checks.errors,
        "valid reserve-before-publish whole-result merge composition was rejected: "
        f"{valid_merge_composition_checks.errors}",
    )

    valid_out_of_line_merge_admission = (
        r"""
DistributedSieveMergeGenerationAdmissionV1::
DistributedSieveMergeGenerationAdmissionV1(
    DistributedSieveWorkerCoordinatorResultV1&& worker_result) noexcept
    : worker_result_(std::move(worker_result)) {}
"""
        + valid_merge_coordinator_composition
    )
    valid_out_of_line_merge_admission_checks = Checks(Path("."))
    valid_out_of_line_merge_admission_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE,
        valid_out_of_line_merge_admission,
    )
    expect(
        not valid_out_of_line_merge_admission_checks.errors,
        "private admission constructor/member whole-result retention was rejected: "
        f"{valid_out_of_line_merge_admission_checks.errors}",
    )

    extra_merge_result_consumer = (
        valid_merge_coordinator_composition
        + "\nvoid consume_elsewhere("
        "DistributedSieveWorkerCoordinatorResultV1&& other_result) {}\n"
    )
    extra_merge_result_consumer_checks = Checks(Path("."))
    extra_merge_result_consumer_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE, extra_merge_result_consumer
    )
    expect(
        any(
            "only the merge entry and the optional private admission constructor "
            "definition may name the complete worker result"
            in error
            for error in extra_merge_result_consumer_checks.errors
        ),
        "a second implementation-side worker-result consumer escaped the gate: "
        f"{extra_merge_result_consumer_checks.errors}",
    )

    reordered_merge_composition = valid_merge_coordinator_composition.replace(
        "    auto reservation = reserve_distributed_sieve_merge_generation_v1(\n"
        "        *worker_result.store, ordinal, terminal_inputs, merge_policy);\n"
        "    auto started = publish_merge_started_v1(\n"
        "        std::move(*reservation.receipt), terminal_inputs, merge_policy);\n",
        "    auto started = publish_merge_started_v1(\n"
        "        std::move(*reservation.receipt), terminal_inputs, merge_policy);\n"
        "    auto reservation = reserve_distributed_sieve_merge_generation_v1(\n"
        "        *worker_result.store, ordinal, terminal_inputs, merge_policy);\n",
    )
    reordered_merge_composition_checks = Checks(Path("."))
    reordered_merge_composition_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE, reordered_merge_composition
    )
    expect(
        any(
            "must reserve the exact merged generation before publishing MergeStarted"
            in error
            for error in reordered_merge_composition_checks.errors
        )
        and any(
            "must order reservation, MergeStarted publication, then whole-result"
            in error
            for error in reordered_merge_composition_checks.errors
        ),
        "merge coordinator accepted publish-before-reserve ordering: "
        f"{reordered_merge_composition_checks.errors}",
    )

    split_worker_result_composition = valid_merge_coordinator_composition.replace(
        "    return DistributedSieveMergeGenerationAdmissionV1(\n",
        "    auto detached_store = std::move(worker_result.store);\n"
        "    return DistributedSieveMergeGenerationAdmissionV1(\n",
    )
    split_worker_result_checks = Checks(Path("."))
    split_worker_result_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE, split_worker_result_composition
    )
    expect(
        any(
            "must not split-move store, claims, chunks, or diagnostics" in error
            for error in split_worker_result_checks.errors
        ),
        "merge coordinator accepted extraction from the complete worker result: "
        f"{split_worker_result_checks.errors}",
    )

    forbidden_merge_coordinator_composition = (
        valid_merge_coordinator_composition.replace(
            "    return DistributedSieveMergeGenerationAdmissionV1(\n",
            "    std::filesystem::path artifact_path;\n"
            "    unlink(raw_leaf);\n"
            "    remove_all(artifact_path);\n"
            "    remove_worker_artifacts();\n"
            "    cleanup_worker_handoff();\n"
            "    arm_cleanup_authority();\n"
            "    MergeStartedV1 caller_built{};\n"
            "    return DistributedSieveMergeGenerationAdmissionV1(\n",
        )
    )
    forbidden_merge_coordinator_checks = Checks(Path("."))
    forbidden_merge_coordinator_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE,
        forbidden_merge_coordinator_composition,
    )
    expect(
        all(
            any(identifier in error for error in forbidden_merge_coordinator_checks.errors)
            for identifier in (
                "filesystem",
                "path",
                "unlink",
                "remove_all",
                "remove_worker_artifacts",
                "cleanup_worker_handoff",
                "arm_cleanup_authority",
                "MergeStartedV1",
            )
        ),
        "merge coordinator filesystem, removal, cleanup/arm, or caller-record bans "
        f"are not closed: {forbidden_merge_coordinator_checks.errors}",
    )

    aliased_merge_reserve = valid_merge_coordinator_composition.replace(
        "    auto reservation = reserve_distributed_sieve_merge_generation_v1(\n",
        "    auto reserve = reserve_distributed_sieve_merge_generation_v1;\n"
        "    auto reservation = reserve(\n",
    )
    aliased_merge_reserve_checks = Checks(Path("."))
    aliased_merge_reserve_checks.validate_merge_coordinator_boundary(
        MERGE_COORDINATOR_IMPLEMENTATION_FILE, aliased_merge_reserve
    )
    expect(
        any(
            "must be used only as a direct call" in error
            for error in aliased_merge_reserve_checks.errors
        ),
        "merge coordinator accepted an aliased reserve authority: "
        f"{aliased_merge_reserve_checks.errors}",
    )

    expect(
        MERGE_COORDINATOR_PRODUCTION_FILES
        == {
            "src/sieve/distributed_sieve_merge_coordinator.cpp",
            "src/sieve/distributed_sieve_merge_coordinator.hpp",
        }
        and MERGE_COORDINATOR_USE_SITE_ALLOWLIST
        == (
            MERGE_COORDINATOR_PRODUCTION_FILES
            | MERGE_WRITER_AUTHORITY_PRODUCTION_FILES
            | {
                "tests/test_distributed_sieve_resume.cpp",
                "tests/test_distributed_sieve_merge_writer_authority.cpp",
            }
        )
        and (
            MERGE_COORDINATOR_PRODUCTION_FILES
            | MERGE_WRITER_AUTHORITY_PRODUCTION_FILES
        )
        <= DURABLE_ENVIRONMENT_FREE_FILES
        and MERGE_COORDINATOR_INTERFACE_FILE
        not in MERGE_GENERATION_AUTHORITY_USE_SITE_ALLOWLIST,
        "merge-coordinator inventory is not the exact source-private "
        "implementation, interface, test, and lower-authority boundary",
    )

    coordinator_use_site_snippet = r"""
DistributedSieveWorkerCoordinatorRequestV1 request;
auto result = coordinate_missing_distributed_sieve_workers_v1(
    std::move(request), identity, frozen, polynomial, factor_base);
"""
    untrusted_coordinator_checks = Checks(Path("."))
    untrusted_coordinator_checks.validate_worker_coordinator_use_site(
        "src/sieve/untrusted_coordinator.cpp", coordinator_use_site_snippet
    )
    expect(
        untrusted_coordinator_checks.errors
        and all(
            "source-private worker-coordinator use site is not allowlisted" in error
            for error in untrusted_coordinator_checks.errors
        ),
        "worker-coordinator repo-wide use-site gate is not enforced: "
        f"{untrusted_coordinator_checks.errors}",
    )

    public_coordinator_header_checks = Checks(Path("."))
    public_coordinator_header_checks.validate_worker_coordinator_use_site(
        "include/gnfs/sieve/coordinator_leak.hpp",
        '#include "distributed_sieve_worker_coordinator_internal.hpp"\n'
        + coordinator_use_site_snippet,
    )
    expect(
        any(
            "worker-coordinator header leaked into a public sieve header" in error
            for error in public_coordinator_header_checks.errors
        )
        and any(
            "worker-coordinator API leaked into a public sieve header" in error
            for error in public_coordinator_header_checks.errors
        ),
        "source-private worker-coordinator public-header leak was not rejected: "
        f"{public_coordinator_header_checks.errors}",
    )

    for relative in sorted(WORKER_COORDINATOR_USE_SITE_ALLOWLIST):
        allowed_coordinator_checks = Checks(Path("."))
        allowed_coordinator_checks.validate_worker_coordinator_use_site(
            relative, coordinator_use_site_snippet
        )
        expect(
            not allowed_coordinator_checks.errors,
            f"allowlisted worker-coordinator use was rejected in {relative}: "
            f"{allowed_coordinator_checks.errors}",
        )

    expect(
        WORKER_COORDINATOR_PRODUCTION_FILES
        == {
            "src/sieve/distributed_sieve_worker_coordinator.cpp",
            "src/sieve/distributed_sieve_worker_coordinator_internal.hpp",
        }
        and WORKER_COORDINATOR_USE_SITE_ALLOWLIST
        == (
            WORKER_COORDINATOR_PRODUCTION_FILES
            | MERGE_COORDINATOR_PRODUCTION_FILES
            | MERGE_WRITER_AUTHORITY_PRODUCTION_FILES
            | {
                "tests/test_distributed_sieve_resume.cpp",
                "tests/test_distributed_sieve_merge_writer_authority.cpp",
            }
        )
        and WORKER_COORDINATOR_AUTHORITY_FREE_CLEANUP_FACTS
        == {"cleanup_intent_absent"}
        and (
            BOUND_WORK_USE_SITE_ALLOWLIST & WORKER_COORDINATOR_USE_SITE_ALLOWLIST
            == {WORKER_COORDINATOR_IMPLEMENTATION_FILE}
        )
        and WORKER_COORDINATOR_PRODUCTION_FILES <= DURABLE_ENVIRONMENT_FREE_FILES,
        "worker-coordinator inventory is not the exact source-private "
        "implementation, interface, and dedicated resume-test boundary",
    )

    valid_coordinator_composition = r"""
DistributedSieveWorkerCoordinatorResultV1
coordinate_missing_distributed_sieve_workers_v1() noexcept {
    DistributedSieveWorkerCoordinatorResultV1 result;
    if (!handoff.cleanup_intent_absent) {
        return result;
    }
    auto bound = bind_distributed_sieve_work_v1(
        identity, frozen_policy, polynomial, factor_base);
    auto opened = result.store->open_worker_attempt_private_lease_root(
        initial_attempt.chunk_id, initial_attempt.attempt_ordinal);
    auto reconciled =
        resume::reconcile_worker_attempt_started(std::move(opened));
    auto terminal_failure =
        resume::publish_chunk_terminal_failure_v1(
            std::move(*reconciled.terminal_failure_admission));
    if (reconciled.terminal_handoff.has_value()) {
        const auto& terminal = *reconciled.terminal_handoff;
        expected_adopted_witnesses[manifest_slot] = terminal;
    }
    auto launched = result.store->launch_worker_process_batch_v1(
        std::move(launch_request), identity, frozen_policy, polynomial, factor_base);
    auto adoption =
        expected_adopted_witnesses[index].has_value()
            ? result.store->adopt_expected_worker_handoff_v1(
                  *expected_adopted_witnesses[index])
            : result.store->adopt_worker_handoff_v1(manifest.chunks[index].chunk_id);
    return result;
}
"""
    exact_coordinator_checks = Checks(Path("."))
    exact_coordinator_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, valid_coordinator_composition
    )
    expect(
        not exact_coordinator_checks.errors,
        "exact worker-coordinator sealed-launcher composition was rejected: "
        f"{exact_coordinator_checks.errors}",
    )

    missing_coordinator_bound_work = valid_coordinator_composition.replace(
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen_policy, polynomial, factor_base);\n",
        "",
    )
    missing_coordinator_bound_work_checks = Checks(Path("."))
    missing_coordinator_bound_work_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, missing_coordinator_bound_work
    )
    expect(
        any(
            "must contain exactly one direct bind_distributed_sieve_work_v1 call"
            in error
            for error in missing_coordinator_bound_work_checks.errors
        ),
        "worker coordinator without bound-work validation was accepted: "
        f"{missing_coordinator_bound_work_checks.errors}",
    )

    duplicate_coordinator_bound_work = valid_coordinator_composition.replace(
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen_policy, polynomial, factor_base);\n",
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen_policy, polynomial, factor_base);\n"
        "    auto duplicate_bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen_policy, polynomial, factor_base);\n",
    )
    duplicate_coordinator_bound_work_checks = Checks(Path("."))
    duplicate_coordinator_bound_work_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, duplicate_coordinator_bound_work
    )
    expect(
        any(
            "must contain exactly one direct bind_distributed_sieve_work_v1 call"
            in error
            for error in duplicate_coordinator_bound_work_checks.errors
        ),
        "worker coordinator with duplicate bound-work validation was accepted: "
        f"{duplicate_coordinator_bound_work_checks.errors}",
    )

    coordinator_open_call = (
        "    auto opened = result.store->open_worker_attempt_private_lease_root(\n"
        "        initial_attempt.chunk_id, initial_attempt.attempt_ordinal);\n"
    )
    coordinator_reconcile_call = (
        "    auto reconciled =\n"
        "        resume::reconcile_worker_attempt_started(std::move(opened));\n"
    )
    coordinator_terminal_failure_publish_call = (
        "    auto terminal_failure =\n"
        "        resume::publish_chunk_terminal_failure_v1(\n"
        "            std::move(*reconciled.terminal_failure_admission));\n"
    )
    missing_coordinator_retry_open = valid_coordinator_composition.replace(
        coordinator_open_call, ""
    )
    missing_coordinator_retry_open_checks = Checks(Path("."))
    missing_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, missing_coordinator_retry_open
    )
    expect(
        any(
            "must contain exactly one direct open_worker_attempt_private_lease_root call"
            in error
            for error in missing_coordinator_retry_open_checks.errors
        ),
        "worker coordinator without the typed retry open was accepted: "
        f"{missing_coordinator_retry_open_checks.errors}",
    )

    wrong_coordinate_coordinator_retry_open = valid_coordinator_composition.replace(
        coordinator_open_call,
        "    auto opened = result.store->open_worker_attempt_private_lease_root(\n"
        "        manifest.chunks.front().chunk_id, 0U);\n",
    )
    wrong_coordinate_coordinator_retry_open_checks = Checks(Path("."))
    wrong_coordinate_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_coordinate_coordinator_retry_open,
    )
    expect(
        any(
            "must open the exact initial worker attempt exactly once" in error
            for error in wrong_coordinate_coordinator_retry_open_checks.errors
        ),
        "worker coordinator accepted caller-selected retry coordinates: "
        f"{wrong_coordinate_coordinator_retry_open_checks.errors}",
    )

    duplicate_coordinator_retry_open = valid_coordinator_composition.replace(
        coordinator_open_call,
        coordinator_open_call
        + "    auto duplicate_opened = "
        "result.store->open_worker_attempt_private_lease_root(\n"
        "        initial_attempt.chunk_id, initial_attempt.attempt_ordinal);\n",
    )
    duplicate_coordinator_retry_open_checks = Checks(Path("."))
    duplicate_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        duplicate_coordinator_retry_open,
    )
    expect(
        any(
            "must contain exactly one direct open_worker_attempt_private_lease_root call"
            in error
            for error in duplicate_coordinator_retry_open_checks.errors
        ),
        "worker coordinator with duplicate retry opens was accepted: "
        f"{duplicate_coordinator_retry_open_checks.errors}",
    )

    aliased_coordinator_retry_open = valid_coordinator_composition.replace(
        coordinator_open_call,
        "    auto opener = "
        "&DistributedSieveWaveStore::open_worker_attempt_private_lease_root;\n"
        "    auto opened = (result.store.get()->*opener)(\n"
        "        initial_attempt.chunk_id, initial_attempt.attempt_ordinal);\n",
    )
    aliased_coordinator_retry_open_checks = Checks(Path("."))
    aliased_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        aliased_coordinator_retry_open,
    )
    expect(
        any(
            "worker-attempt retry open authority must be used only as a direct call"
            in error
            for error in aliased_coordinator_retry_open_checks.errors
        ),
        "worker coordinator accepted aliased retry-open authority: "
        f"{aliased_coordinator_retry_open_checks.errors}",
    )

    outside_coordinator_retry_open = (
        valid_coordinator_composition
        + "\nauto outside_opened = "
        "result.store->open_worker_attempt_private_lease_root(\n"
        "    initial_attempt.chunk_id, initial_attempt.attempt_ordinal);\n"
    )
    outside_coordinator_retry_open_checks = Checks(Path("."))
    outside_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        outside_coordinator_retry_open,
    )
    expect(
        any(
            "must contain exactly one direct open_worker_attempt_private_lease_root call"
            in error
            for error in outside_coordinator_retry_open_checks.errors
        ),
        "worker coordinator retry-open authority escaped its entry function: "
        f"{outside_coordinator_retry_open_checks.errors}",
    )

    wrong_receiver_coordinator_retry_open = valid_coordinator_composition.replace(
        "result.store->open_worker_attempt_private_lease_root(",
        "raw_store->open_worker_attempt_private_lease_root(",
    )
    wrong_receiver_coordinator_retry_open_checks = Checks(Path("."))
    wrong_receiver_coordinator_retry_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_receiver_coordinator_retry_open,
    )
    expect(
        any(
            "must open the exact initial worker attempt exactly once" in error
            for error in wrong_receiver_coordinator_retry_open_checks.errors
        ),
        "worker coordinator accepted an unsealed retry-open receiver: "
        f"{wrong_receiver_coordinator_retry_open_checks.errors}",
    )

    missing_coordinator_reconciler = valid_coordinator_composition.replace(
        coordinator_reconcile_call, ""
    )
    missing_coordinator_reconciler_checks = Checks(Path("."))
    missing_coordinator_reconciler_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, missing_coordinator_reconciler
    )
    expect(
        any(
            "must contain exactly one direct reconcile_worker_attempt_started call"
            in error
            for error in missing_coordinator_reconciler_checks.errors
        ),
        "worker coordinator without the typed attempt reconciler was accepted: "
        f"{missing_coordinator_reconciler_checks.errors}",
    )

    duplicate_coordinator_reconciler = valid_coordinator_composition.replace(
        coordinator_reconcile_call,
        coordinator_reconcile_call
        + "    auto duplicate_reconciled =\n"
        "        resume::reconcile_worker_attempt_started(std::move(other_opened));\n",
    )
    duplicate_coordinator_reconciler_checks = Checks(Path("."))
    duplicate_coordinator_reconciler_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, duplicate_coordinator_reconciler
    )
    expect(
        any(
            "must contain exactly one direct reconcile_worker_attempt_started call"
            in error
            for error in duplicate_coordinator_reconciler_checks.errors
        ),
        "worker coordinator with duplicate attempt reconciliation was accepted: "
        f"{duplicate_coordinator_reconciler_checks.errors}",
    )

    wrong_operand_coordinator_reconciler = valid_coordinator_composition.replace(
        "resume::reconcile_worker_attempt_started(std::move(opened))",
        "resume::reconcile_worker_attempt_started(std::move(other_opened))",
    )
    wrong_operand_coordinator_reconciler_checks = Checks(Path("."))
    wrong_operand_coordinator_reconciler_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_operand_coordinator_reconciler,
    )
    expect(
        any(
            "must consume the exact opened attempt once" in error
            for error in wrong_operand_coordinator_reconciler_checks.errors
        ),
        "worker coordinator accepted reconciliation of an unbound open result: "
        f"{wrong_operand_coordinator_reconciler_checks.errors}",
    )

    reordered_coordinator_retry = valid_coordinator_composition.replace(
        coordinator_open_call + coordinator_reconcile_call,
        coordinator_reconcile_call + coordinator_open_call,
    )
    reordered_coordinator_retry_checks = Checks(Path("."))
    reordered_coordinator_retry_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, reordered_coordinator_retry
    )
    expect(
        any(
            "must open the exact attempt before reconciling it" in error
            for error in reordered_coordinator_retry_checks.errors
        ),
        "worker coordinator accepted attempt reconciliation before exact open: "
        f"{reordered_coordinator_retry_checks.errors}",
    )

    coordinator_bound_call = (
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen_policy, polynomial, factor_base);\n"
    )
    reordered_coordinator_bound_open = valid_coordinator_composition.replace(
        coordinator_bound_call + coordinator_open_call,
        coordinator_open_call + coordinator_bound_call,
    )
    reordered_coordinator_bound_open_checks = Checks(Path("."))
    reordered_coordinator_bound_open_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reordered_coordinator_bound_open,
    )
    expect(
        any(
            "must validate bound work before opening a retry attempt" in error
            for error in reordered_coordinator_bound_open_checks.errors
        ),
        "worker coordinator accepted retry open before bound-work validation: "
        f"{reordered_coordinator_bound_open_checks.errors}",
    )

    coordinator_launcher_call = (
        "    auto launched = result.store->launch_worker_process_batch_v1(\n"
        "        std::move(launch_request), identity, frozen_policy, polynomial, factor_base);\n"
    )
    reordered_coordinator_reconcile_launcher = valid_coordinator_composition.replace(
        coordinator_reconcile_call, ""
    ).replace(
        coordinator_launcher_call,
        coordinator_launcher_call + coordinator_reconcile_call,
    )
    reordered_coordinator_reconcile_launcher_checks = Checks(Path("."))
    reordered_coordinator_reconcile_launcher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reordered_coordinator_reconcile_launcher,
    )
    expect(
        any(
            "must reconcile retry state before invoking the sealed WaveStore launcher"
            in error
            for error in reordered_coordinator_reconcile_launcher_checks.errors
        ),
        "worker coordinator accepted launch before retry reconciliation: "
        f"{reordered_coordinator_reconcile_launcher_checks.errors}",
    )

    aliased_coordinator_reconciler = valid_coordinator_composition.replace(
        coordinator_reconcile_call,
        "    auto reconciler = resume::reconcile_worker_attempt_started;\n"
        "    auto reconciled = reconciler(std::move(opened));\n",
    )
    aliased_coordinator_reconciler_checks = Checks(Path("."))
    aliased_coordinator_reconciler_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, aliased_coordinator_reconciler
    )
    expect(
        any(
            "worker-attempt reconciler must be used only as a direct call" in error
            for error in aliased_coordinator_reconciler_checks.errors
        ),
        "worker coordinator accepted aliased attempt reconciliation: "
        f"{aliased_coordinator_reconciler_checks.errors}",
    )

    outside_coordinator_reconciler = (
        valid_coordinator_composition
        + "\nauto outside_reconciled =\n"
        "    resume::reconcile_worker_attempt_started(std::move(outside_opened));\n"
    )
    outside_coordinator_reconciler_checks = Checks(Path("."))
    outside_coordinator_reconciler_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, outside_coordinator_reconciler
    )
    expect(
        any(
            "must contain exactly one direct reconcile_worker_attempt_started call"
            in error
            for error in outside_coordinator_reconciler_checks.errors
        ),
        "worker coordinator reconciler authority escaped its entry function: "
        f"{outside_coordinator_reconciler_checks.errors}",
    )

    missing_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            coordinator_terminal_failure_publish_call, ""
        )
    )
    missing_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    missing_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        missing_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "must contain exactly one direct publish_chunk_terminal_failure_v1 call"
            in error
            for error in missing_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator without the typed terminal-failure publisher was accepted: "
        f"{missing_coordinator_terminal_failure_publisher_checks.errors}",
    )

    duplicate_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            coordinator_terminal_failure_publish_call,
            coordinator_terminal_failure_publish_call
            + "    auto duplicate_terminal_failure =\n"
            "        resume::publish_chunk_terminal_failure_v1(\n"
            "            std::move(*reconciled.terminal_failure_admission));\n",
        )
    )
    duplicate_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    duplicate_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        duplicate_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "must contain exactly one direct publish_chunk_terminal_failure_v1 call"
            in error
            for error in duplicate_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator with duplicate terminal-failure publication was accepted: "
        f"{duplicate_coordinator_terminal_failure_publisher_checks.errors}",
    )

    aliased_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            coordinator_terminal_failure_publish_call,
            "    auto terminal_failure_publisher =\n"
            "        &resume::publish_chunk_terminal_failure_v1;\n"
            "    auto terminal_failure = terminal_failure_publisher(\n"
            "        std::move(*reconciled.terminal_failure_admission));\n",
        )
    )
    aliased_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    aliased_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        aliased_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "terminal-failure publisher must be used only as a direct call" in error
            for error in aliased_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator accepted aliased terminal-failure publication authority: "
        f"{aliased_coordinator_terminal_failure_publisher_checks.errors}",
    )

    wrong_receiver_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            "resume::publish_chunk_terminal_failure_v1(",
            "other::publish_chunk_terminal_failure_v1(",
        )
    )
    wrong_receiver_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    wrong_receiver_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_receiver_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "must consume the exact reconciler terminal-failure admission once"
            in error
            for error in wrong_receiver_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator accepted an alternate terminal-failure publisher receiver: "
        f"{wrong_receiver_coordinator_terminal_failure_publisher_checks.errors}",
    )

    wrong_operand_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            "std::move(*reconciled.terminal_failure_admission)",
            "std::move(*other_reconciled.terminal_failure_admission)",
        )
    )
    wrong_operand_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    wrong_operand_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_operand_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "must consume the exact reconciler terminal-failure admission once"
            in error
            for error in wrong_operand_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator accepted an unbound terminal-failure admission: "
        f"{wrong_operand_coordinator_terminal_failure_publisher_checks.errors}",
    )

    outside_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition
        + "\nauto outside_terminal_failure =\n"
        "    resume::publish_chunk_terminal_failure_v1(\n"
        "        std::move(*outside_reconciled.terminal_failure_admission));\n"
    )
    outside_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    outside_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        outside_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "must contain exactly one direct publish_chunk_terminal_failure_v1 call"
            in error
            for error in outside_coordinator_terminal_failure_publisher_checks.errors
        ),
        "terminal-failure publication authority escaped the coordinator entry function: "
        f"{outside_coordinator_terminal_failure_publisher_checks.errors}",
    )

    reordered_coordinator_terminal_failure_before_reconcile = (
        valid_coordinator_composition.replace(
            coordinator_reconcile_call
            + coordinator_terminal_failure_publish_call,
            coordinator_terminal_failure_publish_call
            + coordinator_reconcile_call,
        )
    )
    reordered_coordinator_terminal_failure_before_reconcile_checks = Checks(
        Path(".")
    )
    reordered_coordinator_terminal_failure_before_reconcile_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reordered_coordinator_terminal_failure_before_reconcile,
    )
    expect(
        any(
            "must reconcile retry state before publishing terminal failure" in error
            for error in (
                reordered_coordinator_terminal_failure_before_reconcile_checks.errors
            )
        ),
        "worker coordinator accepted terminal-failure publication before reconciliation: "
        f"{reordered_coordinator_terminal_failure_before_reconcile_checks.errors}",
    )

    reordered_coordinator_terminal_failure_after_launcher = (
        valid_coordinator_composition.replace(
            coordinator_terminal_failure_publish_call, ""
        ).replace(
            coordinator_launcher_call,
            coordinator_launcher_call + coordinator_terminal_failure_publish_call,
        )
    )
    reordered_coordinator_terminal_failure_after_launcher_checks = Checks(
        Path(".")
    )
    reordered_coordinator_terminal_failure_after_launcher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reordered_coordinator_terminal_failure_after_launcher,
    )
    expect(
        any(
            "must publish terminal failure before invoking any sealed WaveStore launcher"
            in error
            for error in (
                reordered_coordinator_terminal_failure_after_launcher_checks.errors
            )
        ),
        "worker coordinator accepted terminal-failure publication after launch: "
        f"{reordered_coordinator_terminal_failure_after_launcher_checks.errors}",
    )

    dead_coordinator_terminal_failure_publisher = (
        valid_coordinator_composition.replace(
            coordinator_terminal_failure_publish_call,
            "    if (false) {\n"
            "        auto terminal_failure =\n"
            "            resume::publish_chunk_terminal_failure_v1(\n"
            "                std::move("
            "*reconciled.terminal_failure_admission));\n"
            "    }\n",
        )
    )
    dead_coordinator_terminal_failure_publisher_checks = Checks(Path("."))
    dead_coordinator_terminal_failure_publisher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        dead_coordinator_terminal_failure_publisher,
    )
    expect(
        any(
            "forbids constant-dead control flow" in error
            for error in dead_coordinator_terminal_failure_publisher_checks.errors
        ),
        "worker coordinator accepted constant-dead terminal-failure publication: "
        f"{dead_coordinator_terminal_failure_publisher_checks.errors}",
    )

    coordinator_expected_adoption_call = (
        "    auto adoption =\n"
        "        expected_adopted_witnesses[index].has_value()\n"
        "            ? result.store->adopt_expected_worker_handoff_v1(\n"
        "                  *expected_adopted_witnesses[index])\n"
        "            : result.store->adopt_worker_handoff_v1("
        "manifest.chunks[index].chunk_id);\n"
    )
    ordinary_coordinator_terminal_adoption = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        "    auto adoption = result.store->adopt_worker_handoff_v1(\n"
        "        manifest.chunks[index].chunk_id);\n",
    )
    ordinary_coordinator_terminal_adoption_checks = Checks(Path("."))
    ordinary_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        ordinary_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must contain exactly one direct adopt_expected_worker_handoff_v1 call"
            in error
            for error in ordinary_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator silently degraded terminal-witness adoption to ordinary "
        f"adoption: {ordinary_coordinator_terminal_adoption_checks.errors}",
    )

    wrong_witness_coordinator_terminal_adoption = (
        valid_coordinator_composition.replace(
            coordinator_expected_adoption_call,
            "    auto adoption = "
            "result.store->adopt_expected_worker_handoff_v1(\n"
            "        *unbound_witnesses[index]);\n",
        )
    )
    wrong_witness_coordinator_terminal_adoption_checks = Checks(Path("."))
    wrong_witness_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_witness_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must consume the exact reconciler terminal witness once" in error
            for error in wrong_witness_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator accepted an unbound terminal witness: "
        f"{wrong_witness_coordinator_terminal_adoption_checks.errors}",
    )

    duplicate_coordinator_terminal_adoption = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        coordinator_expected_adoption_call
        + "    auto duplicate_expected_adoption = "
        "result.store->adopt_expected_worker_handoff_v1(\n"
        "        *expected_adopted_witnesses[index]);\n",
    )
    duplicate_coordinator_terminal_adoption_checks = Checks(Path("."))
    duplicate_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        duplicate_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must contain exactly one direct adopt_expected_worker_handoff_v1 call"
            in error
            for error in duplicate_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator accepted duplicate terminal-witness adoption: "
        f"{duplicate_coordinator_terminal_adoption_checks.errors}",
    )

    aliased_coordinator_terminal_adoption = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        "    auto adopter = "
        "&DistributedSieveWaveStore::adopt_expected_worker_handoff_v1;\n"
        "    auto adoption = (result.store.get()->*adopter)(\n"
        "        *expected_adopted_witnesses[index]);\n",
    )
    aliased_coordinator_terminal_adoption_checks = Checks(Path("."))
    aliased_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        aliased_coordinator_terminal_adoption,
    )
    expect(
        any(
            "terminal-witness adoption must be used only as a direct call" in error
            for error in aliased_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator accepted aliased terminal-witness adoption: "
        f"{aliased_coordinator_terminal_adoption_checks.errors}",
    )

    outside_coordinator_terminal_adoption = (
        valid_coordinator_composition
        + "\nauto outside_expected_adoption = "
        "result.store->adopt_expected_worker_handoff_v1(\n"
        "    *expected_adopted_witnesses[index]);\n"
    )
    outside_coordinator_terminal_adoption_checks = Checks(Path("."))
    outside_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        outside_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must contain exactly one direct adopt_expected_worker_handoff_v1 call"
            in error
            for error in outside_coordinator_terminal_adoption_checks.errors
        ),
        "terminal-witness adoption escaped the coordinator entry function: "
        f"{outside_coordinator_terminal_adoption_checks.errors}",
    )

    wrong_receiver_coordinator_terminal_adoption = (
        valid_coordinator_composition.replace(
            "result.store->adopt_expected_worker_handoff_v1(",
            "raw_store->adopt_expected_worker_handoff_v1(",
        )
    )
    wrong_receiver_coordinator_terminal_adoption_checks = Checks(Path("."))
    wrong_receiver_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_receiver_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must consume the exact reconciler terminal witness once" in error
            for error in wrong_receiver_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator accepted an unsealed terminal-witness adoption receiver: "
        f"{wrong_receiver_coordinator_terminal_adoption_checks.errors}",
    )

    reordered_coordinator_terminal_adoption = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call, ""
    ).replace(
        coordinator_reconcile_call,
        coordinator_expected_adoption_call + coordinator_reconcile_call,
    )
    reordered_coordinator_terminal_adoption_checks = Checks(Path("."))
    reordered_coordinator_terminal_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reordered_coordinator_terminal_adoption,
    )
    expect(
        any(
            "must capture a terminal witness before expected same-handle adoption"
            in error
            for error in reordered_coordinator_terminal_adoption_checks.errors
        ),
        "worker coordinator accepted expected adoption before terminal-witness capture: "
        f"{reordered_coordinator_terminal_adoption_checks.errors}",
    )

    wrong_terminal_witness_provenance = valid_coordinator_composition.replace(
        "expected_adopted_witnesses[manifest_slot] = terminal;",
        "expected_adopted_witnesses[manifest_slot] = unbound_terminal;",
    )
    wrong_terminal_witness_provenance_checks = Checks(Path("."))
    wrong_terminal_witness_provenance_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        wrong_terminal_witness_provenance,
    )
    expect(
        any(
            "bind the exact reconciler terminal witness" in error
            or "must preserve reconcile -> terminal binding" in error
            for error in wrong_terminal_witness_provenance_checks.errors
        ),
        "worker coordinator accepted an unbound terminal witness source: "
        f"{wrong_terminal_witness_provenance_checks.errors}",
    )

    dead_expected_adoption = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        "    if (false) {\n"
        "        auto ignored = result.store->adopt_expected_worker_handoff_v1(\n"
        "            *expected_adopted_witnesses[index]);\n"
        "    }\n"
        "    auto adoption = result.store->adopt_worker_handoff_v1(\n"
        "        manifest.chunks[index].chunk_id);\n",
    )
    dead_expected_adoption_checks = Checks(Path("."))
    dead_expected_adoption_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        dead_expected_adoption,
    )
    expect(
        any(
            "must dispatch witnessed terminal handoffs" in error
            for error in dead_expected_adoption_checks.errors
        ),
        "dead expected-adoption code hid an ordinary live branch: "
        f"{dead_expected_adoption_checks.errors}",
    )
    full_dead_expected_dispatch = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        "    if (false) {\n"
        + coordinator_expected_adoption_call
        + "    }\n"
        "    auto live_adoption = result.store->adopt_worker_handoff_v1(\n"
        "        manifest.chunks[index].chunk_id);\n",
    )
    full_dead_expected_dispatch_checks = Checks(Path("."))
    full_dead_expected_dispatch_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        full_dead_expected_dispatch,
    )
    expect(
        any(
            "forbids constant-dead control flow" in error
            for error in full_dead_expected_dispatch_checks.errors
        )
        and any(
            "exactly one direct ordinary handoff adoption fallback" in error
            for error in full_dead_expected_dispatch_checks.errors
        ),
        "complete dead terminal dispatch hid an extra live ordinary adoption: "
        f"{full_dead_expected_dispatch_checks.errors}",
    )

    coordinator_terminal_provenance = (
        "    if (reconciled.terminal_handoff.has_value()) {\n"
        "        const auto& terminal = *reconciled.terminal_handoff;\n"
        "        expected_adopted_witnesses[manifest_slot] = terminal;\n"
        "    }\n"
    )
    full_dead_terminal_provenance = valid_coordinator_composition.replace(
        coordinator_terminal_provenance,
        "    if (false) {\n"
        + coordinator_terminal_provenance
        + "    }\n"
        "    expected_adopted_witnesses[manifest_slot] = unbound_terminal;\n",
    )
    full_dead_terminal_provenance_checks = Checks(Path("."))
    full_dead_terminal_provenance_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        full_dead_terminal_provenance,
    )
    expect(
        any(
            "forbids constant-dead control flow" in error
            for error in full_dead_terminal_provenance_checks.errors
        )
        and any(
            "exact one-write/two-read manifest-slot flow" in error
            for error in full_dead_terminal_provenance_checks.errors
        ),
        "complete dead reconciler provenance hid an unbound live terminal store: "
        f"{full_dead_terminal_provenance_checks.errors}",
    )

    reversed_terminal_adoption_dispatch = valid_coordinator_composition.replace(
        coordinator_expected_adoption_call,
        "    auto adoption =\n"
        "        expected_adopted_witnesses[index].has_value()\n"
        "            ? result.store->adopt_worker_handoff_v1(\n"
        "                  manifest.chunks[index].chunk_id)\n"
        "            : result.store->adopt_expected_worker_handoff_v1(\n"
        "                  *expected_adopted_witnesses[index]);\n",
    )
    reversed_terminal_adoption_dispatch_checks = Checks(Path("."))
    reversed_terminal_adoption_dispatch_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        reversed_terminal_adoption_dispatch,
    )
    expect(
        any(
            "must dispatch witnessed terminal handoffs" in error
            for error in reversed_terminal_adoption_dispatch_checks.errors
        ),
        "worker coordinator accepted reversed terminal-adoption dispatch: "
        f"{reversed_terminal_adoption_dispatch_checks.errors}",
    )

    reordered_coordinator_calls = valid_coordinator_composition.replace(
        coordinator_bound_call, ""
    ).replace(
        coordinator_launcher_call,
        coordinator_launcher_call + coordinator_bound_call,
    )
    reordered_coordinator_calls_checks = Checks(Path("."))
    reordered_coordinator_calls_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, reordered_coordinator_calls
    )
    expect(
        any(
            "must validate bound work before invoking the sealed WaveStore launcher"
            in error
            for error in reordered_coordinator_calls_checks.errors
        ),
        "worker coordinator accepted bound-work validation after launch: "
        f"{reordered_coordinator_calls_checks.errors}",
    )

    missing_coordinator_launcher = valid_coordinator_composition.replace(
        "    auto launched = result.store->launch_worker_process_batch_v1(\n"
        "        std::move(launch_request), identity, frozen_policy, polynomial, factor_base);\n",
        "    auto launched = 0;\n",
    )
    missing_coordinator_launcher_checks = Checks(Path("."))
    missing_coordinator_launcher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, missing_coordinator_launcher
    )
    expect(
        any(
            "must contain exactly one direct sealed WaveStore" in error
            for error in missing_coordinator_launcher_checks.errors
        ),
        "worker coordinator without the sealed launcher call was accepted: "
        f"{missing_coordinator_launcher_checks.errors}",
    )

    duplicate_coordinator_launcher = valid_coordinator_composition.replace(
        "    return result;\n",
        "    auto duplicate = result.store->launch_worker_process_batch_v1(\n"
        "        std::move(other_request), identity, frozen_policy, polynomial, factor_base);\n"
        "    return result;\n",
    )
    duplicate_coordinator_launcher_checks = Checks(Path("."))
    duplicate_coordinator_launcher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, duplicate_coordinator_launcher
    )
    expect(
        any(
            "must contain exactly one direct sealed WaveStore" in error
            for error in duplicate_coordinator_launcher_checks.errors
        ),
        "worker coordinator with duplicate sealed launcher calls was accepted: "
        f"{duplicate_coordinator_launcher_checks.errors}",
    )

    outside_coordinator_launcher = (
        valid_coordinator_composition
        + "\nauto outside = result.store->launch_worker_process_batch_v1(\n"
        "    std::move(other_request), identity, frozen_policy, polynomial, factor_base);\n"
    )
    outside_coordinator_launcher_checks = Checks(Path("."))
    outside_coordinator_launcher_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, outside_coordinator_launcher
    )
    expect(
        any(
            "must contain exactly one direct sealed WaveStore" in error
            for error in outside_coordinator_launcher_checks.errors
        ),
        "worker coordinator launcher authority escaped its entry function: "
        f"{outside_coordinator_launcher_checks.errors}",
    )

    wrong_coordinator_receiver = valid_coordinator_composition.replace(
        "result.store->launch_worker_process_batch_v1(",
        "raw_store->launch_worker_process_batch_v1(",
    )
    wrong_coordinator_receiver_checks = Checks(Path("."))
    wrong_coordinator_receiver_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, wrong_coordinator_receiver
    )
    expect(
        any(
            "must call the sealed WaveStore launcher exactly once through result.store"
            in error
            for error in wrong_coordinator_receiver_checks.errors
        ),
        "worker coordinator accepted an unsealed launcher receiver: "
        f"{wrong_coordinator_receiver_checks.errors}",
    )

    forbidden_coordinator_authority = valid_coordinator_composition.replace(
        "    return result;\n",
        "    auto forked = fork();\n"
        "    auto spawned = posix_spawn(&pid, path, actions, attrs, argv, envp);\n"
        "    auto waited = waitpid(pid, &status, 0);\n"
        "    auto shelled = system(command);\n"
        "    auto replaced = execve(path, argv, envp);\n"
        "    auto recovered = recover_worker_attempt_private_lease(std::move(claimed));\n"
        "    ChunkTerminalFailureV1 terminal_failure;\n"
        "    auto raw_published = durable_record::publish_at(\n"
        "        parent, pending, canonical, bytes);\n"
        "    cleanup_worker_artifacts();\n"
        "    unlink_worker_handoff();\n"
        "    run_distributed_sieve(config);\n"
        "    return result;\n",
    )
    forbidden_coordinator_checks = Checks(Path("."))
    forbidden_coordinator_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE, forbidden_coordinator_authority
    )
    expect(
        all(
            any(identifier in error for error in forbidden_coordinator_checks.errors)
            for identifier in (
                "fork",
                "posix_spawn",
                "waitpid",
                "system",
                "execve",
                "recover_worker_attempt_private_lease",
                "ChunkTerminalFailureV1",
                "publish_at",
                "cleanup_worker_artifacts",
                "unlink_worker_handoff",
                "run_distributed_sieve",
            )
        ),
        "worker-coordinator raw process, cleanup, unlink, or legacy-entry bans "
        f"are not closed: {forbidden_coordinator_checks.errors}",
    )

    legacy_coordinator_header_checks = Checks(Path("."))
    legacy_coordinator_header_checks.validate_worker_coordinator_boundary(
        WORKER_COORDINATOR_IMPLEMENTATION_FILE,
        "#include <gnfs/sieve/distributed_sieve.hpp>\n"
        + valid_coordinator_composition,
    )
    expect(
        any(
            "must not include the legacy public distributed-sieve header" in error
            for error in legacy_coordinator_header_checks.errors
        ),
        "worker coordinator accepted the legacy public distributed-sieve header: "
        f"{legacy_coordinator_header_checks.errors}",
    )

    launcher_use_site_snippet = r"""
DistributedSieveWorkerLaunchRequestV1 request(path, std::move(slots));
DistributedSieveWorkerLaunchBatchResultV1 result =
    store.launch_worker_process_batch_v1(
        std::move(request), identity, frozen, polynomial, factor_base);
"""
    launcher_use_site_checks = Checks(Path("."))
    launcher_use_site_checks.validate_worker_launcher_use_site(
        "src/sieve/untrusted_launcher.cpp", launcher_use_site_snippet
    )
    expect(
        len(launcher_use_site_checks.errors) == 3
        and all(
            "receipt-gated worker-launcher use site is not allowlisted" in error
            for error in launcher_use_site_checks.errors
        ),
        "receipt-gated worker-launcher repo-wide use-site gate is not enforced",
    )

    wrong_launcher_test_checks = Checks(Path("."))
    wrong_launcher_test_checks.validate_worker_launcher_use_site(
        "tests/test_distributed_sieve_worker_process.cpp",
        launcher_use_site_snippet,
    )
    expect(
        len(wrong_launcher_test_checks.errors) == 3,
        "worker-launcher test use escaped the dedicated resume-test allowlist",
    )

    for relative in sorted(WORKER_LAUNCHER_USE_SITE_ALLOWLIST):
        allowed_launcher_checks = Checks(Path("."))
        allowed_launcher_checks.validate_worker_launcher_use_site(
            relative, launcher_use_site_snippet
        )
        expect(
            not allowed_launcher_checks.errors,
            f"allowlisted receipt-gated worker-launcher use was rejected in "
            f"{relative}: {allowed_launcher_checks.errors}",
        )

    expect(
        WORKER_LAUNCHER_TEST_FILES
        == {
            "tests/test_distributed_sieve_resume.cpp",
            "tests/test_distributed_sieve_worker_entry.cpp",
            "tests/test_distributed_sieve_worker_writer_authority.cpp",
        }
        and (
            WORKER_LAUNCHER_USE_SITE_ALLOWLIST - WORKER_LAUNCHER_TEST_FILES
            == (
                WORKER_LAUNCHER_INTERFACE_FILES
                | {WORKER_LAUNCHER_IMPLEMENTATION_FILE}
                | WORKER_COORDINATOR_PRODUCTION_FILES
            )
        ),
        "worker-launcher allowlist is not the exact implementation boundary "
        "plus the source-private coordinator and the dedicated resume, worker-entry, "
        "and writer-authority tests",
    )

    lower_capability_allowlists = (
        BOUND_WORK_USE_SITE_ALLOWLIST,
        WORK_PACKAGE_CARRIER_USE_SITE_ALLOWLIST,
        WORKER_PROCESS_FIXED_CAPABILITY_USE_SITE_ALLOWLIST,
    )
    for allowlist in lower_capability_allowlists:
        expect(
            allowlist
            & (WORKER_LAUNCHER_INTERFACE_FILES | {WORKER_LAUNCHER_IMPLEMENTATION_FILE})
            == {WORKER_LAUNCHER_IMPLEMENTATION_FILE},
            "lower-level launch capability was expanded beyond the real "
            "WaveStore launcher implementation",
        )

    launcher_composition_checks = Checks(Path("."))
    launcher_composition_checks.validate_bound_work_use_site(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        "auto bound = bind_distributed_sieve_work_v1("
        "identity, frozen, polynomial, factor_base);",
    )
    launcher_composition_checks.validate_work_package_carrier_use_site(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        "auto package = create_distributed_sieve_worker_work_package_file_v1("
        "carrier_request, identity);",
    )
    launcher_composition_checks.validate_worker_process_fixed_capability_use_site(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        "DistributedSieveWorkerProcessFixedCapabilitySourcesV1 sources;\n"
        "auto child = spawn_distributed_sieve_worker_process_batch_with_capabilities("
        "std::move(prepared), capabilities);",
    )
    expect(
        not launcher_composition_checks.errors,
        "real WaveStore launcher composition was rejected by a lower-level "
        f"use-site gate: {launcher_composition_checks.errors}",
    )

    valid_launcher_composition = r"""
auto DistributedSieveWaveStore::launch_worker_process_batch_v1() {
    auto bound = bind_distributed_sieve_work_v1(
        identity, frozen, polynomial, factor_base);
    std::vector<std::optional<DistributedSieveWorkerWorkPackageFileV1>> packages;
    auto created = create_distributed_sieve_worker_work_package_file_v1(
        carrier_request, identity);
    std::vector<DistributedSieveWorkerProcessFixedCapabilitySourcesV1> capabilities;
    if (packages[0]->retained_reader_ < 0 ||
        packages[0]->retained_reader_ > maximum_descriptor) {
        return failed;
    }
    auto package_reader = packages[0]->retained_reader_;
    return spawn_distributed_sieve_worker_process_batch_with_capabilities(
        std::move(prepared), capabilities);
}
"""
    exact_launcher_composition_checks = Checks(Path("."))
    exact_launcher_composition_checks.validate_worker_launcher_composition_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE, valid_launcher_composition
    )
    expect(
        WORKER_LAUNCHER_COMPOSITION_USE_COUNTS
        == {
            "bind_distributed_sieve_work_v1": 1,
            "DistributedSieveWorkerWorkPackageFileV1": 1,
            "create_distributed_sieve_worker_work_package_file_v1": 1,
            "DistributedSieveWorkerProcessFixedCapabilitySourcesV1": 1,
            "spawn_distributed_sieve_worker_process_batch_with_capabilities": 1,
            "retained_reader_": 3,
        }
        and not exact_launcher_composition_checks.errors,
        "exact launcher composition body was rejected: "
        f"{exact_launcher_composition_checks.errors}",
    )

    outside_launcher_composition = (
        valid_launcher_composition
        + r"""
auto outside_bound = bind_distributed_sieve_work_v1(
    identity, frozen, polynomial, factor_base);
DistributedSieveWorkerWorkPackageFileV1 outside_package;
auto outside_created =
    create_distributed_sieve_worker_work_package_file_v1(carrier_request, identity);
DistributedSieveWorkerProcessFixedCapabilitySourcesV1 outside_sources;
auto outside_child = spawn_distributed_sieve_worker_process_batch_with_capabilities(
    std::move(prepared), capabilities);
auto outside_reader = outside_package.retained_reader_;
"""
    )
    outside_launcher_composition_checks = Checks(Path("."))
    outside_launcher_composition_checks.validate_worker_launcher_composition_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE, outside_launcher_composition
    )
    expect(
        len(outside_launcher_composition_checks.errors) == 6
        and all(
            "launcher authority must remain inside "
            f"{WORKER_LAUNCHER_COMPOSITION_FUNCTION}" in error
            for error in outside_launcher_composition_checks.errors
        ),
        "same-file outside-function launcher composition bypass was not rejected: "
        f"{outside_launcher_composition_checks.errors}",
    )

    duplicate_launcher_call = valid_launcher_composition.replace(
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen, polynomial, factor_base);\n",
        "    auto bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen, polynomial, factor_base);\n"
        "    auto duplicate_bound = bind_distributed_sieve_work_v1(\n"
        "        identity, frozen, polynomial, factor_base);\n",
    )
    duplicate_launcher_call_checks = Checks(Path("."))
    duplicate_launcher_call_checks.validate_worker_launcher_composition_body(
        WORKER_LAUNCHER_IMPLEMENTATION_FILE, duplicate_launcher_call
    )
    expect(
        len(duplicate_launcher_call_checks.errors) == 1
        and "exactly 1 direct bind_distributed_sieve_work_v1 call"
        in duplicate_launcher_call_checks.errors[0],
        "launcher lower-capability call count is not closed: "
        f"{duplicate_launcher_call_checks.errors}",
    )

    reader_use_site_checks = Checks(Path("."))
    reader_use_site_checks.validate_work_package_reader_use_site(
        "src/sieve/untrusted_launcher.cpp",
        "auto descriptor = package.retained_reader_;",
    )
    expect(
        len(reader_use_site_checks.errors) == 1
        and "work-package reader authority is not receipt-gated/allowlisted"
        in reader_use_site_checks.errors[0],
        "anonymous package-reader repo-wide use-site gate is not enforced",
    )
    expect(
        WORK_PACKAGE_READER_USE_SITE_ALLOWLIST
        == {
            "src/sieve/distributed_sieve_worker_work_package_file.cpp",
            "src/sieve/distributed_sieve_worker_work_package_file_internal.hpp",
            WORKER_LAUNCHER_IMPLEMENTATION_FILE,
        },
        "anonymous package-reader allowlist is not the exact definition and "
        "WaveStore launcher boundary",
    )

    legacy_launcher_checks = Checks(Path("."))
    legacy_launcher_checks.validate_worker_launcher_use_site(
        WORKER_PROCESS_LEGACY_FILE,
        "auto result = store.launch_worker_process_batch_v1("
        "std::move(request), identity, frozen, polynomial, factor_base);",
    )
    expect(
        len(legacy_launcher_checks.errors) == 1
        and "receipt-gated worker-launcher use site is not allowlisted"
        in legacy_launcher_checks.errors[0],
        "legacy distributed runner is not isolated from the receipt-gated launcher",
    )

    pipeline_checks = Checks(Path("."))
    pipeline_checks.validate_legacy_pipeline_boundary(
        r"""
// bind_distributed_sieve_work_v1 is ignored in comments.
const char* ignored = "DistributedSieveWorkIdentityV1";
DistributedSieveWorkIdentityV1 identity;
auto bound = bind_distributed_sieve_work_v1(identity, frozen, context, factor_base);
"""
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
        ("auto " + EXECUTION_POLICY_ENVIRONMENT_CAPTURE + "() noexcept {"),
        *[
            f'  owned_environment_value(std::getenv("{flag}"));'
            for flag in EXECUTION_POLICY_ENVIRONMENT_DESCRIPTOR_FLAGS
        ],
        ("  const auto host = " + EXECUTION_POLICY_HOST_CONCURRENCY_IDENTIFIER + "();"),
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
        adapter_checks.classify(EXECUTION_POLICY_ENVIRONMENT_ADAPTER, call, categories)
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
        and any(
            "not in canonical key order" in error for error in reordered_checks.errors
        ),
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

    valid_terminal_transition_boundary = r"""
bool exact_worker_attempt_terminal_transition() {
    return true;
}

Result DistributedSieveWaveStore::claim_worker_attempt_private_lease_root() const noexcept {
    bool terminal_transition_refreshed = false;
    const auto refresh_exact_terminal_transition = [&](const auto& observed) {
        if (terminal_transition_refreshed ||
            expectation != AttemptBaseLockExpectation::present ||
            !exact_worker_attempt_terminal_transition(
                before, observed, state_->manifest,
                *claim.worker_attempt_names_, chunk_id, attempt_ordinal,
                target_index)) {
            return false;
        }
        terminal_transition_refreshed = true;
        return true;
    };
    auto immediately_before = capture_manifest_bound_inventory_witness();
    if (!expected_successor_matches(immediately_before) &&
        !refresh_exact_terminal_transition(immediately_before)) {
        return {};
    }
    target = DistributedSievePrivateLeaseBaseLockAt::create_new_locked();
    target = DistributedSievePrivateLeaseBaseLockAt::open_existing_locked();
    auto held_target = capture_manifest_bound_inventory_witness(
        state_->root_fd, state_->manifest, state_->absolute_root,
        state_->creator_process_id, held_inventory());
    if (!expected_successor_matches(held_target) &&
        !refresh_exact_terminal_transition(held_target)) {
        return {};
    }
    invoke_private_lease_base_lock_hook(
        hooks.after_target_lock_acquired, hooks.context,
        state_->creator_process_id);
    revalidate_higher_priority_bindings();
    auto held_target_confirmed = capture_manifest_bound_inventory_witness(
        state_->root_fd, state_->manifest, state_->absolute_root,
        state_->creator_process_id, held_inventory());
    if (!held_target_confirmed ||
        !expected_successor_matches(held_target_confirmed)) {
        return {};
    }
    const auto revalidate_closed_successor = [&] { return Result{}; };
    revalidate_closed_successor();
    claim.base_lock_at_ = std::move(target);
    return {};
}
"""
    exact_terminal_transition_checks = Checks(Path("."))
    exact_terminal_transition_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        valid_terminal_transition_boundary,
    )
    expect(
        not exact_terminal_transition_checks.errors,
        "exact held-target terminal-transition boundary was rejected: "
        f"{exact_terminal_transition_checks.errors}",
    )

    reusable_terminal_transition = valid_terminal_transition_boundary.replace(
        "terminal_transition_refreshed ||\n            ", ""
    )
    reusable_terminal_transition_checks = Checks(Path("."))
    reusable_terminal_transition_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        reusable_terminal_transition,
    )
    expect(
        any(
            "single-use guard must appear exactly once" in error
            for error in reusable_terminal_transition_checks.errors
        ),
        "reusable terminal-transition adjudication was accepted: "
        f"{reusable_terminal_transition_checks.errors}",
    )

    confirmation_refresh = valid_terminal_transition_boundary.replace(
        "if (!held_target_confirmed ||\n"
        "        !expected_successor_matches(held_target_confirmed)) {",
        "if (!expected_successor_matches(held_target_confirmed) &&\n"
        "        !refresh_exact_terminal_transition(held_target_confirmed)) {",
    )
    confirmation_refresh_checks = Checks(Path("."))
    confirmation_refresh_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        confirmation_refresh,
    )
    expect(
        any(
            "exactly two direct call sites" in error
            or "confirmed held-target exact match must appear exactly once" in error
            for error in confirmation_refresh_checks.errors
        ),
        "terminal confirmation was allowed to refresh its baseline: "
        f"{confirmation_refresh_checks.errors}",
    )

    held_capture_source = (
        "    auto held_target = capture_manifest_bound_inventory_witness(\n"
        "        state_->root_fd, state_->manifest, state_->absolute_root,\n"
        "        state_->creator_process_id, held_inventory());\n"
    )
    post_held_hook_source = (
        "    invoke_private_lease_base_lock_hook(\n"
        "        hooks.after_target_lock_acquired, hooks.context,\n"
        "        state_->creator_process_id);\n"
    )
    early_post_target_hook = valid_terminal_transition_boundary.replace(
        post_held_hook_source, ""
    ).replace(
        held_capture_source,
        post_held_hook_source + held_capture_source,
    )
    early_post_target_hook_checks = Checks(Path("."))
    early_post_target_hook_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        early_post_target_hook,
    )
    expect(
        any(
            "must order pre-lock adjudication" in error
            for error in early_post_target_hook_checks.errors
        ),
        "post-target seam before the first held witness was accepted: "
        f"{early_post_target_hook_checks.errors}",
    )
    wrong_terminal_helper_operand = valid_terminal_transition_boundary.replace(
        "before, observed, state_->manifest,\n"
        "                *claim.worker_attempt_names_, chunk_id, attempt_ordinal,\n"
        "                target_index",
        "before, observed, unbound_manifest,\n"
        "                *claim.worker_attempt_names_, chunk_id, attempt_ordinal,\n"
        "                target_index",
    )
    wrong_terminal_helper_operand_checks = Checks(Path("."))
    wrong_terminal_helper_operand_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        wrong_terminal_helper_operand,
    )
    expect(
        any(
            "exact helper operands must appear exactly once" in error
            for error in wrong_terminal_helper_operand_checks.errors
        ),
        "worker-attempt transition accepted an unbound manifest helper operand: "
        f"{wrong_terminal_helper_operand_checks.errors}",
    )

    unheld_inventory_capture = valid_terminal_transition_boundary.replace(
        held_capture_source,
        held_capture_source.replace("held_inventory()", "unheld_inventory()"),
        1,
    )
    unheld_inventory_capture_checks = Checks(Path("."))
    unheld_inventory_capture_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        unheld_inventory_capture,
    )
    expect(
        any(
            "first held-target inventory witness must appear exactly once" in error
            for error in unheld_inventory_capture_checks.errors
        ),
        "worker-attempt transition accepted a first witness without the held "
        "lock inventory: "
        f"{unheld_inventory_capture_checks.errors}",
    )

    dead_held_target_chain = valid_terminal_transition_boundary.replace(
        held_capture_source,
        "    if (false) {\n" + held_capture_source,
        1,
    ).replace(
        "    claim.base_lock_at_ = std::move(target);\n",
        "    claim.base_lock_at_ = std::move(target);\n"
        "    }\n"
        "    claim.base_lock_at_ = std::move(unvalidated_target);\n",
        1,
    )
    dead_held_target_chain_checks = Checks(Path("."))
    dead_held_target_chain_checks.validate_worker_attempt_terminal_transition_boundary(
        WORKER_ATTEMPT_WAVE_STORE_IMPLEMENTATION_FILE,
        dead_held_target_chain,
    )
    expect(
        any(
            "forbids constant-dead control flow" in error
            for error in dead_held_target_chain_checks.errors
        )
        and any(
            "transfer only the exact validated target lock exactly once" in error
            for error in dead_held_target_chain_checks.errors
        ),
        "worker-attempt transition accepted a dead validated chain plus live "
        "unvalidated capability transfer: "
        f"{dead_held_target_chain_checks.errors}",
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
    parser.add_argument(
        "--root", type=Path, help="repository root (defaults to this script's parent)"
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run parser/table self-tests first"
    )
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
