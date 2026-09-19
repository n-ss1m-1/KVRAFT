//raft_log.cc

#include "raft_log.h"


void RaftLog::AppendEntry(const int64_t term,const Command& command)
{
    entries_.emplace_back(LastIndex()+1,term,command);
}

void RaftLog::AppendEntry(const LogEntry& entry)
{
    entries_.emplace_back(entry);
}

bool RaftLog::AppendFrom(const int64_t startIndex,const std::vector<LogEntry>& entries)
{
    if(entries.empty()) return true;                 
    if(startIndex<=0 || startIndex>Size()+1) return false;           //超过范围了 例：Size()=1时 startIndex可以是1或2
    
    //!▲ 我的 RaftLog 内部假设：entries_[i].index == i + 1 永远成立。如果外部传入的entries不满足这个规则，则逻辑全乱。   !一定要在截断前检查，否则日志坏了再退出也没用了
    //1. 保证起点对的上 2. 保证内部是连续的index
    for (size_t i = 0; i < entries.size(); i++) 
    {
        if (entries[i].index != startIndex + static_cast<int64_t>(i)) return false;
    }

    //先截断
    if(startIndex<=Size()) 
    {
        TruncateFrom(startIndex);
    }

    //再插入末尾
    entries_.insert(entries_.end(),entries.begin(),entries.end());

    return true;
}

//已自动计算偏移
std::optional<LogEntry> RaftLog::GetEntry(const int64_t index) const
{
    if(index<=0 || index>Size()) return std::nullopt;

    return entries_[index-1];           //!隐式构造optional 避免拷贝  !注意此处已经自动计算偏移
}

std::vector<LogEntry> RaftLog::GetEntries(const int64_t startIndex,const int64_t endIndex) const
{
    if(startIndex<=0 || startIndex>=endIndex || endIndex>Size()+1) return {};                       //返回区间[start,end)

    return std::vector<LogEntry>(entries_.begin()+startIndex-1,entries_.begin()+endIndex-1);        //!构造函数区间是左闭右开的
}

int64_t RaftLog::LastIndex() const
{
    if(Size()==0) return 0;

    return entries_.back().index;
}

int64_t RaftLog::LastTerm() const
{
    if(Size()==0) return 0;
    
    return entries_.back().term;
}



void RaftLog::TruncateFrom(int64_t index)
{
    if(index<=0 || index>Size()) return;                    //此处调用方应该检查，若是传入错误，则什么都不做
    entries_.erase(entries_.begin()+index-1,entries_.end());
}

//!注意实现逻辑
ConsistencyCheckResult RaftLog::CheckConsistency(int64_t prevLogIndex,int64_t prevLogTerm) const
{
    //1. Leader日志为空，总是通过
    if(prevLogIndex == 0) 
    {
        return {true,0};
    }

    //2. 我落后于Leader
    if(prevLogIndex > LastIndex())
    {
        return {false,LastIndex()+1};       //从LastIndex()+1开始为空
    }

    //3. 我有prevLogIndex，还需要检查term是否对应：只有(index,term)相同，才能表明是同一条日志
    int64_t myTerm = GetEntry(prevLogIndex).value().term;         //! 注意[0]对应index=1
    if(myTerm != prevLogTerm)
    {
        int64_t idx = prevLogIndex;
        while(idx>1 && GetEntry(idx-1).value().term == myTerm)  //! 往前找相同term段的最早位置(避免Leader逐条回退)
        {
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
