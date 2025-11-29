#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            _exit(EXIT_FAILURE);
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0;
        
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return (int)n;
}

int main(int argc, char **argv) {
    char filename[1024];
    int rl = read_line(STDIN_FILENO, filename, sizeof(filename));
    if (rl <= 0) {
        const char msg[] = "error: expected output filename on first line\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
    if (filename[rl-1] == '\n') filename[rl-1] = '\0';

    int p2c[2];
    int c2p[2];
    if (pipe(p2c) == -1 || pipe(c2p) == -1) {
        const char msg[] = "error: failed to create pipes\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    pid_t child = fork();
    if (child < 0) {
        const char msg[] = "error: failed to fork\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        if (dup2(p2c[0], STDIN_FILENO) == -1) _exit(EXIT_FAILURE);
        if (dup2(c2p[1], STDOUT_FILENO) == -1) _exit(EXIT_FAILURE);
        
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);

        char *const args[] = { (char*)"sum-client-clean", filename, NULL };
        
        execv("./sum-client-clean", args);

        const char msg[] = "error: failed to exec sum-client\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    close(p2c[0]);
    close(c2p[1]);

    char buf[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            const char msg[] = "error: failed to read from stdin\n";
            write_all(STDERR_FILENO, msg, sizeof(msg)-1);
            break;
        }
        if (r == 0) break;
        write_all(p2c[1], buf, (size_t)r);
    }
    close(p2c[1]);

    for (;;) {
        ssize_t r = read(c2p[0], buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        write_all(STDOUT_FILENO, buf, (size_t)r);
    }
    close(c2p[0]);

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        const char msg[] = "error: waitpid failed\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return EXIT_SUCCESS;
    } else {
        const char msg[] = "error: child exited with error\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
}