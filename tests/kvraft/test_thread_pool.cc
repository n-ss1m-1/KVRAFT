//test_thread_pool.cc
// 覆盖 common/thread_pool：任务执行、并发计数、任务内提交、异常隔离、优雅停止、Stop 后提交

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#include "common/thread_pool.h"
#include "test_util.h"

// ==================== 基础执行 ====================

void TestBasicExecute() {
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) {
        pool.Submit([&] { counter.fetch_add(1); });
    }
    pool.Stop();
    CHECK_EQ(counter.load(), 100);
}

void TestReturnValueIgnored() {
    // Submit 只接收 void() 任务，返回值任务可包在 future 里
    ThreadPool pool(1);
    std::promise<int> p;
    std::future<int> f = p.get_future();
    pool.Submit([&p] { p.set_value(42); });
    CHECK_EQ(f.get(), 42);   // 析构会 Stop 并 join
}

// ==================== 并发正确性 ====================

void TestConcurrentManyTasks() {
    constexpr int kThreads = 4;
    constexpr int kTasks = 2000;
    ThreadPool pool(kThreads);
    std::atomic<int> counter{0};
    for (int i = 0; i < kTasks; ++i) {
        pool.Submit([&] {
            // 制造一点竞争
            int x = counter.load();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            counter.store(x + 1);
        });
    }
    pool.Stop();
    // 注意：上面的 load/sleep/store 不是原子 RMW，这里不要求精确等于 kTasks；
    // 真正要验证的是 2000 个任务全部被执行（没有丢任务）。
    // 所以改用 fetch_add 版本的断言见 TestConcurrentExactCount。
    CHECK(counter.load() > 0);
}

void TestConcurrentExactCount() {
    constexpr int kTasks = 5000;
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    for (int i = 0; i < kTasks; ++i) {
        pool.Submit([&] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.Stop();
    CHECK_EQ(counter.load(), kTasks);
}

// ==================== 任务内再提交（防死锁） ====================

void TestSubmitFromWorker() {
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    std::promise<void> done;
    std::future<void> fdone = done.get_future();

    pool.Submit([&] {
        counter.fetch_add(1);
        // 工作线程内再提交，不能死锁
        pool.Submit([&] {
            counter.fetch_add(1);
            done.set_value();
        });
    });

    CHECK_EQ(fdone.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    pool.Stop();
    CHECK_EQ(counter.load(), 2);
}

// ==================== 异常隔离 ====================

void TestTaskExceptionDoesNotKillWorker() {
    ThreadPool pool(1);
    std::promise<void> afterException;
    std::future<void> f = afterException.get_future();

    pool.Submit([] { throw std::runtime_error("boom"); });
    pool.Submit([&afterException] { afterException.set_value(); });

    // 异常后线程仍存活，后续任务能执行
    CHECK_EQ(f.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    pool.Stop();
}

void TestUnknownExceptionCaught() {
    ThreadPool pool(1);
    std::promise<void> afterThrow;
    std::future<void> f = afterThrow.get_future();
    pool.Submit([] { throw 42; });                 // 非 std::exception
    pool.Submit([&afterThrow] { afterThrow.set_value(); });
    CHECK_EQ(f.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    pool.Stop();
}

// ==================== 停止语义 ====================

void TestStopDrainsQueuedTasks() {
    ThreadPool pool(1);   // 单线程，后几个任务必然排队
    std::atomic<int> counter{0};
    std::promise<void> firstStarted;
    std::future<void> fs = firstStarted.get_future();
    std::promise<void> releaseFirst;
    std::future<void> fr = releaseFirst.get_future();

    pool.Submit([&] {
        firstStarted.set_value();
        fr.wait();          // 卡住第一个任务
        counter.fetch_add(1);
    });
    for (int i = 0; i < 10; ++i) {
        pool.Submit([&] { counter.fetch_add(1); });
    }

    fs.wait();
    // 此时 10 个任务在队列里。另起线程 Stop，必须等全部排空
    std::thread stopper([&] { pool.Stop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(counter.load(), 0);   // 第一个任务还没放行
    releaseFirst.set_value();
    stopper.join();
    CHECK_EQ(counter.load(), 11);  // 全部排空
}

void TestSubmitAfterStopIgnored() {
    ThreadPool pool(1);
    pool.Stop();
    std::atomic<int> counter{0};
    pool.Submit([&] { counter.fetch_add(1); });  // 静默忽略，不崩溃
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(counter.load(), 0);
}

void TestStopIdempotent() {
    ThreadPool pool(2);
    pool.Stop();
    pool.Stop();   // 重复调用不崩溃
    CHECK(true);
}

// ==================== 线程数配置 ====================

void TestDefaultThreadCount() {
    ThreadPool pool(0);   // 0 = 硬件并发数，至少 1
    std::atomic<int> counter{0};
    std::promise<void> done;
    std::future<void> f = done.get_future();
    pool.Submit([&] {
        counter.fetch_add(1);
        done.set_value();
    });
    CHECK_EQ(f.wait_for(std::chrono::seconds(5)),
             std::future_status::ready);
    pool.Stop();
    CHECK_EQ(counter.load(), 1);
}

int main() {
    std::cout << "===== ThreadPool Tests =====\n";

    RUN_TEST(TestBasicExecute);
    RUN_TEST(TestReturnValueIgnored);
    RUN_TEST(TestConcurrentManyTasks);
    RUN_TEST(TestConcurrentExactCount);
    RUN_TEST(TestSubmitFromWorker);
    RUN_TEST(TestTaskExceptionDoesNotKillWorker);
    RUN_TEST(TestUnknownExceptionCaught);
    RUN_TEST(TestStopDrainsQueuedTasks);
    RUN_TEST(TestSubmitAfterStopIgnored);
    RUN_TEST(TestStopIdempotent);
    RUN_TEST(TestDefaultThreadCount);

    TEST_SUMMARY_RETURN();
}
