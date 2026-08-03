#pragma once

// Forward declarations for the source-private, receipt-gated worker launcher.
//
// Keep this header free of launcher implementation dependencies so the
// WaveStore can name its sole receipt-consuming entry point without creating
// a circular include.

namespace gnfs::sieve::distributed_sieve_worker_launcher_detail {

class DistributedSieveWorkerLaunchSlotV1;
struct DistributedSieveWorkerLauncherTestHooksV1;
class DistributedSieveWorkerLaunchRequestV1;
class DistributedSieveLaunchedWorkerAttemptV1;
struct DistributedSieveWorkerLaunchChildResultV1;
struct DistributedSieveWorkerLaunchBatchResultV1;

} // namespace gnfs::sieve::distributed_sieve_worker_launcher_detail
