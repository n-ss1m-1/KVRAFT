// noncopyable.h
#pragma once

namespace lsm
{

class NonCopyable
{
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

public:
    NonCopyable(const NonCopyable&) = delete;               // 拷贝构造
    NonCopyable& operator=(const NonCopyable&) = delete;    // 拷贝赋值
    NonCopyable(NonCopyable&&) = delete;                    // !移动构造
    NonCopyable& operator=(NonCopyable&&) = delete;         // !移动赋值
};

} // namespace lsm