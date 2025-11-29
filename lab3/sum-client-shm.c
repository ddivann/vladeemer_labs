#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>
#include <semaphore.h>

#define SHM_NAME "/sum_shm"
#define SEM_DATA_READY "/sum_data_ready"
#define SEM_PROCESSED "/sum_processed"
#define BUF_SIZE 8192

typedef struct {
    char buf[BUF_SIZE];
    size_t len;
    int eof;
} shared_data_t;

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
    // returns pointer past last written char (no NUL added)
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
        const char msg[] = "usage: sum-client-shm <output_file>\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    int out = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out == -1) {
        const char msg[] = "error: failed to open output file\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    // Open shared memory
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (shm_fd == -1) {
        const char msg[] = "error: failed to open shared memory\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        close(out);
        _exit(EXIT_FAILURE);
    }

    shared_data_t *data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        const char msg[] = "error: failed to mmap shared memory\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        close(shm_fd);
        close(out);
        _exit(EXIT_FAILURE);
    }
    close(shm_fd);

    // Open semaphores
    sem_t *sem_data_ready = sem_open(SEM_DATA_READY, 0);
    sem_t *sem_processed = sem_open(SEM_PROCESSED, 0);
    if (sem_data_ready == SEM_FAILED || sem_processed == SEM_FAILED) {
        const char msg[] = "error: failed to open semaphores\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        munmap(data, sizeof(shared_data_t));
        close(out);
        _exit(EXIT_FAILURE);
    }

    char inbuf[BUF_SIZE];
    size_t in_len = 0;

    for (;;) {
        // Wait for data
        if (sem_wait(sem_data_ready) == -1) {
            if (errno == EINTR) continue;
            const char msg[] = "error: sem_wait failed\n";
            write_all(STDERR_FILENO, msg, sizeof(msg)-1);
            break;
        }

        if (data->eof) {
            // EOF
            break;
        }

        // Copy data to local buffer
        size_t to_copy = data->len;
        if (in_len + to_copy > sizeof(inbuf)) {
            to_copy = sizeof(inbuf) - in_len;
        }
        memcpy(inbuf + in_len, data->buf, to_copy);
        in_len += to_copy;

        // Signal processed
        sem_post(sem_processed);

        // Process complete lines
        size_t start = 0;
        for (size_t i = 0; i < in_len; ++i) {
            if (inbuf[i] == '\n') {
                size_t linelen = i - start; // not including \n
                if (linelen == 0) {
                    // Empty line => finish
                    close(out);
                    munmap(data, sizeof(shared_data_t));
                    sem_close(sem_data_ready);
                    sem_close(sem_processed);
                    _exit(EXIT_SUCCESS);
                }

                // Parse ints and compute sum
                long long sum = 0;
                char *p = inbuf + start;
                char *end = inbuf + i;
                while (p < end) {
                    // skip spaces
                    while (p < end && isspace((unsigned char)*p)) p++;
                    if (p >= end) break;
                    errno = 0;
                    char *q = NULL;
                    long val = strtol(p, &q, 10);
                    if (q == p) {
                        // invalid token, skip one char
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

                // Write sum followed by newline to file
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
    munmap(data, sizeof(shared_data_t));
    sem_close(sem_data_ready);
    sem_close(sem_processed);
    return EXIT_SUCCESS;
}
