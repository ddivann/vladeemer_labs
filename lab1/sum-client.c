#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

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

static char *i64_to_str(long long v, char *dst) {
    if (v == 0) { *dst++ = '0'; return dst; }
    bool neg = v < 0;
    unsigned long long x = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    char tmp[32];
    int i = 0;
    while (x > 0) { tmp[i++] = (char)('0' + (x % 10)); x /= 10; }
    if (neg) *dst++ = '-';
    while (i--) *dst++ = tmp[i];
    return dst;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        const char msg[] = "usage: sum-client <output_file>\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out == -1) {
        const char msg[] = "error: failed to open output file\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    char inbuf[8192];
    size_t in_len = 0;

    for (;;) {
        // Fill buffer
        if (in_len < sizeof(inbuf)) {
            ssize_t r = read(STDIN_FILENO, inbuf + in_len, sizeof(inbuf) - in_len);
            if (r < 0) {
                if (errno == EINTR) continue;
                const char msg[] = "error: failed to read from stdin\n";
                write_all(STDERR_FILENO, msg, sizeof(msg)-1);
                close(out);
                _exit(EXIT_FAILURE);
            }
            if (r == 0) {
                break;
            }
            in_len += (size_t)r;
        }

        size_t start = 0;
        for (size_t i = 0; i < in_len; ++i) {
            if (inbuf[i] == '\n') {
                size_t linelen = i - start; // not including \n
                if (linelen == 0) {
                    close(out);
                    _exit(EXIT_SUCCESS);
                }

                long long sum = 0;
                char *p = inbuf + start;
                char *end = inbuf + i;
                while (p < end) {
                    while (p < end && isspace((unsigned char)*p)) p++;
                    if (p >= end) break;
                    errno = 0;
                    char *q = NULL;
                    long val = strtol(p, &q, 10);
                    if (q == p) {
                        p++;
                        continue;
                    }
                    if (errno == ERANGE) {
                        const char msg[] = "warning: integer out of range, clamped\n";
                        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
                    }
                    sum += (long long)val;
                    p = q;
                }

                char outbuf[64];
                char *w = outbuf;
                w = i64_to_str(sum, w);
                *w++ = '\n';
                write_all(out, outbuf, (size_t)(w - outbuf));

                // Move to next line
                start = i + 1;
            }
        }

        if (start > 0) {
            // Move leftover to beginning
            size_t rem = in_len - start;
            memmove(inbuf, inbuf + start, rem);
            in_len = rem;
        } else if (in_len == sizeof(inbuf)) {
            // Line too long without newline; drop buffer (or could error)
            const char msg[] = "warning: input line too long, truncating\n";
            write_all(STDERR_FILENO, msg, sizeof(msg)-1);
            in_len = 0;
        }
    }

    close(out);
    return EXIT_SUCCESS;
}
