#!/usr/bin/env bash

# Source common functions and settings
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

ASSERT expect <<DONE
set timeout ${TIMEOUT}
spawn make check
expect "buildroot login:" { send "root\n" } timeout { exit 1 }
expect "# " { send "uname -a\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "\x01"; send "x" } timeout { exit 3 }
DONE

ret="$?"

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
)

if [ "$ret" -eq 0 ]; then
    print_success "${MESSAGES["$ret"]}"
else
    print_error "${MESSAGES["$ret"]}"
fi

exit "$ret"
