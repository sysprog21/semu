#!/usr/bin/env bash

set -euo pipefail

# Source common functions and settings
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SCRIPT_DIR
source "${SCRIPT_DIR}/common.sh"

SAMPLE_SOUND="/usr/share/sounds/alsa/Front_Center.wav"

# Override timeout for sound tests
# Sound tests need different timeout: 30s for Linux, 900s for macOS
case "${OS_TYPE}" in
    Darwin)
        TIMEOUT=900
        ;;
    Linux)
        TIMEOUT=30
        ;;
    *)
        TIMEOUT=30
        ;;
esac

# Clean up any existing semu processes before starting tests
cleanup

# Test sound device
expect <<DONE
set timeout ${TIMEOUT}
spawn make check
expect "buildroot login:" { send "root\\n" } timeout { exit 1 }
expect "# " { send "uname -a\\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "aplay ${SAMPLE_SOUND} \\n" } timeout { exit 3 }
expect " Mono" { } timeout { exit 4 }
DONE

ret="$?"

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run playback commands" \
     "Playback fails" \
)

if [ "$ret" -eq 0 ]; then
    print_success "${MESSAGES["$ret"]}"
else
    print_error "${MESSAGES["$ret"]}"
fi

exit "$ret"
