#include "thread_pool.h"

// the consumer loop each worker runs until told to stop
// wait with a predicate handles spurious wakeups for us so no manual while loop
static void worker(ThreadPool* tp) {
    while (true) {
        unique_lock<mutex> lock(tp->mu);
        tp->cv.wait(lock, [tp] { return tp->stop || !tp->queue.empty(); });
        if (tp->stop && tp->queue.empty()) {
            return;                     // drained and asked to stop so exit
        }
        // take one task then release the lock before running it
        // running under the lock would serialise every worker
        Work w = tp->queue.front();
        tp->queue.pop_front();
        lock.unlock();

        w.f(w.arg);
    }
}

void thread_pool_init(ThreadPool* tp, size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        tp->threads.emplace_back(worker, tp);
    }
}

// the producer side push a task under the lock then wake one sleeping worker
void thread_pool_queue(ThreadPool* tp, void (*f)(void*), void* arg) {
    {
        lock_guard<mutex> lock(tp->mu);
        tp->queue.push_back(Work{ f, arg });
    }
    tp->cv.notify_one();
}

// signal every worker to finish the queue then wind down and join them
ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> lock(mu);
        stop = true;
    }
    cv.notify_all();
    for (thread& t : threads) {
        if (t.joinable()) t.join();
    }
}
