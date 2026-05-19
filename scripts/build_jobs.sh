#!/usr/bin/env bash
set -euo pipefail

parse_positive_int() {
    local raw="$1"
    [[ "$raw" =~ ^[0-9]+$ ]] || return 1
    (( raw > 0 )) || return 1
    echo "$raw"
}

if [[ -n "${OKILTV_BUILD_JOBS:-}" ]]; then
    jobs="$(parse_positive_int "$OKILTV_BUILD_JOBS")" || {
        echo "OKILTV_BUILD_JOBS must be a positive integer." >&2
        exit 1
    }
    echo "$jobs"
    exit 0
fi

cpu_count="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
cpu_count="$(parse_positive_int "$cpu_count" || echo 1)"
cpu_cap=1
if (( cpu_count > 1 )); then
    cpu_cap=$((cpu_count - 1))
fi

per_job_mb="${OKILTV_BUILD_PER_JOB_MB:-1536}"
per_job_mb="$(parse_positive_int "$per_job_mb")" || {
    echo "OKILTV_BUILD_PER_JOB_MB must be a positive integer." >&2
    exit 1
}

mem_cap="$cpu_cap"
if [[ -r /proc/meminfo ]]; then
    mem_available_kb="$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo)"
    if [[ -z "$mem_available_kb" ]]; then
        mem_available_kb="$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)"
    fi
    swap_free_kb="$(awk '/^SwapFree:/ { print $2; exit }' /proc/meminfo)"
    mem_available_kb="${mem_available_kb:-0}"
    swap_free_kb="${swap_free_kb:-0}"
    total_available_mb=$(((mem_available_kb + swap_free_kb) / 1024))
    if (( total_available_mb > 0 )); then
        mem_cap=$((total_available_mb / per_job_mb))
        if (( mem_cap < 1 )); then
            mem_cap=1
        fi
    fi
fi

jobs="$cpu_cap"
if (( mem_cap < jobs )); then
    jobs="$mem_cap"
fi
if (( jobs < 1 )); then
    jobs=1
fi

if [[ -n "${OKILTV_BUILD_MAX_JOBS:-}" ]]; then
    max_jobs="$(parse_positive_int "$OKILTV_BUILD_MAX_JOBS")" || {
        echo "OKILTV_BUILD_MAX_JOBS must be a positive integer." >&2
        exit 1
    }
    if (( max_jobs < jobs )); then
        jobs="$max_jobs"
    fi
fi

echo "$jobs"
