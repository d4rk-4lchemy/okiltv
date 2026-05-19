#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

build_dir_arg="${1:-qt/out/build/qt-linux-debug}"
if [[ "$build_dir_arg" = /* ]]; then
    build_dir="$build_dir_arg"
else
    build_dir="$repo_root/$build_dir_arg"
fi

compile_db="$build_dir/compile_commands.json"
if [[ ! -f "$compile_db" ]]; then
    echo "Missing compilation database: $compile_db" >&2
    echo "Run: cmake --fresh --preset qt-linux-debug" >&2
    exit 1
fi

for tool in jq clang-tidy rg; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required tool not found: $tool" >&2
        exit 1
    fi
done

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

source_prefix="$repo_root/qt/src/"
dedup_db="$tmpdir/compile_commands.json"

jq --arg source_prefix "$source_prefix" '
    [
      .[]
      | select(.file | startswith($source_prefix))
      | select(.file | endswith(".cpp"))
      | . + {
          rank:
            (if ((.output // "") | contains("qt/CMakeFiles/OKILTVQt.dir/")) then 0
             elif ((.output // "") | contains("qt/tests/")) then 2
             else 1
             end)
        }
    ]
    | sort_by(.file, .rank)
    | unique_by(.file)
    | map(del(.rank))
' "$compile_db" >"$dedup_db"

raw_entry_count="$(jq 'length' "$compile_db")"
dedup_entry_count="$(jq 'length' "$dedup_db")"

mapfile -t files < <(jq -r '.[].file' "$dedup_db")
if (( ${#files[@]} == 0 )); then
    echo "No qt/src .cpp files found in $compile_db" >&2
    exit 1
fi

raw_log="$(mktemp "${TMPDIR:-/tmp}/okiltv-clang-tidy-raw.XXXXXX.log")"
findings_log="$(mktemp "${TMPDIR:-/tmp}/okiltv-clang-tidy-findings.XXXXXX.log")"

status=0
if ! clang-tidy -p "$tmpdir" "${files[@]}" --quiet >"$raw_log" 2>&1; then
    status=$?
fi

awk '/^\/.*(warning|error|note): / {print}' "$raw_log" >"$findings_log"

warning_count="$(rg -c '^/.+warning:' "$findings_log" || true)"
warning_count="${warning_count:-0}"
error_count="$(rg -c '^/.+error:' "$findings_log" || true)"
error_count="${error_count:-0}"
unique_finding_count="$(awk '/^\/.*(warning|error): / {print}' "$findings_log" | sort -u | wc -l | tr -d "[:space:]")"
unique_finding_count="${unique_finding_count:-0}"

echo "clang-tidy source pass"
echo "build_dir: $build_dir"
echo "raw_compile_commands: $raw_entry_count"
echo "deduped_src_entries: $dedup_entry_count"
echo "analyzed_files: ${#files[@]}"
echo "warnings: $warning_count"
echo "errors: $error_count"
echo "unique_findings: $unique_finding_count"
echo "raw_log: $raw_log"
echo "findings_log: $findings_log"

if (( unique_finding_count > 0 )); then
    echo "top_checks:"
    rg -o '\[[^]]+\]$' "$findings_log" | sort | uniq -c | sort -nr | sed 's/^/  /'
fi

exit "$status"
