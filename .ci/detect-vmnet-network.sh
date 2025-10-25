#!/usr/bin/env bash

# Detect vmnet shared network parameters (gateway IP, prefix)
# Outputs: "<guest_ip> <gateway_ip> <prefix_len>"

set -euo pipefail

timeout="${VMNET_DETECT_TIMEOUT:-30}"
guest_octet="${VMNET_GUEST_HOST_OCTET:-10}"
guest_ip_override="${VMNET_GUEST_IP_OVERRIDE:-}"
target_bridge="${VMNET_BRIDGE:-}"

convert_netmask_to_prefix() {
    local mask="$1"
    case "${mask}" in
        0xffffff00) echo 24 ;;
        0xfffffe00) echo 23 ;;
        0xfffffc00) echo 22 ;;
        0xfffff800) echo 21 ;;
        0xfffff000) echo 20 ;;
        0xffffe000) echo 19 ;;
        0xffffc000) echo 18 ;;
        0xffff8000) echo 17 ;;
        0xffff0000) echo 16 ;;
        0xfffe0000) echo 15 ;;
        0xfffc0000) echo 14 ;;
        0xfff80000) echo 13 ;;
        0xfff00000) echo 12 ;;
        0xffe00000) echo 11 ;;
        0xffc00000) echo 10 ;;
        0xff800000) echo 9 ;;
        0xff000000) echo 8 ;;
        *) echo 24 ;;
    esac
}

detect_bridge() {
    local candidates=""
    if [[ -n "${target_bridge}" ]]; then
        candidates="${target_bridge}"
    else
        candidates="$(ifconfig -l)"
    fi

    local candidate
    for candidate in ${candidates}; do
        if [[ -n "${target_bridge}" ]] || [[ "${candidate}" =~ ^bridge[0-9]+$ ]]; then
            local inet_line
            inet_line="$(ifconfig "${candidate}" 2>/dev/null | awk '/inet /{print $0; exit}')"
            if [[ -n "${inet_line}" ]]; then
                echo "${candidate}|${inet_line}"
                return 0
            fi
        fi
    done

    return 1
}

while (( timeout > 0 )); do
    if bridge_info="$(detect_bridge)"; then
        bridge_name="${bridge_info%%|*}"
        inet_line="${bridge_info#*|}"

        gateway="$(awk '{print $2}' <<<"${inet_line}")"
        netmask="$(awk '{print $4}' <<<"${inet_line}")"
        prefix="$(convert_netmask_to_prefix "${netmask}")"

        IFS=. read -r o1 o2 o3 o4 <<<"${gateway}"
        if [[ -z "${o1:-}" || -z "${o2:-}" || -z "${o3:-}" ]]; then
            echo "Error: Unexpected gateway format: ${gateway}" >&2
            exit 1
        fi

        guest_ip="${guest_ip_override}"
        if [[ -z "${guest_ip}" ]]; then
            guest_ip="${o1}.${o2}.${o3}.${guest_octet}"
        fi

        printf "%s %s %s\n" "${guest_ip}" "${gateway}" "${prefix}"
        exit 0
    fi

    sleep 1
    timeout=$((timeout - 1))
done

echo "Error: vmnet gateway not detected within timeout" >&2
exit 1
