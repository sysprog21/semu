#!/usr/bin/env bash

pkill -9 semu

expect <<DONE
set timeout 120
spawn make check
expect "buildroot login:" { send "root\n" } timeout { exit 1 }
expect "# " { send "uname -a\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "\x01"; send "x" } timeout { exit 3 }
DONE

ret=$?
pkill -9 semu
echo

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
)

echo "${MESSAGES[$ret]}"
exit ${ret}
