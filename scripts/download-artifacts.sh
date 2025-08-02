#!/bin/sh

BASE_URL="https://github.com/sysprog21/semu/raw/blob"

IMAGE_SHA1="72a8454575508d09210f067b25239cd51b929d27"
ROOTFS_SHA1="18bd639c17fc299c317f8fed1c845e7dda296748"

DEST_DIR="./build"

if [ -z "$1" ]; then
    echo "No file name provided. Please provide a target as the first argument."
    exit 1
fi

case "$1" in
    "Image")
        FULL_URL="${BASE_URL}/Image.bz2"
        LOCAL_FILE_PATH="${DEST_DIR}/Image"
        FILE_SHA1="${IMAGE_SHA1}"
        ;;
    "rootfs")
        FULL_URL="${BASE_URL}/rootfs.cpio.bz2"
        LOCAL_FILE_PATH="${DEST_DIR}/rootfs.cpio"
        FILE_SHA1="${ROOTFS_SHA1}"
        ;;
    *)
        echo "Invalid target: \"$1\""
        echo "Available keywords are: Image, rootfs"
        exit 1
        ;;
esac

if [ ! -z "$2" ]; then
    DEST_DIR="$2" 
else
    echo "No destination directory provided. Using the default one: \"$DEST_DIR\""
fi

if [ ! -d "$DEST_DIR" ]; then
    mkdir -p "$DEST_DIR"
fi

if [ ! -f "${LOCAL_FILE_PATH}.bz2" ]; then
    NEED_REDOWNLOAD="1"

    echo "The file \"${LOCAL_FILE_PATH}.bz2\" was not found. Downloading..."
else
    SHA1_CHECK_RES=$(echo "${FILE_SHA1} ${LOCAL_FILE_PATH}.bz2" | sha1sum -c - >/dev/null 2>&1 || echo "failed")

    if [ "$SHA1_CHECK_RES" = "failed" ]; then
        NEED_REDOWNLOAD="1"

        echo "Local file \"${LOCAL_FILE_PATH}.bz2\" already exists. Checking SHA1... Failed"

        # if the existing file is larger, the extra part would be ignored by "curl -C".
        rm "${LOCAL_FILE_PATH}.bz2"

        echo "Re-downloading file..."
    else
        echo "Local file \"${LOCAL_FILE_PATH}.bz2\" already exists. Checking SHA1... Passed"
        echo "File is up-to-date. Skipping download."
    fi
fi

if [ ! -z "$NEED_REDOWNLOAD" ]; then
    curl --progress-bar -L -C - "$FULL_URL" -o "${LOCAL_FILE_PATH}.bz2"

    SHA1_CHECK_RES=$(echo "${FILE_SHA1} ${LOCAL_FILE_PATH}.bz2" | sha1sum -c - >/dev/null 2>&1 || echo "failed")

    if [ "$SHA1_CHECK_RES" = "failed" ]; then
        echo "Checking SHA1... Failed"
        echo "Error: The SHA1 might be outdated. Please update your local copy and try again."
        exit 1
    else
        echo "Checking SHA1... Passed"
    fi
fi

echo "Extracting from \"${LOCAL_FILE_PATH}.bz2\""

bunzip2 --keep "${LOCAL_FILE_PATH}.bz2" >/dev/null 2>&1

if [ ! -f "$LOCAL_FILE_PATH" ]; then
    echo "Something went wrong! The file \"$LOCAL_FILE_PATH\" was not found."
    exit 1
else
    echo "Completed!"
fi
