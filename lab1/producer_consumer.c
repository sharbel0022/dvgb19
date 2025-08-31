// DVGB01 Lab 1 — Producer–Consumer (bounded buffer) med Pthreads
// Windows-kompatibel (ingen sigwait/sigset_t). Avslut via Ctrl-C (SIGINT).
// Kompilera (MSYS2/MinGW):  gcc producer_consumer.c -o pc -lpthread
// Kompilera (Linux/WSL):   gcc producer_consumer.c -o pc -pthread
// Kör: ./pc N BufferSize TimeInterval
// Ex:  ./pc 3 8 1

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  static void sleep_seconds(int s) { if (s > 0) Sleep((DWORD)s * 1000); }
  static void sleep_millis(int ms) { if (ms > 0) Sleep((DWORD)ms); }
#else
  #include <unistd.h>
  static void sleep_seconds(int s) {
      if (s <= 0) return;
      struct timespec req = { .tv_sec = s, .tv_nsec = 0 }, rem;
      while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
  }
  static void sleep_millis(int ms) {
      if (ms <= 0) return;
      struct timespec req = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L }, rem;
      while (nanosleep(&req, &rem) == -1 && errno == EINTR) req = rem;
  }
#endif

typedef struct {
    int *data;
    int size;
    int head;   // dequeue
    int tail;   // enqueue
    int count;

    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    int shutdown; // 0=running, 1=stäng ner
    unsigned long produced_total;
    unsigned long consumed_total;
} ring_buffer_t;

typedef struct {
    int id;
    ring_buffer_t *rb;
    int time_interval; // sekunder (används av producent)
} thread_arg_t;

// Global flagga som sätts av signal-handlern
static volatile sig_atomic_t g_stop_flag = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop_flag = 1;
}

// Tråd: väntar tills g_stop_flag=1, sätter rb->shutdown och broadcast:ar
static void *shutdown_watcher(void *arg) {
    ring_buffer_t *rb = (ring_buffer_t*)arg;
    while (!g_stop_flag) {
        // Sov lite för att inte spinna (10 ms)
        sleep_millis(10);
    }
    pthread_mutex_lock(&rb->mtx);
    if (!rb->shutdown) {
        rb->shutdown = 1;
        printf("\n[Signal] SIGINT mottagen. Påbörjar nedstängning...\n");
        pthread_cond_broadcast(&rb->not_empty);
        pthread_cond_broadcast(&rb->not_full);
    }
    pthread_mutex_unlock(&rb->mtx);
    return NULL;
}

static void rb_init(ring_buffer_t *rb, int size) {
    rb->data  = (int*)malloc(sizeof(int) * size);
    if (!rb->data) { perror("malloc"); exit(EXIT_FAILURE); }
    rb->size  = size;
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    rb->shutdown = 0;
    rb->produced_total = 0;
    rb->consumed_total = 0;

    if (pthread_mutex_init(&rb->mtx, NULL) != 0) { perror("pthread_mutex_init"); exit(EXIT_FAILURE); }
    if (pthread_cond_init(&rb->not_empty, NULL) != 0) { perror("pthread_cond_init not_empty"); exit(EXIT_FAILURE); }
    if (pthread_cond_init(&rb->not_full, NULL) != 0)  { perror("pthread_cond_init not_full");  exit(EXIT_FAILURE); }
}

static void rb_destroy(ring_buffer_t *rb) {
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_mutex_destroy(&rb->mtx);
    free(rb->data);
}

static int rb_enqueue(ring_buffer_t *rb, int value) {
    rb->data[rb->tail] = value;
    rb->tail = (rb->tail + 1) % rb->size;
    rb->count++;
    rb->produced_total++;
    return 0;
}

static int rb_dequeue(ring_buffer_t *rb, int *out) {
    *out = rb->data[rb->head];
    rb->head = (rb->head + 1) % rb->size;
    rb->count--;
    rb->consumed_total++;
    return 0;
}

static void *producer_main(void *arg) {
    thread_arg_t *targ = (thread_arg_t*)arg;
    ring_buffer_t *rb = targ->rb;
    int interval = targ->time_interval;
    int value = 1;

    for (;;) {
        // Om Ctrl-C tryckts: trigga shutdown
        if (g_stop_flag) {
            pthread_mutex_lock(&rb->mtx);
            rb->shutdown = 1;
            pthread_cond_broadcast(&rb->not_empty);
            pthread_cond_broadcast(&rb->not_full);
            pthread_mutex_unlock(&rb->mtx);
            break;
        }

        sleep_seconds(interval);

        pthread_mutex_lock(&rb->mtx);
        if (rb->shutdown) { pthread_mutex_unlock(&rb->mtx); break; }

        while (rb->count == rb->size && !rb->shutdown) {
            pthread_cond_wait(&rb->not_full, &rb->mtx);
        }
        if (rb->shutdown) { pthread_mutex_unlock(&rb->mtx); break; }

        rb_enqueue(rb, value);
        printf("[Producer] +%d (count=%d)\n", value, rb->count);
        value++;

        pthread_cond_signal(&rb->not_empty);
        pthread_mutex_unlock(&rb->mtx);
    }

    printf("[Producer] Stänger.\n");
    return NULL;
}

