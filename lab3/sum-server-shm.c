#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
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

static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0; // EOF
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return (int)n;
}

int main(int argc, char **argv) {
    // 1) Read output filename from parent's stdin (first line)
    char filename[1024];
    int rl = read_line(STDIN_FILENO, filename, sizeof(filename));
    if (rl <= 0) {
        const char msg[] = "error: expected output filename on first line\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
    // strip trailing newline
    if (filename[rl-1] == '\n') filename[rl-1] = '\0';

    // Create shared memory
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (shm_fd == -1) {
        const char msg[] = "error: failed to create shared memory\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
    if (ftruncate(shm_fd, sizeof(shared_data_t)) == -1) {
        const char msg[] = "error: failed to ftruncate shared memory\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    shared_data_t *data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        const char msg[] = "error: failed to mmap shared memory\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }
    close(shm_fd);

    sem_t *sem_data_ready = sem_open(SEM_DATA_READY, O_CREAT, 0600, 0);
    sem_t *sem_processed = sem_open(SEM_PROCESSED, O_CREAT, 0600, 0);
    if (sem_data_ready == SEM_FAILED || sem_processed == SEM_FAILED) {
        const char msg[] = "error: failed to create semaphores\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        munmap(data, sizeof(shared_data_t));
        shm_unlink(SHM_NAME);
        return EXIT_FAILURE;
    }

    pid_t child = fork();
    if (child < 0) {
        const char msg[] = "error: failed to fork\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        munmap(data, sizeof(shared_data_t));
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_DATA_READY);
        sem_unlink(SEM_PROCESSED);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        // Child: exec sum-client-shm
        munmap(data, sizeof(shared_data_t)); // child will mmap again

        char *const args[] = { (char*)"sum-client-shm", filename, NULL };
        execv("./sum-client-shm", args);

        const char msg[] = "error: failed to exec sum-client-shm\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    // Parent
    char inbuf[BUF_SIZE];
    size_t in_len = 0;

    for (;;) {
        // Fill buffer
        if (in_len < sizeof(inbuf)) {
            ssize_t r = read(STDIN_FILENO, inbuf + in_len, sizeof(inbuf) - in_len);
            if (r < 0) {
                if (errno == EINTR) continue;
                const char msg[] = "error: failed to read from stdin\n";
                write_all(STDERR_FILENO, msg, sizeof(msg)-1);
                break;
            }
            if (r == 0) {
                // EOF
                data->len = 0;
                data->eof = 1;
                sem_post(sem_data_ready);
                break;
            }
            in_len += (size_t)r;
        }

        // Send data to shared memory
        data->len = in_len;
        data->eof = 0;
        memcpy(data->buf, inbuf, in_len);
        in_len = 0;

        // Signal data ready
        sem_post(sem_data_ready);

        // Wait for processed
        if (sem_wait(sem_processed) == -1) {
            if (errno == EINTR) continue;
            const char msg[] = "error: sem_wait processed failed\n";
            write_all(STDERR_FILENO, msg, sizeof(msg)-1);
            break;
        }
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        const char msg[] = "error: waitpid failed\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        munmap(data, sizeof(shared_data_t));
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_DATA_READY);
        sem_unlink(SEM_PROCESSED);
        return EXIT_FAILURE;
    }

    munmap(data, sizeof(shared_data_t));
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_DATA_READY);
    sem_unlink(SEM_PROCESSED);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return EXIT_SUCCESS;
    } else {
        const char msg[] = "error: child exited with error\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
}
