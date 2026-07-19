#pragma once
// ----------------------------------------------------------------------------
// Минимальный пул потоков.
//
// Потоки создаются один раз и живут всё время работы программы (создание
// потока на каждую задачу было бы заметным накладным расходом: в горячем
// цикле parallelFor вызывается на каждый 8-МБ блок рёбер).
//
// Модель простая: run(job) синхронно исполняет job(threadIndex) на всех
// потоках пула и ждёт завершения. Поверх неё построен parallelFor —
// динамическая раздача диапазонов через атомарный счётчик (work stealing
// по индексам), что автоматически балансирует нагрузку даже при сильно
// неравномерных данных (гипер-узлы).
// ----------------------------------------------------------------------------

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common.hpp"

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            workers_.emplace_back([this, t] { workerLoop(t); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            ++generation_;
        }
        cvStart_.notify_all();
        for (auto& w : workers_) w.join();
    }

    unsigned size() const { return unsigned(workers_.size()); }

    // Синхронно выполняет job(tid) на всех потоках пула.
    void run(const std::function<void(unsigned)>& job) {
        std::unique_lock<std::mutex> lk(m_);
        job_ = &job;
        pending_ = unsigned(workers_.size());
        ++generation_;
        cvStart_.notify_all();
        cvDone_.wait(lk, [this] { return pending_ == 0; });
        job_ = nullptr;
    }

private:
    void workerLoop(unsigned tid) {
        u64 seenGen = 0;
        for (;;) {
            const std::function<void(unsigned)>* job = nullptr;
            {
                std::unique_lock<std::mutex> lk(m_);
                cvStart_.wait(lk, [&] { return stop_ || generation_ != seenGen; });
                if (stop_) return;
                seenGen = generation_;
                job = job_;
            }
            (*job)(tid);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (--pending_ == 0) cvDone_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_;
    std::condition_variable cvStart_, cvDone_;
    const std::function<void(unsigned)>* job_ = nullptr;
    u64 generation_ = 0;
    unsigned pending_ = 0;
    bool stop_ = false;
};

// Параллельный цикл по диапазону [0, n): потоки атомарно разбирают куски
// размера grain. Порядок обработки кусков не гарантирован (см. README,
// раздел про детерминизм: при --threads 1 порядок строго последовательный).
inline void parallelFor(ThreadPool& pool, u64 n, u64 grain,
                        const std::function<void(u64, u64)>& body) {
    if (n == 0) return;
    if (grain == 0) grain = 1;
    std::atomic<u64> next{0};
    pool.run([&](unsigned) {
        for (;;) {
            u64 b = next.fetch_add(grain, std::memory_order_relaxed);
            if (b >= n) break;
            u64 e = b + grain < n ? b + grain : n;
            body(b, e);
        }
    });
}
