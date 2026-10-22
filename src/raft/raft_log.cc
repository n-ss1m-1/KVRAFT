//raft_log.cc
#include<cassert>

#include<spdlog/spdlog.h>

#include "common/config.h"
#include "raft/raft_log.h"


void RaftLog::AppendEntry(const int64_t term,const Command& command)
{
    entries_.emplace_back(LastIndex()+1,term,command);
}

void RaftLog::AppendEntry(const LogEntry& entry)
{
    assert(entry.index == LastIndex()+1);
    entries_.emplace_back(entry);
}

bool RaftLog::AppendFrom(int64_t startIndex,const std::vector<LogEntry>& entries)
{              
    spdlog::info("[RaftLog] AppendFrom: startIndex={}, entries.size={}, snapshotIndex={}, LastIndex={}",
        startIndex, entries.size(), snapshotIndex_, LastIndex());

    //! TODO: startIndex<FirstIndex()时，表示快照需要被覆盖，此时需要Leader发送快照。  // 后来的补充：此处不需要，因为会先checkConsistency()，那里会告知Leader发送快照，然后就不会调用AppendFrom()了
    // 校验startIndex的范围： 例：Size()=1时 startIndex可以是1或2
    if(startIndex<FirstIndex() || startIndex>LastIndex()+1) return false;           
    
    //!▲ 我的 RaftLog 内部假设：entries_[i].index == snapshotIndex_ + i + 1 永远成立。如果外部传入的entries不满足这个规则，则逻辑全乱。   !一定要在截断前检查，否则日志坏了再退出也没用了
    //1. 保证起点对的上 2. 保证内部是连续的index
    if(!entries.empty())
    {
        for (size_t i = 0; i < entries.size(); i++) 
        {
            if (entries[i].index != startIndex + static_cast<int64_t>(i)) return false;
        }
    }

    //!▲不管entries是否为空 都需要判断是否需要截断
    //先截断
    if(startIndex<=LastIndex()) 
    {
        if(!TruncateFrom(startIndex)) return false;
    }

    //再插入末尾
    entries_.insert(entries_.end(),entries.begin(),entries.end());

    return true;
}

//已自动计算偏移
std::optional<LogEntry> RaftLog::GetEntry(const int64_t index) const
{
    if(index<FirstIndex() || index>LastIndex()) return std::nullopt;

    return entries_[IndexToArrayPos(index)];           //!隐式构造optional 避免拷贝  !注意此处已经自动计算偏移
}

std::vector<LogEntry> RaftLog::GetEntries(const int64_t startIndex,const int64_t endIndex) const
{
    if(startIndex<FirstIndex() || startIndex>=endIndex || endIndex>LastIndex()+1) return {};                       //返回区间[start,end)

    return std::vector<LogEntry>(entries_.begin()+ IndexToArrayPos(startIndex), entries_.begin()+ IndexToArrayPos(endIndex));        //!构造函数区间是左闭右开的
}

int64_t RaftLog::GetTerm(int64_t index) const
{
    //刚好在快照点
    if(index == snapshotIndex_) return snapshotTerm_;

    //在快照内 or 非正数越界
    if(index < FirstIndex()) return 0;

    //在逻辑日志内
    return GetEntry(index)->term;
}

int64_t RaftLog::FirstIndex() const
{
    return snapshotIndex_ + 1;
}

int64_t RaftLog::LastIndex() const
{
    if(entries_.empty()) return snapshotIndex_;

    return entries_.back().index;
}

int64_t RaftLog::LastTerm() const
{
    if(entries_.empty()) return snapshotTerm_;
    
    return entries_.back().term;
}

bool RaftLog::TruncateFrom(int64_t index)
{
    if(index<FirstIndex() || index>LastIndex()) return false;                    //此处调用方应该检查，若是传入错误，则什么都不做
    entries_.erase(entries_.begin()+IndexToArrayPos(index),entries_.end());
    return true;
}

void RaftLog::SetSnapshotIndexAndTerm(int64_t index,int64_t term)
{
    if(index <= snapshotIndex_) return;        //无效位置               //bug!▲  LastIndex()<index 是合理的，快照点可以比日志更新

    //需要删除日志项
    if(index > snapshotIndex_)      
    {
        size_t removeCount = index - snapshotIndex_;
        if(removeCount >= Size())   entries_.clear();         //! 注意：当removeCount>Size()时，begin()+removeCount为无效迭代器，不能使用erase
        else                        entries_.erase(entries_.begin(), entries_.begin()+removeCount);      //! 左闭右开
    }

    snapshotIndex_ = index;
    snapshotTerm_ = term;
}

//!注意实现逻辑
ConsistencyCheckResult RaftLog::CheckConsistency(int64_t prevLogIndex,int64_t prevLogTerm) const
{
    //1. Leader需对比的日志为空，总是通过
    if(prevLogIndex == 0) 
    {
        return {true, 0};
    }

    //2. 我落后于Leader
    if(prevLogIndex > LastIndex())
    {
        return {false, LastIndex()+1};       //从LastIndex()+1开始为空
    }

    //3. 我有prevLogIndex，还需要检查term是否对应：只有(index,term)相同，才能表明是同一条日志

    //3.1 prevLogIndex 恰好等于快照点
    if(prevLogIndex == snapshotIndex_)
    {
        if(prevLogTerm == snapshotTerm_)
        {
            return {true, 0};
        }

        //!▲ term不匹配：理论上不会发生，除非快照创建or状态机有bug，此时只能通过发快照覆盖
        spdlog::error("[RaftLog] inconsistent snapshot: prevLog=({},{}), mySnapshot=({},{})",
            prevLogIndex, prevLogTerm, snapshotIndex_, snapshotTerm_);
        return {false, config::kNeedSnapshot};         //!▲ conflictIndex = -1 告知 leader发送快照
    }

    //3.2 prevLogIndex 严格小于 快照点
    if(prevLogIndex < snapshotIndex_)
    {
        //!▲ 严格小于快照点：无法判断，conflictIndex = -1 告知 leader发送快照来恢复
        return {false, config::kNeedSnapshot};
    }

    //3.3 prevLogIndex 在内存日志里(snapshotIndex_ < prevLogIndex <= LastIndex()): 正常比较
    int64_t myTerm = GetTerm(prevLogIndex);
    if(myTerm != prevLogTerm)
    {
        int64_t idx = prevLogIndex;
        while(idx > FirstIndex())                               //! 生成快照后 内存中第一条日志index不再是1
        {                                   
            if(GetTerm(idx - 1) != myTerm) break;     //! 往前找相同term段的最早位置(避免Leader逐条回退)
            idx--;
        }
        return {false,idx};
    }

    return {true,0};
}

int64_t RaftLog::Size() const
{
    return entries_.size();
}

int64_t RaftLog::SnapshotIndex() const
{
    return snapshotIndex_;
}

int64_t RaftLog::SnapshotTerm() const
{
    return snapshotTerm_;
}

size_t RaftLog::IndexToArrayPos(int64_t index) const
{
    assert(snapshotIndex_ < index);
    assert(index <= snapshotIndex_ + Size());

    return static_cast<size_t>(index - snapshotIndex_ - 1);
}
