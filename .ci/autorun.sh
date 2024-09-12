#!/usr/bin/env bash

function cleanup {
    sleep 1
    pkill -9 semu
}

function ASSERT {
    $*
    local RES=$?
    if [ $RES -ne 0 ]; then
        echo 'Assert failed: "' $* '"'
        exit $RES
    fi
}

cleanup

# macOS needs more time to boot compared to Linux, so the timeout is set to
# 600 seconds for macOS to handle the longer startup. For Linux, 90 seconds
# is sufficient due to its faster boot process.
UNAME_S=$(uname -s)
if [[ ${UNAME_S} == "Darwin" ]]; then
    TIMEOUT=600
else # Linux
    TIMEOUT=90
fi

ASSERT expect <<DONE
set timeout ${TIMEOUT}
spawn make check
expect "buildroot login:" { send "root\n" } timeout { exit 1 }
expect "# " { send "uname -a\n" } timeout { exit 2 }
expect "riscv32 GNU/Linux" { send "\x01"; send "x" } timeout { exit 3 }
DONE

ret=$?
cleanup

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
)

COLOR_G='\e[32;01m' # Green
COLOR_N='\e[0m'
printf "\n[ ${COLOR_G}${MESSAGES[$ret]}${COLOR_N} ]\n"

exit ${ret}
