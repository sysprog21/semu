#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "console.h"

#define CONSOLE_ESCAPE 1 /* Ctrl-a */

static struct termios saved_termios;
static int terminal_fd = -1;
static int output_fd = -1;
static bool escape_pending;

static void host_console_restore(void)
{
    if (terminal_fd >= 0)
        tcsetattr(terminal_fd, TCSANOW, &saved_termios);
}

void host_console_setup(int in_fd, int out_fd)
{
    struct termios termios;

    output_fd = out_fd;
    if (!isatty(in_fd) || tcgetattr(in_fd, &saved_termios) < 0)
        return;

    termios = saved_termios;
    termios.c_lflag &= ~(ICANON | ECHO | ISIG);
    if (tcsetattr(in_fd, TCSANOW, &termios) < 0)
        return;

    terminal_fd = in_fd;
    atexit(host_console_restore);
}

ssize_t host_console_read(int fd, void *buf, size_t len)
{
    ssize_t nread = read(fd, buf, len);
    uint8_t *bytes = buf;

    if (nread <= 0)
        return nread;

    for (ssize_t i = 0; i < nread; i++) {
        if (escape_pending && bytes[i] == 'x') {
            if (output_fd >= 0) {
                ssize_t written = write(output_fd, "\n", 1);
                (void) written;
            }
            exit(0);
        }
        escape_pending = bytes[i] == CONSOLE_ESCAPE;
    }

    return nread;
}
