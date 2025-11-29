#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

// --- Вспомогательные функции ---

/**
 * @brief Гарантированно записывает все данные из буфера в файловый дескриптор.
 * 
 * Системный вызов write() не гарантирует, что все запрошенные байты будут записаны
 * за один раз. Эта функция-обертка продолжает вызывать write() в цикле, пока
 * все данные не будут отправлены. Она также обрабатывает прерывание вызова сигналом (EINTR).
 * 
 * @param fd Файловый дескриптор для записи.
 * @param buf Указатель на буфер с данными.
 * @param len Количество байт для записи.
 */
static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            // Если запись была прервана сигналом, просто повторяем попытку.
            if (errno == EINTR) continue;
            // В случае другой ошибки, завершаем процесс.
            _exit(EXIT_FAILURE);
        }
        // Сдвигаем указатель и уменьшаем оставшееся количество байт.
        buf += (size_t)n;
        len -= (size_t)n;
    }
}

/**
 * @brief Читает одну строку из файлового дескриптора.
 * 
 * Функция читает данные по одному байту до тех пор, пока не встретит символ
 * новой строки ('\n') или пока не будет достигнут конец файла (EOF).
 * В конец строки добавляется нулевой символ для корректной работы со строковыми функциями.
 * 
 * @param fd Файловый дескриптор для чтения.
 * @param buf Буфер для сохранения строки.
 * @param cap Максимальная вместимость буфера.
 * @return Количество прочитанных байт, 0 при EOF, -1 при ошибке.
 */
static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    // Читаем побайтно, пока есть место в буфере (оставляя место для '\0').
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            // Если чтение было прервано сигналом, повторяем.
            if (errno == EINTR) continue;
            return -1; // Ошибка чтения.
        }
        if (r == 0) return 0; // Достигнут конец файла (EOF).
        
        buf[n++] = c;
        // Если прочитали символ новой строки, строка закончилась.
        if (c == '\n') break;
    }
    // Завершаем строку нулевым символом.
    buf[n] = '\0';
    return (int)n;
}

// --- Основная логика программы ---

int main(int argc, char **argv) {
    // --- Этап 1: Чтение имени файла для вывода ---
    // Сервер ожидает, что первая строка, полученная из стандартного ввода,
    // будет содержать имя файла, в который клиент запишет результат.
    char filename[1024];
    int rl = read_line(STDIN_FILENO, filename, sizeof(filename));
    if (rl <= 0) {
        const char msg[] = "error: expected output filename on first line\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
    // Убираем символ новой строки ('\n') с конца имени файла.
    if (filename[rl-1] == '\n') filename[rl-1] = '\0';

    // --- Этап 2: Создание каналов для межпроцессного взаимодействия ---
    // p2c: parent-to-child (родитель -> ребенок)
    // c2p: child-to-parent (ребенок -> родитель)
    int p2c[2];
    int c2p[2];
    if (pipe(p2c) == -1 || pipe(c2p) == -1) {
        const char msg[] = "error: failed to create pipes\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    // --- Этап 3: Создание дочернего процесса ---
    pid_t child = fork();
    if (child < 0) {
        const char msg[] = "error: failed to fork\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    // --- Логика дочернего процесса ---
    if (child == 0) {
        // --- Этап 3.1: Перенаправление стандартных потоков ввода/вывода ---
        // Стандартный ввод дочернего процесса (stdin) перенаправляем на чтение из канала p2c.
        if (dup2(p2c[0], STDIN_FILENO) == -1) _exit(EXIT_FAILURE);
        // Стандартный вывод (stdout) перенаправляем на запись в канал c2p.
        if (dup2(c2p[1], STDOUT_FILENO) == -1) _exit(EXIT_FAILURE);
        
        // --- Этап 3.2: Закрытие всех файловых дескрипторов каналов ---
        // После dup2 оригинальные дескрипторы больше не нужны в дочернем процессе.
        close(p2c[0]); close(p2c[1]);
        close(c2p[0]); close(c2p[1]);

        // --- Этап 3.3: Запуск sum-client ---
        // Аргументы для sum-client: имя программы и имя выходного файла.
        char *const args[] = { (char*)"sum-client", filename, NULL };
        
        // Пытаемся запустить sum-client (пробуем разные варианты).
        execv("./bin/sum-client", args);
        //execv("./sum-client", args);
        //execv("sum-client", args);

        // Если все попытки execv провалились, выводим ошибку и завершаемся.
        const char msg[] = "error: failed to exec sum-client\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        _exit(EXIT_FAILURE);
    }

    // --- Логика родительского процесса ---
    // --- Этап 4.1: Закрытие неиспользуемых концов каналов ---
    // Родитель пишет в p2c, поэтому закрывает конец для чтения.
    close(p2c[0]);
    // Родитель читает из c2p, поэтому закрывает конец для записи.
    close(c2p[1]);

    // --- Этап 4.2: Пересылка данных из stdin в дочерний процесс ---
    char buf[4096];
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            const char msg[] = "error: failed to read from stdin\n";
            write_all(STDERR_FILENO, msg, sizeof(msg)-1);
            break;
        }
        // Если достигнут конец ввода, выходим из цикла.
        if (r == 0) break;
        // Записываем прочитанные данные в канал, ведущий к дочернему процессу.
        write_all(p2c[1], buf, (size_t)r);
    }
    // Закрываем конец для записи в канал p2c. Это пошлет сигнал EOF дочернему процессу.
    close(p2c[1]);

    // --- Этап 4.3: Чтение результата от дочернего процесса и вывод в stdout ---
    for (;;) {
        ssize_t r = read(c2p[0], buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break; // Дочерний процесс закрыл свой конец канала.
        // Выводим полученные данные на стандартный вывод.
        write_all(STDOUT_FILENO, buf, (size_t)r);
    }
    // Закрываем конец для чтения из канала c2p.
    close(c2p[0]);

    // --- Этап 5: Ожидание завершения дочернего процесса и проверка статуса ---
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        const char msg[] = "error: waitpid failed\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }

    // Проверяем, завершился ли дочерний процесс успешно.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return EXIT_SUCCESS;
    } else {
        const char msg[] = "error: child exited with error\n";
        write_all(STDERR_FILENO, msg, sizeof(msg)-1);
        return EXIT_FAILURE;
    }
}
