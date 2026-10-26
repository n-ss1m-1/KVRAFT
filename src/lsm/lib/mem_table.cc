// mem_table.cc
#include<stdexcept>

#include "mem_table.h"

bool MemTable::Add(uint64_t sequence, ValueType type, const std::string& user_key, const std::string& value)
{
    if(IsFrozen()) throw std::logic_error("MemTable is frozen, cannot add new entry");          //! 需要检查是否已冻结：冻结后只能读 不能写
    if(sequence == 0) throw std::invalid_argument("InternalKey sequence cannot be zero");       //! 即使SkipList::Insert()会检查，此处也需要检查，防止以后更换底层容器时的错误
    if(type!=ValueType::kValue && type!=ValueType::kDeletion) throw std::invalid_argument("invalid ValueType, only kValue and kDeletion supported");        //! 需要检查type是否合法
    if(type==ValueType::kDeletion && !value.empty()) throw std::invalid_argument("deletion entry must have empty value");       //!删除记录的value必须为空

    InternalKey key;
    key.sequence=sequence;
    key.type=type;
    key.user_key=user_key;

    return skipList_.Insert(key,value);
}

LookupResult MemTable::Get(uint64_t sequence, const std::string& user_key) const
{
    // 构造targetKey
    InternalKey target;
    target.sequence=sequence;
    target.user_key=user_key;
    Iterator it = skipList_.Seek(target);

    LookupResult result;

    // 检查是否为空
    if(!it.Valid()) return result;      // State默认初始化为kNotFound

    // 接收Result
    InternalKey resultKey = it.Key();

    // kNotFound    !无需也不能比较sequence
    if(resultKey.user_key != target.user_key) return LookupResult();

    // kDeleted or kValue
    if(resultKey.type == ValueType::kDeletion) result.state = LookupState::kDeleted;
    if(resultKey.type == ValueType::kValue)
    {
        result.state = LookupState::kValue;
        result.value = it.Value();
    }

    return result;
}

MemTable::Iterator MemTable::Begin() const      // 类外(返回值)：必须指明MemTable作用域
{
    return skipList_.Begin();
}

size_t MemTable::Size() const noexcept
{
    return skipList_.Size();
}

bool MemTable::Empty() const noexcept
{
    return skipList_.Empty();
}

size_t MemTable::ApproximateMemoryUsage() const noexcept
{
    return skipList_.ApproximateMemoryUsage();
}

void MemTable::Freeze() noexcept
{
    frozen_ = true;
}

bool MemTable::IsFrozen() const noexcept
{
    return frozen_;
}
