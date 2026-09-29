//thread_pool.h

#pragma once
#include<functional>
#include<vector>
#include<queue>
#include<thread>
#include<mutex>
#include<condition_variable>

class ThreadPool
{
public:
    using Task = std::function<void()>;

    ThreadPool(size_t threadCount = 0);
    ~ThreadPool();           
    
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    //外部提交任务 唤醒线程执行(未停止才能提交)
    void Submit(Task task);

    //优雅停止：更改状态 执行完所有任务后释放线程(幂等，多次调用无副作用)
    void Stop();
private:
    //线程 循环等待/执行任务
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;

    bool stopping_ = false;
};


