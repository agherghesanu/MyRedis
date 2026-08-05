#include <assert.h>
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "thread_pool.h"

using namespace std;

// every queued task bumps this so we can prove all of them ran
static atomic<long> g_counter{ 0 };

// sums the argument into the counter so we also prove the arg is passed intact
static void add_task(void* arg) {
    long v = (long)(intptr_t)arg;
    g_counter.fetch_add(v, memory_order_relaxed);
}

int main() {
    printf("thread pool tests:\n");

    ThreadPool tp;
    thread_pool_init(&tp, 4);

    const long N = 100000;
    long expected = 0;
    for (long i = 1; i <= N; i++) {
        expected += i;
        thread_pool_queue(&tp, &add_task, (void*)(intptr_t)i);
    }

    // fire and forget so wait for the workers to drain with a timeout guard
    auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
    while (g_counter.load() != expected) {
        if (chrono::steady_clock::now() > deadline) {
            printf("  FAIL: timed out got %ld want %ld\n", g_counter.load(), expected);
            return 1;
        }
        this_thread::sleep_for(chrono::milliseconds(2));
    }

    printf("  ok: all %ld tasks ran once sum %ld\n", N, expected);
    printf("all thread pool tests passed\n");
    return 0;
}