static void *consumer_main(void *arg) {
    thread_arg_t *targ = (thread_arg_t*)arg;
    ring_buffer_t *rb = targ->rb;
    int id = targ->id;

    for (;;) {
        pthread_mutex_lock(&rb->mtx);

        while (rb->count == 0 && !rb->shutdown) {
            // Om Ctrl-C har tryckts: markera shutdown och väck alla
            if (g_stop_flag) {
                rb->shutdown = 1;
                pthread_cond_broadcast(&rb->not_empty);
                pthread_cond_broadcast(&rb->not_full);
                break;
            }
            pthread_cond_wait(&rb->not_empty, &rb->mtx);
        }

        if (rb->shutdown && rb->count == 0) {
            pthread_mutex_unlock(&rb->mtx);
            break;
        }

        int v = 0;
        rb_dequeue(rb, &v);
        int current = rb->count;
        pthread_cond_signal(&rb->not_full);
        pthread_mutex_unlock(&rb->mtx);

        printf("  [Consumer %d] -%d (count=%d)\n", id, v, current);
        sleep_millis(50); // simulera jobb
    }

    printf("  [Consumer %d] Stänger.\n", id);
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr, "Användning: %s N BufferSize TimeInterval\n", prog);
    fprintf(stderr, "  N           = antal konsumenttrådar (>=1)\n");
    fprintf(stderr, "  BufferSize  = ringbuffer-storlek (>=1)\n");
    fprintf(stderr, "  TimeInterval= sekunder mellan producerade värden (>=0)\n");
}

int main(int argc, char **argv) {
    if (argc != 4) { usage(argv[0]); return EXIT_FAILURE; }

    int N = atoi(argv[1]);
    int BufferSize = atoi(argv[2]);
    int TimeInterval = atoi(argv[3]);
    if (N < 1 || BufferSize < 1 || TimeInterval < 0) { usage(argv[0]); return EXIT_FAILURE; }

    // Installera signalhanterare (finns i både Windows/MinGW och Linux)
    signal(SIGINT, handle_sigint);

    ring_buffer_t rb;
    rb_init(&rb, BufferSize);

    // Starta “shutdown-watcher” som lyssnar på g_stop_flag
    pthread_t shut_thr;
    if (pthread_create(&shut_thr, NULL, shutdown_watcher, &rb) != 0) {
        perror("pthread_create shutdown_watcher");
        rb_destroy(&rb);
        return EXIT_FAILURE;
    }

    // Starta producent
    pthread_t prod;
    thread_arg_t parg = { .id = 0, .rb = &rb, .time_interval = TimeInterval };
    if (pthread_create(&prod, NULL, producer_main, &parg) != 0) {
        perror("pthread_create producer");
        pthread_mutex_lock(&rb.mtx);
        rb.shutdown = 1;
        pthread_cond_broadcast(&rb.not_empty);
        pthread_cond_broadcast(&rb.not_full);
        pthread_mutex_unlock(&rb.mtx);
        pthread_join(shut_thr, NULL);
        rb_destroy(&rb);
        return EXIT_FAILURE;
    }

    // Starta konsumenter
    pthread_t *cons = (pthread_t*)malloc(sizeof(pthread_t) * N);
    thread_arg_t *cargs = (thread_arg_t*)malloc(sizeof(thread_arg_t) * N);
    if (!cons || !cargs) {
        perror("malloc");
        pthread_mutex_lock(&rb.mtx);
        rb.shutdown = 1;
        pthread_cond_broadcast(&rb.not_empty);
        pthread_cond_broadcast(&rb.not_full);
        pthread_mutex_unlock(&rb.mtx);
        pthread_join(prod, NULL);
        pthread_join(shut_thr, NULL);
        rb_destroy(&rb);
        free(cons); free(cargs);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < N; ++i) {
        cargs[i].id = i + 1;
        cargs[i].rb = &rb;
        cargs[i].time_interval = 0;
        if (pthread_create(&cons[i], NULL, consumer_main, &cargs[i]) != 0) {
            perror("pthread_create consumer");
            pthread_mutex_lock(&rb.mtx);
            rb.shutdown = 1;
            pthread_cond_broadcast(&rb.not_empty);
            pthread_cond_broadcast(&rb.not_full);
            pthread_mutex_unlock(&rb.mtx);
            N = i; // antal som faktiskt startade
            break;
        }
    }

    // Vänta in nedstängning
    pthread_join(shut_thr, NULL);
    pthread_join(prod, NULL);
    for (int i = 0; i < N; ++i) pthread_join(cons[i], NULL);

    // Summering
    printf("\n=== Summering ===\n");
    printf("Producerat: %lu\n", rb.produced_total);
    printf("Konsumerat: %lu\n", rb.consumed_total);
    printf("Kvar i buffert: %d\n", rb.count);

    rb_destroy(&rb);
    free(cons);
    free(cargs);
    return EXIT_SUCCESS;
}
