# Check the benchmark records of one SIQS solve, kernel, prepare, or fbcheck run.
BEGIN {
    required_count = split(workload, required, " ")
}
/^GNFS_SIQS_SHADOW_MATRIX_BENCH_(CONFIG|RESULT)_V1 / {
    for (key in fields) delete fields[key]
    for (i = 2; i <= NF; ++i) {
        equals = index($i, "=")
        key = substr($i, 1, equals - 1)
        if (equals < 2 || key in fields) invalid = 1
        fields[key] = substr($i, equals + 1)
    }
    if (fields["mode"] != mode || fields["build_contract"] != "release_ndebug" ||
        fields["cmake_build_type"] != "Release" || fields["timing_asserted"] != "false" ||
        fields["seed"] != "0x53a9f19d97e8c641") invalid = 1
    for (i = 1; i <= required_count; ++i) {
        key = substr(required[i], 1, index(required[i], "=") - 1)
        if ((key "=" fields[key]) != required[i]) invalid = 1
    }
    if ($1 == "GNFS_SIQS_SHADOW_MATRIX_BENCH_CONFIG_V1") {
        configs++
        next
    }

    results++
    worker = fields["workers"]
    implementation = fields["implementation"]
    digest = fields["result_digest"]
    dependency_digest = fields["dependency_digest"]
    if (fields["status"] != "ok" || $NF !~ /^result_digest=0x[0-9a-f]+$/ ||
        fields["wall_min_ns"] !~ /^[0-9]+$/ || fields["wall_median_ns"] !~ /^[0-9]+$/ ||
        fields["wall_max_ns"] !~ /^[0-9]+$/ ||
        fields["wall_min_ns"] + 0 > fields["wall_median_ns"] + 0 ||
        fields["wall_median_ns"] + 0 > fields["wall_max_ns"] + 0) invalid = 1
    if (results == 1) reference_digest = digest
    else if (digest != reference_digest) invalid = 1
    if (mode == "solve") {
        if (dependency_digest != digest) invalid = 1
    } else if (dependency_digest != "na") invalid = 1
    seen[worker, implementation]++
}
END {
    if (configs != 1) invalid = 1
    expected_count = split(requested_workers, expected, ",")
    if (mode == "solve") {
        if (results != expected_count) invalid = 1
        for (index_ = 1; index_ <= expected_count; ++index_)
            if (seen[expected[index_], "public_solver"] != 1) invalid = 1
    } else if (mode == "kernel") {
        if (results != expected_count * 3) invalid = 1
        for (index_ = 1; index_ <= expected_count; ++index_) {
            if (seen[expected[index_], "legacy_per_pivot_jthread"] != 1)
                invalid = 1
            if (seen[expected[index_],
                     "benchmark_only_queued_thread_pool"] != 1)
                invalid = 1
            if (seen[expected[index_],
                     "production_persistent_worker_team"] != 1)
                invalid = 1
        }
    } else if (mode == "prepare") {
        if (results != 2 || seen["1", "public_identity_wrapper"] != 1 ||
            seen["1", "prevalidated_identity_helper"] != 1) invalid = 1
    } else if (mode == "fbcheck") {
        if (results != 1 || seen["1", "factor_base_validation"] != 1) invalid = 1
    } else {
        invalid = 1
    }
    if (invalid) exit 1
}
