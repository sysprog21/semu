#!/usr/bin/env bash

set -euo pipefail

# Get list of source files into array (POSIX-compatible for macOS bash 3.2)
SOURCES=()
while IFS= read -r file; do
    SOURCES+=("$file")
done < <(git ls-files '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp')

# Use clang-format dry-run mode with --Werror to fail on format violations
# This eliminates the need for temporary files and manual diff comparisons
clang-format-18 -n --Werror "${SOURCES[@]}"
