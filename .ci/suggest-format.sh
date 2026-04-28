#!/usr/bin/env bash
#
# Post clang-format-20 diffs as PR review suggestions via reviewdog. Pairs
# with .ci/check-format.sh: this script *suggests* on pull requests; that
# one *enforces* by failing CI when violations land on master.
#
# Designed for the GitHub Actions `coding_style` job. The workflow must
# provide REVIEWDOG_GITHUB_API_TOKEN in the environment and install
# clang-format-20 + reviewdog beforehand.
#
# The script applies clang-format in-place to surface a diff, then always
# restores the working tree via an EXIT trap so a failed reviewdog run
# does not leave reformatted sources behind.

set -euo pipefail

# Collect all C/C++ sources tracked by git (POSIX-compatible array build
# for bash 3.2 compatibility on macOS runners).
SOURCES=()
while IFS= read -r file; do
    SOURCES+=("$file")
done < <(git ls-files '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp')

# Restore files on exit so a failed reviewdog run does not leave
# clang-format-modified sources in the working tree.
cleanup_files() {
    if [ ${#SOURCES[@]} -gt 0 ]; then
        echo "Restoring files to original state..."
        git checkout -- "${SOURCES[@]}" 2>/dev/null || true
    fi
}
trap cleanup_files EXIT INT TERM

# Apply formatting in-place to surface the diff against the working tree.
clang-format-20 -i "${SOURCES[@]}"

# Pipe the diff into reviewdog. `|| true` keeps the trap from being
# pre-empted by reviewdog's exit status when there are findings.
git diff -u --no-color | reviewdog -f=diff \
    -name="clang-format" \
    -reporter=github-pr-review \
    -filter-mode=added \
    -fail-on-error=false \
    -level=warning || true
