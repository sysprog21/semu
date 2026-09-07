#pragma once

#include <stddef.h>
#include <sys/types.h>

void host_console_setup(int in_fd, int out_fd);
ssize_t host_console_read(int fd, void *buf, size_t len);
