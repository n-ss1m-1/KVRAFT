//thread_pool.cc

#include<algorithm>

#include<spdlog/spdlog.h>

#include "common/thread_pool.h"


ThreadPool::ThreadPool(size_t threadCount)
{
    //如果没有输入参数；则默认 线程数=CPU核心数；如果读取失败，则设置为1个
    if(threadCount == 0) 
    {
        threadCount = std::max(1u, std::thread::hardware_concurrency());
    }

    for(size_t i=0;i<threadCount;i++)
    {
        workers_.emplace_back( [this](){WorkerLoop();} );
    }
}

ThreadPool::~ThreadPool()
{
    Stop();
}

void ThreadPool::Submit(Task task)
{
    {    
        std::lock_guard<std::mutex> lock(mutex_);

        if(stopping_) return;

        tasks_.push(std::move(task));               //! 值成员+move移动构造---减少开销
    }
    //! 锁外notify唤醒 
    //  如果在锁内唤醒：被唤醒的线程会尝试重新加锁，但 Submit 还持有锁 → 它要等 Submit 释放锁后才能继续 → 多一次上下文切换
    cond_.notify_one();
}

void ThreadPool::Stop()
{
    {    
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }

    //唤醒所有线程 -> stopping -> 退出循环
    cond_.notify_all();

    for(auto& t : workers_)
    {
        if(t.joinable()) t.join();          //! join：阻塞等待当前线程执行完任务，然后回收线程资源 (joinable用于检查是否能join)
    }

    //workers_.clear();  会自动清空
}

void ThreadPool::WorkerLoop()
{
    while(true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);          //! unique_lock才能和条件变量配合

            //退出 or 新任务到来 -> 重新持锁
            cond_.wait(lock,[this](){return stopping_ || !tasks_.empty();});

            if(stopping_ && tasks_.empty()) return;             //! 停止状态 + 任务全部执行完 -> 才退出；不能直接把任务丢了退出

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        //! 锁外执行任务
        //! 使用try/catch捕获异常：如果没有 - task内throw() - 异常穿透WorkerLoop - 线程异常终止 - 线程不会自动重启导致越来越少 - 最终线程池无工作线程执行任务
        try
        {
            task();
        }
        catch(const std::exception& e)
        {
            spdlog::error("[ThreadPool] Task threw: {}", e.what());
        }
        catch(...)
        {
            spdlog::error("[ThreadPool] Task threw unknown exception");
        }
    }
}

