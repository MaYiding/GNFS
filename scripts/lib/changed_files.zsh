#!/usr/bin/env zsh

# Resolve the commit that anchors `test.sh changed` without mutating the
# repository. Explicit CI inputs win; local runs use the common ancestor with
# origin/main so a clean feature branch still exercises its committed diff.
resolve_changed_base() {
    local project_root="$1"
    local configured_base="${GNFS_TEST_BASE_SHA:-}"

    # The repository's CI workflows use BASE_SHA for pull-request and push
    # comparisons. GNFS_TEST_BASE_SHA remains the explicit local override.
    if [[ -z "$configured_base" ]]; then
        configured_base="${BASE_SHA:-}"
    fi
    if [[ -z "$configured_base" && -n "${GITHUB_BASE_REF:-}" ]]; then
        configured_base="origin/${GITHUB_BASE_REF}"
    fi

    if [[ -n "$configured_base" && ! "$configured_base" =~ ^0+$ ]]; then
        git -C "$project_root" rev-parse --verify "${configured_base}^{commit}" 2>/dev/null
        return $?
    fi

    local origin_main
    origin_main=$(git -C "$project_root" rev-parse --verify 'origin/main^{commit}' 2>/dev/null) || return 1
    git -C "$project_root" merge-base HEAD "$origin_main"
}

# Emit all committed and working-tree paths that can affect a changed-mode
# selection. The caller may pass an already-resolved base to avoid resolving
# it twice and to make an unavailable base visible to the caller.
collect_changed_files() {
    local project_root="$1"
    local base_ref="${2:-}"

    if [[ -n "$base_ref" ]]; then
        git -C "$project_root" diff --name-only "$base_ref" HEAD
    fi
    git -C "$project_root" diff --name-only HEAD
    git -C "$project_root" diff --name-only --cached
    git -C "$project_root" ls-files --others --exclude-standard
}
