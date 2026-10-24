// skip_list.h

#pragma once
#include<vector>
#include<array>
#include<memory>
#include<functional>
#include<random>

#include "internal_key.h"
#include "noncopyable.h"


class SkipList : private lsm::NonCopyable
{
private:
    //外部不需要，放入private
    struct Node
    {
        InternalKey key;
        std::string value;
        std::vector<Node*> next;

        Node(const InternalKey& key, const std::string& value, int height);
    };

public:
    static constexpr int kMaxHeight = 12;           // 从上到下：Level减小 节点变多
    using HeightGenerator = std::function<int()>;       //!用于测试：传入固定高度序列生成器

    class Iterator
    {
    public:
        bool Valid() const noexcept;

        // 调用前必须 Valid()
        const InternalKey& key() const;
        const std::string& value() const;
        void Next();                            // 沿next[0]移动一次

    private:
        friend class SkipList;      //! friend + 私有Iterator构造函数：只有 SkipList 可以创建合法的 Iterator。外部代码不能随便拿一个 Node* 构造迭代器
        explicit Iterator(const Node* node);
        const Node* current_;
    };


    SkipList();
    explicit SkipList(HeightGenerator generator);       // !用于测试：传入固定高度序列生成器
    ~SkipList() = default;

    SkipList(const SkipList&) = delete;                 // 拷贝构造
    SkipList& operator=(const SkipList&) = delete;      // 拷贝赋值
    SkipList(SkipList&&) = delete;                      // !移动构造
    SkipList& operator=(SkipList&&) = delete;           // !移动赋值

    //插入: 重复节点 -> false  其余->抛出异常
    bool Insert(const InternalKey& key, const std::string& value);

    //!查找：返回第一条不小于 target 的记录，1.目标节点  2.同一user_key的更旧版本  3.下一个user_key  4.nullptr
    Iterator Seek(const InternalKey& key) const;

    // Level 0 的第一条真实记录
    Iterator Begin() const;

    size_t Size() const noexcept;
    bool Empty() const noexcept;

private:
    // !查找候选位置 + 记录前驱(Insert和Seek复用)
    // predecessors: Insert每层插入位置的前驱节点
    Node* FindGreaterOrEqual(const InternalKey& target, std::array<Node*, kMaxHeight>* predecessors=nullptr) const;

    int RandomHeight();

    InternalKeyComparator comparator_;

    std::vector<std::unique_ptr<Node>> nodes_;
    Node* head_ = nullptr;

    std::mt19937 random_;
    HeightGenerator heightGenerator_;
};










