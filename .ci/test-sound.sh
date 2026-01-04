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
<<<<<<< HEAD
    *)  
=======
    *)
>>>>>>> master
        TIMEOUT=30
        ;;
esac

<<<<<<< HEAD
test_sound() {
    ASSERT expect <<DONE
    set timeout ${TIMEOUT}
    spawn make check
    expect "buildroot login:" { send "root\\n" } timeout { exit 1 }
    expect "# " { send "uname -a\\n" } timeout { exit 2 }

    expect "riscv32 GNU/Linux" { send "aplay ${SAMPLE_SOUND} --fatal-errors > /dev/null\\n" } timeout { exit 3 }
    expect "# " { send "aplay -C -d 3 --fatal-errors -f S16_LE > /dev/null\\n" } timeout { exit 4 }
    expect "# " { send "aplay -L\\n" }
    expect "# " { }
DONE
    echo "✓ sound test passed"
}

=======
>>>>>>> master
# Clean up any existing semu processes before starting tests
cleanup

# Test sound device
<<<<<<< HEAD
test_sound
=======
expect <<DONE
set timeout ${TIMEOUT}
spawn make check
expect "buildroot login:" { send "root\\n" } timeout { exit 1 }
expect "# " { send "uname -a\\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "aplay ${SAMPLE_SOUND} \\n" } timeout { exit 3 }
expect " Mono" { } timeout { exit 4 }
DONE
>>>>>>> master

ret="$?"

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
<<<<<<< HEAD
     "Playback fails" \
     "Capture fails" \
=======
     "Fail to run playback commands" \
     "Playback fails" \
>>>>>>> master
)

if [ "$ret" -eq 0 ]; then
    print_success "${MESSAGES["$ret"]}"
else
    print_error "${MESSAGES["$ret"]}"
fi

exit "$ret"
