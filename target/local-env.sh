#!/usr/bin/env bash

# Guest-side environment for manually overlaid user payloads. Buildroot
# packages normally live under /usr, while Meson/autotools payloads staged
# through DESTDIR commonly keep their default /usr/local prefix.

add_path()
{
    [ -d "$1" ] || return 0

    case ":$PATH:" in
        *":$1:"*) ;;
        *) PATH="${PATH:+$PATH:}$1" ;;
    esac
}

add_library_path()
{
    [ -d "$1" ] || return 0

    case ":${LD_LIBRARY_PATH:-}:" in
        *":$1:"*) ;;
        *) LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$1" ;;
    esac
}

add_path /usr/local/bin
add_library_path /usr/local/lib

export PATH
export LD_LIBRARY_PATH
