// data_block.h
#pragma once
#include <string>
#include <string_view>

#include "internal_key.h"
#include "coding.h"
#include "lookup_result.h"
#include "noncopyable.h"


/*
 顺序：必须按照InternalKey递增追加
 容量：默认4kb，(初始化)允许空块(不意味着将空块写入SSTable)，允许大于4kb的单记录块
 磁盘布局：
 - [记录1][记录2][记录3]...
 - 记录(Entry)：[sequence：8 B][type：1 B][key_len：4 B][value_len：4 B][key][value]
*/
class DataBlockBuilder : private lsm::NonCopyable
{
public:
    DataBlockBuilder() = default;

    // 严格按 InternalKey 顺序递增追加
    // 非法记录、重复键或逆序：抛 invalid_argument
    void Add(const lsm::Entry& entry);

    // !返回借用视图，Add/Reset 后可能失效
    std::string_view Data() const noexcept;

    const InternalKey& FirstKey() const noexcept;       // 仅当非空块时调用
    size_t ByteSize() const noexcept;
    bool Empty() const noexcept;
    void Reset();                       // ! 情况当前内容和排序检查状态 -> 让Builder可以构建下一个块

private:
    std::string data_;
    InternalKey firstKey_;          // 当前正在构造的块的第一个InternalKey
    InternalKey lastKey_;           // ! 与准备追加的记录做比较
    bool hasFirstKey_ = false;
    bool hasLastKey_ = false;
};




class DataBlockReader : private lsm::NonCopyable
{
public:
    class Iterator
    {
    public:
        /* ! valid_状态统一由 LoadCurrent() 管理
         1. 成功解析当前位置的一条记录：true
         2. 当前位置大于等于块长度，即到达末尾：false
         3. 格式错误：先设 false，再抛异常
        */
        bool Valid() const noexcept;

        // 仅 Valid() 为 true 时可以调用：
        const InternalKey& Key() const;
        const std::string& Value() const;
        void Next();                        // ! 获取下一条记录 + 更新offset  !自动调用LoadCurrent()
    private:
        friend class DataBlockReader;       // ! friend + 私有Iterator构造函数：只有 DataBlockReader 可以创建合法的 Iterator。外部代码不能随便拿一个 Node* 构造迭代器

        explicit Iterator(const DataBlockReader* owner);

        // 从 offset_ 解析当前记录，并计算下一条位置  
        void LoadCurrent();

        const DataBlockReader* owner_ = nullptr;    // 借用所属Reader，获取块字节
        size_t offset_ = 0;                         // 当前记录的开始位置
        size_t nextOffset_ = 0;                     // 下一条记录的开始位置
        lsm::Entry current_;                        // 当前的记录
        bool valid_ = false;                        // 当前位置是否有记录 (到达块末尾 -> false)
    };

    // 接管块字节，验证全部记录及排列顺序
    // 空块合法；损坏数据抛 runtime_error
    explicit DataBlockReader(std::string data);

    Iterator Begin() const;

    // 找第一个不小于 target 的 InternalKey，再检查 user_key 是否相同
    LookupResult Get(uint64_t readSequence, std::string_view userKey) const;

private:
    // 从offset开始解析一条entry
    // 成功：输出entry+下一条记录的offset  失败：格式无法解析 返回false
    bool ReadEntryAt(size_t offset, lsm::Entry& entry, size_t& nextOffset) const;

    std::string data_;
};
