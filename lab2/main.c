/*
 * Лабораторная работа №2: Многопоточная обработка данных
 * Задание: Рассчитать экспериментально (методом Монте-Карло) вероятность
 * того, что две верхние карты в колоде из 52 карт одинаковые (по значению).

 * Компиляция:
 *   gcc -o monte-carlo-mutex main.c -pthread -DUSE_MUTEX
 *   gcc -o monte-carlo-atomic main.c -pthread -DUSE_ATOMIC
 * 
 * Запуск:
 *   ./monte-carlo-mutex -r <rounds> -t <max_threads>
 *   ./monte-carlo-atomic -r <rounds> -t <max_threads>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef USE_ATOMIC
#include <stdatomic.h>
#endif

#define DECK_SIZE 52
#define CARD_VALUES 13  // A, 2-10, J, Q, K

// Глобальные переменные для подсчета результатов
#ifdef USE_ATOMIC
atomic_long total_matches = 0;
atomic_long total_simulations = 0;
#else
long total_matches = 0;
long total_simulations = 0;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

// Структура для передачи параметров в поток
typedef struct {
    long simulations_per_thread;
    int thread_id;
} thread_args_t;

// Семафор для ограничения количества потоков
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int max;
} semaphore_t;

semaphore_t thread_limit_sem;

// Функции для работы с семафором
void sem_init_custom(semaphore_t *sem, int max) {
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->count = max;
    sem->max = max;
}

void sem_wait_custom(semaphore_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    while (sem->count == 0) {
        pthread_cond_wait(&sem->cond, &sem->mutex);
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
}

void sem_post_custom(semaphore_t *sem) {
    pthread_mutex_lock(&sem->mutex);
    sem->count++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
}

void sem_destroy_custom(semaphore_t *sem) {
    pthread_mutex_destroy(&sem->mutex);
    pthread_cond_destroy(&sem->cond);
}

// Функция для перемешивания колоды (алгоритм Фишера-Йетса)
void shuffle_deck(int *deck, unsigned int *seed) {
    for (int i = DECK_SIZE - 1; i > 0; i--) {
        int j = rand_r(seed) % (i + 1);
        int temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}  

// Функция проверки: две верхние карты имеют одинаковое значение?
int check_top_two_match(int *deck) {
    // Карты от 0 до 51, значение карты = card % CARD_VALUES
    int card1_value = deck[0] % CARD_VALUES;
    int card2_value = deck[1] % CARD_VALUES;
    return (card1_value == card2_value);
}

// Функция выполнения одной симуляции
int simulate_once(unsigned int *seed) {
    int deck[DECK_SIZE];
    
    // Инициализация колоды (0-51: 4 масти по 13 карт)
    for (int i = 0; i < DECK_SIZE; i++) {
        deck[i] = i;
    }
    
    // Перемешиваем колоду
    shuffle_deck(deck, seed);
    
    // Проверяем, совпадают ли две верхние карты по значению
    return check_top_two_match(deck);
}

// Функция потока
void *thread_function(void *arg) {
    thread_args_t *args = (thread_args_t *)arg;
    unsigned int seed = time(NULL) + args->thread_id;
    
    long local_matches = 0;
    
    // Выполняем симуляции
    for (long i = 0; i < args->simulations_per_thread; i++) {
        if (simulate_once(&seed)) {
            local_matches++;
        }
    }
    
    // Обновляем глобальные счетчики
#ifdef USE_ATOMIC
    atomic_fetch_add(&total_matches, local_matches);
    atomic_fetch_add(&total_simulations, args->simulations_per_thread);
#else
    pthread_mutex_lock(&stats_mutex);
    total_matches += local_matches;
    total_simulations += args->simulations_per_thread;
    pthread_mutex_unlock(&stats_mutex);
#endif
    
    free(args);
    return NULL;
}

// Функция для получения текущего времени в микросекундах
long long current_time_micros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

void print_usage(const char *program_name) {
    printf("Использование: %s -r <rounds> -t <max_threads>\n", program_name);
    printf("  -r <rounds>       Количество раундов (симуляций) Монте-Карло\n");
    printf("  -t <max_threads>  Максимальное количество одновременно работающих потоков\n");
    printf("\nПример: %s -r 1000000 -t 4\n", program_name);
}

int main(int argc, char *argv[]) {
    long total_rounds = 0;
    int max_threads = 0;
    
    // Парсинг аргументов командной строки
    int opt;
    while ((opt = getopt(argc, argv, "r:t:h")) != -1) {
        switch (opt) {
            case 'r':
                total_rounds = atol(optarg);
                break;
            case 't':
                max_threads = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Проверка параметров
    if (total_rounds <= 0 || max_threads <= 0) {
        fprintf(stderr, "Ошибка: параметры -r и -t должны быть положительными числами\n");
        print_usage(argv[0]);
        return 1;
    }
    
    printf("=== Симуляция методом Монте-Карло ===\n");
    printf("Тип синхронизации: %s\n", 
#ifdef USE_ATOMIC
           "Атомарные операции (atomic)"
#else
           "Мьютексы (mutex)"
#endif
    );
    printf("Количество раундов: %ld\n", total_rounds);
    printf("Максимум потоков: %d\n", max_threads);
    printf("PID процесса: %d\n", getpid());
    printf("\n");
    
    // Инициализация семафора для ограничения потоков
    sem_init_custom(&thread_limit_sem, max_threads);
    
    // Начало замера времени
    long long start_time = current_time_micros();
    
    // Создание потоков
    long simulations_per_thread = total_rounds / max_threads;
    long remaining = total_rounds % max_threads;
    
    pthread_t threads[max_threads];
    
    for (int i = 0; i < max_threads; i++) {
        sem_wait_custom(&thread_limit_sem);
        
        thread_args_t *args = malloc(sizeof(thread_args_t));
        args->thread_id = i;
        args->simulations_per_thread = simulations_per_thread;
        
        // Добавляем оставшиеся раунды к последнему потоку
        if (i == max_threads - 1) {
            args->simulations_per_thread += remaining;
        }
        
        if (pthread_create(&threads[i], NULL, thread_function, args) != 0) {
            fprintf(stderr, "Ошибка создания потока %d\n", i);
            free(args);
            sem_post_custom(&thread_limit_sem);
            return 1;
        }
    }
    
    // Ожидание завершения всех потоков
    for (int i = 0; i < max_threads; i++) {
        pthread_join(threads[i], NULL);
        sem_post_custom(&thread_limit_sem);
    }
    
    // Конец замера времени
    long long end_time = current_time_micros();
    double elapsed_seconds = (end_time - start_time) / 1000000.0;
    
    // Вычисление и вывод результатов
#ifdef USE_ATOMIC
    long final_matches = atomic_load(&total_matches);
    long final_simulations = atomic_load(&total_simulations);
#else
    long final_matches = total_matches;
    long final_simulations = total_simulations;
#endif
    
    double probability = (double)final_matches / final_simulations;
    
    printf("=== Результаты ===\n");
    printf("Всего симуляций: %ld\n", final_simulations);
    printf("Совпадений: %ld\n", final_matches);
    printf("Экспериментальная вероятность: %.6f (%.4f%%)\n", 
           probability, probability * 100);
    printf("Время выполнения: %.3f сек\n", elapsed_seconds);
    printf("Скорость: %.0f симуляций/сек\n", final_simulations / elapsed_seconds);
    
    // Теоретическая вероятность
    // P = (количество способов выбрать 2 карты одного значения) / (всего способов выбрать 2 карты)
    // Для 52 карт (4 масти, 13 значений):
    // Первая карта - любая (52/52)
    // Вторая карта должна совпадать по значению с первой (осталось 3 такие карты из 51)
    // P = (52/52) * (3/51) = 3/51 ≈ 0.0588
    double theoretical_probability = 3.0 / 51.0;
    printf("\nТеоретическая вероятность: %.6f (%.4f%%)\n", 
           theoretical_probability, theoretical_probability * 100);
    printf("Отклонение: %.6f (%.4f%%)\n", 
           probability - theoretical_probability, 
           (probability - theoretical_probability) * 100);
    
    // Очистка ресурсов
    sem_destroy_custom(&thread_limit_sem);
#ifndef USE_ATOMIC
    pthread_mutex_destroy(&stats_mutex);
#endif
    
    return 0;
}
