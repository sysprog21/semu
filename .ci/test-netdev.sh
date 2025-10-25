#!/usr/bin/env bash

# Source common functions and settings
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

# Override timeout for netdev tests
# Network tests need different timeout: 30s for Linux, 600s for macOS
case "${OS_TYPE}" in
    Darwin)
        TIMEOUT=600
        ;;
    Linux)
        TIMEOUT=30
        ;;
    *)
        TIMEOUT=30
        ;;
esac

# Test network device functionality
TEST_NETDEV() {
    local NETDEV="$1"
    local CMD_PREFIX=""

    if [ "$NETDEV" == "tap" ]; then
        CMD_PREFIX="sudo "
    fi

    ASSERT expect <<DONE
    set timeout ${TIMEOUT}
    spawn ${CMD_PREFIX}make check NETDEV=${NETDEV}
    expect "buildroot login:" { send "root\n" } timeout { exit 1 }
    expect "# " { send "uname -a\n" } timeout { exit 2 }
    if { "$NETDEV" == "tap" } {
        exec sudo ip addr add 192.168.10.1/24 dev tap0
        exec sudo ip link set tap0 up
        expect "riscv32 GNU/Linux" { send "ip l set eth0 up\n" } timeout { exit 3 }
        expect "# " { send "ip a add 192.168.10.2/24 dev eth0\n" }
        expect "# " { send "ping -c 3 192.168.10.1\n" }
        expect "3 packets transmitted, 3 packets received, 0% packet loss" { } timeout { exit 4 }
    } elseif { "$NETDEV" == "user" } {
        expect "riscv32 GNU/Linux" { send "ip addr add 10.0.2.15/24 dev eth0\n" } timeout { exit 3 }
        expect "# " { send "ip link set eth0 up\n"}
        expect "# " { send "ip route add default via 10.0.2.2\n"}
        expect "# " { send "ping -c 3 10.0.2.2\n" }
        expect "3 packets transmitted, 3 packets received, 0% packet loss" { } timeout { exit 4 }
    }
DONE
}

# Network devices to test
NETWORK_DEVICES=(tap user)

for NETDEV in "${NETWORK_DEVICES[@]}"; do
    cleanup
    echo "Test network device: $NETDEV"
    TEST_NETDEV "$NETDEV"
done

ret=$?

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
     "Fail to transfer packet" \
)

if [ $ret -eq 0 ]; then
    print_success "${MESSAGES[$ret]}"
else
    print_error "${MESSAGES[$ret]}"
fi

exit ${ret}
