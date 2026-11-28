// compaction.cc
#include <queue>
#include <optional>
#include <stdexcept>

#include "compaction.h"
#include "sstable.h"
#include "internal_key.h"

void MergeTables(const std::vector<const SSTableReader*>& tables, const std::function<void(const lsm::Entry&)>& emit) 
{
    // 1. 创建SSTable迭代器列表
    std::vector<SSTableReader::Iterator> inputs;
    inputs.reserve(tables.size());
    for(const auto* t : tables)
    {
        if(t == nullptr) throw std::invalid_argument("MergeTables: null sstable reader");
        inputs.push_back(t->Begin());
    }

    // 2. 创建最小堆比较器
    auto compare = [&inputs](size_t lhs, size_t rhs){
        InternalKeyComparator comparator;
        return comparator.Compare(inputs[lhs].Key(), inputs[rhs].Key()) > 0;
    };

    // 3. 创建最小堆
    std::priority_queue<size_t, std::vector<size_t>, decltype(compare)> heap(compare);
    for(size_t i=0; i<inputs.size(); i++) 
    {
        if(inputs[i].Valid()) heap.push(i);     // 空SSTable的Begin()正常返回无效迭代器，此处需检查 -> !保证heap内均为有效迭代器下标
    }

    // 3. 循环：弹出堆顶(最小key) -> 判断是否输出 -> 推进迭代器 -> 重新入堆
    std::optional<InternalKey> previousKey;
    InternalKeyComparator comparator;
    while(!heap.empty())
    {
        size_t idx = heap.top();        // 返回const&: 如果heap内直接存迭代器，则需要再次复制迭代器，更麻烦(!额外使用inputs的原因)
        heap.pop();
        
        auto& it = inputs[idx];
        const auto& key = it.Key();
        if(previousKey.has_value() && 0 == comparator.Compare(previousKey.value(), key))    // 检查重复key
            throw std::runtime_error("MergeTables: duplicate internal key");
        if(!previousKey || previousKey->user_key != key.user_key)                   // 输出合法key
            emit(lsm::Entry{key,it.Value()});
        previousKey = key;                                // 每次都更新: 过程中遇到的重复key都能发现

        it.Next();

        if(it.Valid()) heap.push(idx);      //▲ ! 内部元素变更后 需要重新插入 才能正确排序    // 重新插入前检查是否有效 -> !保证heap内均为有效迭代器下标
    }
}

