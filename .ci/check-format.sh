#!/usr/bin/env bash

set -euo pipefail

SOURCES=$(git ls-files '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp')

# Use clang-format dry-run mode with --Werror to fail on format violations
# This eliminates the need for temporary files and manual diff comparisons
clang-format-18 -n --Werror ${SOURCES}
