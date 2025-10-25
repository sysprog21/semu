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

# Clean up any existing semu processes before starting tests
cleanup

# Platform detection
UNAME_S=$(uname -s)

# Check if running on macOS
if [[ ${UNAME_S} == "Darwin" ]]; then
    IS_MACOS=1
else
    IS_MACOS=0
fi

# Network device to test (can be overridden by NETDEV environment variable)
NETDEV=${NETDEV:-""}

# Check for root privileges based on network device
REQUIRES_SUDO=0
if [[ ${IS_MACOS} -eq 1 && "${NETDEV}" == "vmnet" ]]; then
    REQUIRES_SUDO=1
    SUDO_REASON="vmnet.framework requires root privileges"
elif [[ ${IS_MACOS} -eq 0 && "${NETDEV}" == "tap" ]]; then
    REQUIRES_SUDO=1
    SUDO_REASON="TAP device requires root privileges"
fi

# Check for root privileges if required
if [[ ${REQUIRES_SUDO} -eq 1 ]]; then
    if [[ $EUID -ne 0 ]]; then
        echo "Error: This script must be run with sudo for ${NETDEV} mode"
        echo "Reason: ${SUDO_REASON}"
        echo ""
        echo "Usage: sudo $0"
        echo "Or use NETDEV=user for no-sudo mode (macOS/Linux)"
        exit 1
    fi
fi

# Test network device functionality
TEST_NETDEV() {
    local NETDEV=$1

    ASSERT expect <<DONE
    set timeout ${TIMEOUT}
    spawn make check NETDEV=${NETDEV}
    expect "buildroot login:" { send "root\\n" } timeout { exit 1 }
    expect "# " { send "uname -a\\n" } timeout { exit 2 }

    if { "$NETDEV" == "tap" } {
        exec ip addr add 192.168.10.1/24 dev tap0
        exec ip link set tap0 up
        expect "riscv32 GNU/Linux" { send "ip l set eth0 up\\n" } timeout { exit 3 }
        expect "# " { send "ip a add 192.168.10.2/24 dev eth0\\n" }
        expect "# " { send "ping -c 3 192.168.10.1\\n" }
        expect "3 packets transmitted, 3 packets received, 0% packet loss" { } timeout { exit 4 }
    } elseif { "$NETDEV" == "user" } {
        expect "riscv32 GNU/Linux" { send "ip addr add 10.0.2.15/24 dev eth0\\n" } timeout { exit 3 }
        expect "# " { send "ip link set eth0 up\\n"}
        expect "# " { send "ip route add default via 10.0.2.2\\n"}
        expect "# " { send "ping -c 3 10.0.2.2\\n" }
        expect "3 packets transmitted, 3 packets received, 0% packet loss" { } timeout { exit 4 }
    } elseif { "$NETDEV" == "vmnet" } {
        # vmnet (macOS): use DHCP to obtain IP address
        expect "riscv32 GNU/Linux" { send "ip link set eth0 up\\n" } timeout { exit 3 }
        expect "# " { send "udhcpc -n -q -i eth0\\n" }
        expect "# " { send "sleep 2\\n" }
        expect "# " { send "ip addr show eth0\\n" }
        # Ping the vmnet gateway (typically 192.168.64.1 for shared mode)
        expect "# " { send "ping -c 3 192.168.64.1\\n" }
        expect "3 packets transmitted, 3 packets received, 0% packet loss" { } timeout { exit 4 }
    }
DONE
}

# Determine network devices to test based on platform
if [[ -n "${NETDEV}" ]]; then
    # NETDEV environment variable specified - test only that device
    NETWORK_DEVICES=(${NETDEV})
elif [[ ${IS_MACOS} -eq 1 ]]; then
    # macOS: test both user (no sudo) and vmnet (requires sudo)
    # Default to user if not running as root
    if [[ $EUID -eq 0 ]]; then
        NETWORK_DEVICES=(user vmnet)
    else
        NETWORK_DEVICES=(user)
        echo "Note: Running without sudo, testing user mode only"
        echo "Run with 'sudo' to test vmnet mode"
    fi
else
    # Linux: test tap (requires sudo) and user (no sudo)
    if [[ $EUID -eq 0 ]]; then
        NETWORK_DEVICES=(tap user)
    else
        NETWORK_DEVICES=(user)
        echo "Note: Running without sudo, testing user mode only"
        echo "Run with 'sudo' to test tap mode"
    fi
fi

echo "Platform: ${UNAME_S}"
echo "Network devices to test: ${NETWORK_DEVICES[@]}"

for NETDEV in "${NETWORK_DEVICES[@]}"; do
    cleanup
    echo ""
    echo "========================================="
    echo "Testing network device: $NETDEV"
    echo "========================================="
    TEST_NETDEV $NETDEV
    echo "✓ $NETDEV test passed"
done

ret="$?"

MESSAGES=("OK!" \
     "Fail to boot" \
     "Fail to login" \
     "Fail to run commands" \
     "Fail to transfer packet" \
)

if [ "$ret" -eq 0 ]; then
    print_success "${MESSAGES["$ret"]}"
else
    print_error "${MESSAGES["$ret"]}"
fi

exit "$ret"
