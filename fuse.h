#include "riscv.h"
#include "virtio.h"

#define FUSE_REC_ALIGN(x) \
    (((x) + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1))
#define FUSE_DIRENT_ALIGN(x) FUSE_REC_ALIGN(x)

struct fuse_in_header {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
};

struct fuse_out_header {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};

struct vfs_req_header {
    struct fuse_in_header in;
};

struct vfs_resp_header {
    struct fuse_out_header out;
};

struct fuse_init_in {
    /* FUSE major version supported by the guest (typically 7) */
    uint32_t major;
    /* FUSE minor version supported by the guest (e.g., 31, 26) */
    uint32_t minor;
    uint32_t max_readahead; /* Maximum readahead size supported by the guest */
    uint32_t flags;         /* Flags requested by the guest */
};

struct fuse_init_out {
    uint32_t major;         /* FUSE major version supported by the device */
    uint32_t minor;         /* FUSE minor version supported by the device */
    uint32_t max_readahead; /* Maximum readahead size accepted by the device */
    /* Flags supported by the device (negotiated with the guest) */
    uint32_t flags;
    uint16_t max_background; /* Maximum number of background requests */
    uint16_t congestion_threshold;
    uint32_t max_write;  /* Maximum write size the device can handle */
    uint32_t time_gran;  /* Time granularity (in nanoseconds) */
    uint32_t unused[11]; /* Reserved */
};

struct fuse_getattr_in {
    /* bitmask for valid fields (e.g. FUSE_GETATTR_FH) */
    uint32_t getattr_flags;
    uint32_t padding; /* unused, reserved for alignment */
    uint64_t fh;      /* optional: file handle (used when getattr_flags has */
};

struct fuse_attr {
    uint64_t ino;       /* inode number */
    uint64_t size;      /* file size in bytes */
    uint64_t blocks;    /* number of 512B blocks allocated */
    uint64_t atime;     /* last access time (UNIX time) */
    uint64_t mtime;     /* last modification time */
    uint64_t ctime;     /* last status change time */
    uint32_t atimensec; /* nanoseconds part */
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;    /* file mode (e.g. S_IFDIR | 0755) */
    uint32_t nlink;   /* number of hard links */
    uint32_t uid;     /* owner uid */
    uint32_t gid;     /* owner gid */
    uint32_t rdev;    /* device ID (if special file) */
    uint32_t blksize; /* block size */
    uint32_t flags;   /* reserved */
};

struct fuse_attr_out {
    uint64_t attr_valid;      /* seconds the attributes are valid */
    uint32_t attr_valid_nsec; /* nanoseconds part of attr_valid */
    uint32_t dummy;           /* padding for alignment */
    struct fuse_attr attr;    /* actual attributes */
};

struct fuse_open_in {
    uint32_t flags;
    uint32_t open_flags;
};

struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    int32_t backing_id;
};

struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_entry_out {
    uint64_t nodeid;           /* inode number */
    uint64_t generation;       /* inode generation */
    uint64_t entry_valid;      /* cache timeout (sec) */
    uint64_t attr_valid;       /* attr cache timeout (sec) */
    uint32_t entry_valid_nsec; /* cache timeout (nsec) */
    uint32_t attr_valid_nsec;  /* attr cache timeout (nsec) */
    struct fuse_attr attr;     /* file attributes */
};

struct fuse_dirent {
    uint64_t ino;     /* inode number */
    uint64_t off;     /* offset to next entry */
    uint32_t namelen; /* length of the entry name */
    uint32_t type;    /* file type (DT_REG, DT_DIR, etc.) */
    char name[];      /* name (not null-terminated) */
};

struct fuse_direntplus {
    struct fuse_entry_out entry_out;
    struct fuse_dirent dirent;
};

struct fuse_lookup_in {
    uint64_t parent; /* inode of parent dir */
};

struct fuse_forget_in {
    uint64_t nlookup;
};

struct fuse_create_in {
    uint32_t flags;
    uint32_t mode;
    uint32_t umask;
    uint32_t open_flags;
};

struct fuse_release_in {
    uint64_t fh;
    uint32_t flags;
    uint32_t release_flags;
    uint64_t lock_owner;
};

struct fuse_flush_in {
    uint64_t fh;
    uint32_t unused;
    uint32_t padding;
    uint64_t lock_owner;
};