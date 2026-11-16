// data_block.cc
#include <stdexcept>
#include <cassert>

#include "data_block.h"
#include "diagnostics.h"

void DataBlockBuilder::Add(const lsm::Entry& entry) 
{
    // 1. 校验key顺序(lastKey_>=entry.key均不行)
    InternalKeyComparator cmp;
    if(hasLastKey_ && !cmp(lastKey_,entry.key)) throw std::invalid_argument("Add: DataBlock entry key out of order, key must be strictly increasing");

    // 2. Entry编码
    std::string encoded = lsm::EncodeEntry(entry);     

    // ! 此处不需要校验DataBlock大小以及分块   ->    由SSTableBuilder负责

    // 3. 追加记录
    data_.append(encoded);

    // 4. 更新排序状态              // TODO:!? Append前保留副本: 避免下方直接firstKey_ / lastKey_ = entry.key时因分配内存抛异常(保证状态一致性)
    if(!hasFirstKey_) 
    {
        firstKey_ = entry.key;
        hasFirstKey_ = true;
    }
    lastKey_ = entry.key;
    hasLastKey_ = true;
}

std::string_view DataBlockBuilder::Data() const noexcept 
{
    return data_;
}

const InternalKey& DataBlockBuilder::FirstKey() const noexcept
{
    assert(!Empty());       
    return firstKey_;
}

size_t DataBlockBuilder::ByteSize() const noexcept 
{
    return data_.size();
}

bool DataBlockBuilder::Empty() const noexcept 
{
    return data_.empty();
}

void DataBlockBuilder::Reset() 
{
    data_.clear();
    firstKey_ = InternalKey();
    lastKey_ = InternalKey();
    hasFirstKey_ = false;
    hasLastKey_ = false;
}

bool DataBlockReader::Iterator::Valid() const noexcept 
{
    return valid_;
}

const InternalKey& DataBlockReader::Iterator::Key() const 
{
    assert(Valid());                        // assert() 保证有效
    return current_.key;
}

const std::string& DataBlockReader::Iterator::Value() const 
{
    assert(Valid());
    return current_.value;
}

void DataBlockReader::Iterator::Next() 
{
    assert(Valid());
    offset_ = nextOffset_;
    LoadCurrent();
}

DataBlockReader::Iterator::Iterator(const DataBlockReader* owner)
    : owner_(owner)
{
    LoadCurrent();
}

void DataBlockReader::Iterator::LoadCurrent() 
{
    // 1. 重置状态
    nextOffset_ = offset_;
    current_ = lsm::Entry();
    valid_ = false;

    // 2.1 到达块尾
    if(offset_ >= owner_->data_.size()) return;

    // 3. 未到达块尾：读取一条记录
    lsm::Entry entry;
    size_t nextOffset;
    if(!owner_->ReadEntryAt(offset_, entry, nextOffset))
    {
        throw std::runtime_error("LoadCurrent: corrupted data block during iteration");      // 无法解析记录 -> 读取记录失败
    }

    current_ = std::move(entry);
    nextOffset_ = nextOffset;       // offset_在Next()中移动
    valid_ = true;
}

DataBlockReader::DataBlockReader(std::string data)
    : data_(std::move(data))                        // move 避免拷贝
{
    bool hasPrevious = false;
    lsm::Entry previous;
    size_t offset = 0;

    uint64_t diagnosticRecords = 0;
    while(offset < data_.size())   //! data已经move 此后必须使用data_
    {
        ++diagnosticRecords;
        lsm::Entry current;
        size_t nextOffset = offset;

        // 检查是否都能正常解析
        if(!ReadEntryAt(offset, current, nextOffset)) throw std::runtime_error("DataBlockReader: corrupted data block");

        // 检查排列顺序是否合法
        if(hasPrevious)
        {
            InternalKeyComparator cmp;
            if(!cmp(previous.key,current.key)) throw std::runtime_error("DataBlockReader: data block entries are not increasing");
        }

        hasPrevious = true;
        previous = std::move(current);
        offset = nextOffset;
    }
    lsm_diag::count(lsm_diag::ValidatedRecords, diagnosticRecords);
}

DataBlockReader::Iterator DataBlockReader::Begin() const 
{
    return Iterator(this);
}

LookupResult DataBlockReader::Get(uint64_t readSequence, std::string_view userKey) const 
{
    InternalKey target;
    target.sequence = readSequence;
    target.user_key = userKey;

    InternalKeyComparator cmp;

    uint64_t diagnosticScanned = 0;
    Iterator it = this->Begin();          // 构造时自动LoadCurrent()
    while(it.Valid())
    {
        ++diagnosticScanned;
        if(!cmp(it.Key(),target)) break;        // it.Key() >= target
        it.Next();                                       // 无需手动调用LoadCurrent() 其是私有成员 由Next()自动调用
    }

    lsm_diag::count(lsm_diag::ScannedRecords, diagnosticScanned);
    LookupResult result;
    if(!it.Valid()) result.state = LookupState::kNotFound;
    else
    {
        if(it.Key().user_key != userKey) result.state = LookupState::kNotFound;
        else 
        {
            if(it.Key().type == ValueType::kDeletion) result.state = LookupState::kDeleted;
            else
            {
                result.state = LookupState::kValue;
                result.value = it.Value();
            }
        }
    }

    return result;
}

bool DataBlockReader::ReadEntryAt(size_t offset, lsm::Entry& entry, size_t& nextOffset) const 
{
    // 1. 检查offset
    if(offset >= data_.size()) return false;

    // 2. 检查记录边界：先读取key_len，再定位user_key之后的value_len
    const size_t remaining = data_.size() - offset;
    if(remaining < lsm::kWalMinEntrySize) return false;

    const char* start = data_.data() + offset;
    const uint32_t keyLen = lsm::DecodeFixed32(start + 9);
    
    if(keyLen > remaining - lsm::kWalMinEntrySize) return false;
    const size_t valueLengthOffset = lsm::kMinInternalKeySize + static_cast<size_t>(keyLen);
    const uint32_t valueLen = lsm::DecodeFixed32(start + valueLengthOffset);
    if(valueLen > remaining - valueLengthOffset - 4) return false;
    const size_t entrySize = valueLengthOffset + 4 + static_cast<size_t>(valueLen);

    // 3. 解析Entry
    std::string_view encoded = std::string_view(data_).substr(offset, entrySize);                           // ! 此处先转为string_view 再substr: 减少一次拷贝(相较先substr 再赋值给string_view)
    lsm::Entry decoded;
    if(!lsm::DecodeEntry(encoded, decoded)) return false;

    // 4. 提交输出
    entry = std::move(decoded);
    nextOffset = offset + entrySize;
    return true;
}

