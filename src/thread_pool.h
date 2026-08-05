#pragma once
#include <stddef.h>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>

using namespace std;

// one unit of deferred work a function pointer plus its argument
struct Work {
    void (*f)(void*) = nullptr;
    void* arg = nullptr;
};

// a fixed set of worker threads draining a shared queue
// producers push work under mu and notify cv
// idle workers sleep on cv until there is something to do or stop is set
// the destructor drains any queued work then joins every worker
struct ThreadPool {
    vector<thread> threads;
    deque<Work> queue;
    mutex mu;
    condition_variable cv;
    bool stop = false;

    ~ThreadPool();
};

// spawn num_threads workers that live for the lifetime of the pool
void thread_pool_init(ThreadPool* tp, size_t num_threads);

// hand one task to the pool it runs on some worker later fire and forget
void thread_pool_queue(ThreadPool* tp, void (*f)(void*), void* arg);
