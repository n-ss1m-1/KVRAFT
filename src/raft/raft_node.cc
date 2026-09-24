//raft_node.cc
#include<random>
#include<stdexcept>
#include<algorithm>
#include<iostream>

#include "kv/kv_store.h"
#include "raft/raft_storage.h"
#include "raft/raft_transport.h"
#include "raft/raft_node.h"



//static函数，仅当前文件可见
static int randomTimeoutMs(int low,int high)
{
    //static：这个变量只在第一次调用函数时创建一次，后续调用函数不会重新创建
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(low,high);
    return dist(gen);
}

RaftNode::RaftNode(int32_t nodeId,
                   int32_t totalNodes,
                   std::shared_ptr<RaftTransport> transport,
                   std::shared_ptr<RaftStorage> storage,
                   std::shared_ptr<KVStore> stateMachine):
                nodeId_(nodeId),
                totalNodes_(totalNodes),
                log_(RaftLog()),
                electionResetTime_(std::chrono::steady_clock::now()),
                electionTimeoutMs_(randomTimeoutMs(150, 300)),
                transport_(std::move(transport)),                   // 使用move传递参数，更快速
                storage_(std::move(storage)),
                stateMachine_(std::move(stateMachine))
{
    //仅构造函数中调用recover，保证只调用一次
    {
        //!▲ 构造函数执行期间，对象还没对外发布，其他线程拿不到它的指针，不会有竞争，不需要加锁
        //std::lock_guard<std::mutex> lock(mutex_);               //Recover()必须全区间加锁：如果先读、再加锁、最后写入内存 -> 读和加锁过程中，磁盘的数据可能被重写覆盖，导致使用的是旧数据
        if(!Recover()) 
        {
            //抛出异常而非静默返回 因为恢复失败的RaftNode无法正常使用
            throw std::runtime_error("RaftNode: recover failed");           //#include<stdexcept>
        }
    }
}

//Leader专用
bool RaftNode::Start(const Command& command)
{
    LogEntry entry;
    {    
        std::lock_guard<std::mutex> lock(mutex_);

        //执行命令前 额外确认身份：Leader
        if(!(role_==Role::Leader)) return false;

        //构造日志
        entry.index = log_.LastIndex()+1;   //! 此时log_的下一条日志即为该日志 -> 使用log_.LastIndex()+1
        entry.term = currentTerm_;
        entry.command = command;

        //将日志保存到内存
        log_.AppendEntry(entry);

        //将日志保存到磁盘
        storage_->AppendLogEntry(entry);

        //!自己的matchIndex也更新
        matchIndex_[nodeId_] = entry.index;
    }

    //!锁外主动推送 而非等待Tick
    //! 考虑重复发送的问题：主动SendHeartbeat()之前突然超时 然后tick持锁 发现需要SendHeartbeat() 然后被动SendHeartbeat() 完成之后又回来主动send的问题 这样重复的send会导致什么问题？
    //  重复发送不会改变结果，仅浪费带宽 + RaftLog::AppendFrom()的冗余截断和插入
    SendHeartbeat();                //向所有Follower发送SendAppendEntries_RPC发送消息 -> 需要解锁

    //! unique_lock配合条件变量 (初始化自动加锁，后续可以控制加锁和解锁)
    //条件变量 等待大多数确认：先加锁__不满足条件__解锁__等待__被唤醒__再次加锁检查条件
    //! 谁需要使用notify_all()：Leader && 推进lastApplied_的地方 -> HandleAppendEntriesReply()
    //UpdateCommitIndex()在更新commitIndex_时 -> 调用ApplyCommittedEntries() -> 自动应用到状态机
    std::unique_lock<std::mutex> lock(mutex_);
    if(!(role_==Role::Leader)) return false;                      //! 第二次检查身份：考虑日志追加到自身 但是突然不是Leader的情况->return false：如果新Leader在同步时收到了该日志 则会继续完成同步并提交 / 如果没有 则该日志会被覆盖 -> 均需要客户端重试(幂等指令：直接重试；非幂等指令 / 客户端生成唯一 ID，服务端去重)  //这样的不确定性是分布式系统的特性(一致性和可用性中选择了前者)  //!可优化的点：返回值更明确地说明原因，方便客户端灵活处理错误
    bool apply_result = cond_.wait_for(lock,std::chrono::milliseconds(500),
                                        [this, idx = entry.index](){
                                            return lastApplied_ >= idx;         // 保证写后读，如果使用commitIndex_判断，中间的应用还需要一点时间，可能出现读不到的情况
                                        });

    //回复 提交+应用 成功
    return apply_result;
}

//同步阻塞调用transport_->SendRequestVote，等待消息回复
//▲! 注意设置超时时间，一直等待 那节点就一直阻塞 无法处理其他事情
void RaftNode::SendRequestVote(int32_t peerId,const RequestVoteArgs& args)
{
    //此处(仅RPC发送消息 Sendxxx)不能持有锁 后续HandleRequestVoteReply内会加锁
    std::weak_ptr<RaftNode> weakSelf = weak_from_this();
    transport_->SendRequestVote(peerId,args,
        [weakSelf,peerId](const RequestVoteReply& reply)
        {
            if(auto self = weakSelf.lock())                 //! 生命周期：RpcClient>=RaftNode(RaftNode持有shared_ptr_RpcClient,反向则没有)，所以此处需要检查，如果用this可能悬空
            {
                self->HandleRequestVoteReply(peerId,reply);
            }
        });
}

void RaftNode::SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args)
{
    std::weak_ptr<RaftNode> weakSelf = weak_from_this();
    transport_->SendAppendEntries(peerId,args,
        [weakSelf,peerId](const AppendEntriesReply& reply)
        {
            if(auto self = weakSelf.lock())
            {
                self->HandleAppendEntriesReply(peerId,reply);
            }
        });
}

// 向所有 Follower 发送 AppendEntries：
// - Follower 已同步：entries 为空（纯心跳）
// - Follower 落后：entries 带上缺失的日志
void RaftNode::SendHeartbeat()
{
    //先构造所有args 避免多次检查身份
    std::vector<AppendEntriesArgs> allArgs(totalNodes_);
    {
        std::lock_guard<std::mutex> lock(mutex_);

        //!发心跳前额外确认身份：Leader
        if(!(role_==Role::Leader)) return;

        for(int32_t peerId = 0; peerId<totalNodes_; peerId++) 
        {
            if(peerId == nodeId_) continue;

            AppendEntriesArgs& args = allArgs[peerId];
            //心跳的作用：1. 让follower提交达成共识的日志  2. 让follower对比Leader的最后一条日志标签，进行一致性检查
            args.term = currentTerm_;
            args.leaderId = nodeId_;
            args.prevLogIndex = nextIndex_[peerId] - 1;     //! 心跳应该针对不同peer，使用不同的一致性检查(而非均用leader的lastLog) -> 更灵活的日志复制
            args.prevLogTerm = (args.prevLogIndex == 0)     //! prevLogIndex == 0时，GetEntry()返回nullopt
                               ? 0
                               : log_.GetEntry(args.prevLogIndex).value().term;     
            args.entries = log_.GetEntries(args.prevLogIndex+1, log_.LastIndex()+1);     //! GetEntries返回临时对象(右值) move()没有效果   //! 左闭右开区间
            args.leaderCommit = commitIndex_;
        }
    }

    //再一起发送RPC消息
    for(int32_t peerId = 0; peerId<totalNodes_; peerId++) 
    {
        if(peerId == nodeId_) continue;
        
        //RPC发送消息：锁外
        SendAppendEntries(peerId,allArgs[peerId]);
    }
}

void RaftNode::HandleRequestVote(const RequestVoteArgs& args, RequestVoteReply& reply)
{
    std::cout << "[Node " << nodeId_ << "] HandleRequestVote from candidate="
              << args.candidateId << " args.term=" << args.term
              << " my.term=" << currentTerm_ << std::endl;
    //此处为顶层函数 -> 加锁
    std::lock_guard<std::mutex> lock(mutex_);

    //!不能在此处设置reply.term = currentTerm_   若reply.term > currentTerm_  则follower传回时被candidate认为是一个过期的回复 该投票在candidate处不会起效 但是此时follower实际上投票了 导致错误 
    reply.voteGranted = false;
    
    //1. 检查term
    if(args.term<currentTerm_)            //▲! 过时的候选者不能成为Leader
    {
        std::cout << "[Node " << nodeId_ << "] Vote REJECT: args.term="
                  << args.term << " < my.term=" << currentTerm_ << std::endl;
        reply.term = currentTerm_;
        return;                             
    }
    else if(args.term>currentTerm_)     BecomeFollower(args.term);          //▲! 候选者比我更加新 则我跟随   !BecomeFollower内部会更新currentTerm_

    reply.term=currentTerm_;
    
    //2. 检查是否已投过票(▲!可以重复投给同一候选者_消息丢失_幂等无影响)
    if(votedFor_!=-1 && votedFor_!=args.candidateId) 
    {
        std::cout << "[Node " << nodeId_ << "] Vote REJECT: already voted for "
                  << votedFor_ << ", candidate=" << args.candidateId << std::endl;
        return;                // 根据votedFor_是否被改变来判断-是否投过票 此轮任期是否已经投票(一轮最多投一次)
    }

    //3. ▲!检查候选者的日志新旧(先比较lastLogtTerm 再比较lastLogIndex)
    if(log_.LastTerm()>args.lastLogTerm || (log_.LastTerm()==args.lastLogTerm && log_.LastIndex()>args.lastLogIndex)) 
    {
        std::cout << "[Node " << nodeId_ << "] Vote REJECT: my log (term="
                  << log_.LastTerm() << ",idx=" << log_.LastIndex()
                  << ") newer than candidate's (term=" << args.lastLogTerm
                  << ",idx=" << args.lastLogIndex << ")" << std::endl;
        return;
    }

    //4. 投票 + ▲!持久化
    votedFor_ = args.candidateId;
    reply.voteGranted = true;
    
    PersistMeta();
    
    std::cout << "[Node " << nodeId_ << "] Vote GRANTED to candidate "
              << args.candidateId << " term=" << currentTerm_ << std::endl;
}

//!注意实现逻辑
void RaftNode::HandleAppendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply)
{
    //!读取共享数据需要加锁
    std::lock_guard<std::mutex> lock(mutex_);
    
    reply.success = false;
    reply.term = currentTerm_;      //!覆盖>=<三种情况 刚才漏了=的情况
    
    //1. 检查term
    if(currentTerm_ > args.term) 
    {
        return;                     //过期的Leader
    }
    if(currentTerm_ < args.term) 
    {
        BecomeFollower(args.term);      //内部会更新currentTerm_  所以要后赋值term
        reply.term = currentTerm_;
    }

    //2. 重置选举超时
    electionResetTime_ = std::chrono::steady_clock::now();

    //3. 记录leaderId，供客户端重定向
    leaderId_ = args.leaderId;

    //4. 一致性检查
    auto check = log_.CheckConsistency(args.prevLogIndex,args.prevLogTerm);
    if(!check.ok)
    {
        reply.conflictIndex = check.conflictIndex;
        return;
    }

    //5. 日志复制(内存+磁盘)
    if(!args.entries.empty()) 
    {
        //理论上不应该出错
        if(!log_.AppendFrom(args.prevLogIndex+1,args.entries))
        {
            //! 填入非法值表示严重错误 而非日志冲突
            reply.conflictIndex = -1;
            return;
        }
        if(!storage_->AppendLogEntries(args.entries))
        {
            //! 填入非法值表示严重错误 而非日志冲突
            reply.conflictIndex = -1;
            return;
        }
    }

    //6. 更新commitIndex_
    if(args.leaderCommit > commitIndex_)
    {
        //新的commitIndex不能超过自己的LastIndex(Leader已提交的日志可能我还没有)
        commitIndex_ = std::min(args.leaderCommit,log_.LastIndex());
    }
    

    //7. 应用到状态机
    ApplyCommittedEntries();

    //8. 确认reply->success
    reply.success = true;
    reply.matchIndex = log_.LastIndex();
}

void RaftNode::HandleRequestVoteReply(int32_t peerId, const RequestVoteReply& reply)
{
    std::cout << "[Node " << nodeId_ << "] HandleRequestVoteReply from peer="
              << peerId << " reply.term=" << reply.term
              << " granted=" << reply.voteGranted
              << " my.role=" << (role_ == Role::Candidate ? "Candidate" : 
                                 role_ == Role::Leader ? "Leader" : "Follower")
              << " my.term=" << currentTerm_ << std::endl;
    //! 检查自己是否还是candidate，否则导致错误的票数记录，甚至多个leader
    //if(!(role_==Role::Candidate)) return;            //! 注意：两次加锁之间，角色可能已经变化，此处必须在锁内检查
    
    bool becomeLeader = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if(!(role_==Role::Candidate)) return;        //! 必须锁内检查

        //Leader一定是任期最大者
        if(currentTerm_ < reply.term) 
        {
            BecomeFollower(reply.term);
            return;                         //! 后续不再判断
        }
        if(currentTerm_>reply.term) return;         //▲! 过期的回复

        //投票 -> 累计 -> 判断是否过半 -> 成为Leader
        if(reply.voteGranted)
        {
            votesReceived_.insert(peerId);
            if(votesReceived_.size()>totalNodes_/2) 
            {
                BecomeLeader();
                becomeLeader = true;
            }
        }
    }

    // !RPC消息在锁外发送
    if(becomeLeader) SendHeartbeat();
}

//!注意实现逻辑
void RaftNode::HandleAppendEntriesReply(int32_t peerId, const AppendEntriesReply& reply)
{
    //bool needRetry = false;
    //AppendEntriesArgs args;
    
    bool needNotify = false;

    {   
        //!读取共享数据需要加锁
        std::lock_guard<std::mutex> lock(mutex_);
        
        //1. 检查term (判断是否为过期的Leader)
        if(currentTerm_ < reply.term) 
        {
            BecomeFollower(reply.term);      //内部会更新currentTerm_  所以要后赋值term
        }

        if(currentTerm_ > reply.term) return;   //! 过期回复

        //2. !再次检查角色
        if(!(role_==Role::Leader)) return;

        //3. success分支
        // 规定nodeId_即为nextIndex_和matchIndex_的对应下标
        if(reply.success)
        {
            matchIndex_[peerId] = reply.matchIndex;
            nextIndex_[peerId] = matchIndex_[peerId] + 1;

            int64_t oldCommitIndex = commitIndex_;
            UpdateCommitIndex();                            //! 每次收到follower提交 -> 检查是否达到大多数 -> 即检查是否达成共识
            if(oldCommitIndex < commitIndex_)
            {
                needNotify = true;
            }
        }
        else
        {
            nextIndex_[peerId] = reply.conflictIndex;
            //needRetry = true;

            //args.term = currentTerm_;
            //args.leaderId = nodeId_;
            //args.prevLogIndex = nextIndex_[peerId] - 1;
            //args.prevLogTerm = (args.prevLogIndex == 0)                 //! prevLogIndex = 0 时，GetEntry()返回nullopt
            //                   ? 0 
            //                   : log_.GetEntry(args.prevLogIndex).value().term;
            //args.entries = log_.GetEntries(reply.conflictIndex, log_.LastIndex()+1);        //! 左闭右开
            //args.leaderCommit = commitIndex_;
        }
    }

    //! RPC发送消息: 锁外 
    //if(needRetry) 
    //{
        //▲! 最好不做立即重新发送：让Tick()的心跳自然触发下一次发送
    //    SendAppendEntries(peerId,args);
    //}

    // 必须在锁外notify，否则唤醒后仍然抢不到锁
    if(needNotify)
    {
        cond_.notify_all();
    }
}

//超时：StartElection() 或 SendHeartbeat()
void RaftNode::Tick()
{
    bool shouldStartElection = false;
    bool shouldSendHeartbeat = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();            //! 学习一下使用方法
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - electionResetTime_).count();   //1 millisecond (ms，毫秒) = 1000 microseconds (us，微秒)

        if(role_ == Role::Leader)       // Leader：定期发心跳
        {
            if(elapsedMs >= static_cast<int64_t>(kHeartbeatIntervalMs_))
            {
                shouldSendHeartbeat = true;
                electionResetTime_ = now;           //! 更新心跳的时间 (稍后就发送心跳)
            }
        }
        else                            // Follower/Candidate：检查选举超时
        {
            if(elapsedMs >= static_cast<int64_t>(electionTimeoutMs_))           //! elapsedMs是int64_t类型，此处避免隐式类型转换发生错误
            {
                shouldStartElection = true;
                electionResetTime_ = now;                               //! 重置计时器，避免下次Tick()又立即超时
                electionTimeoutMs_ = randomTimeoutMs(150,300);          //! 重新随机超时时间
            }
        }
    }

    //RPC发送消息、顶层方法 -> 控制锁的粒度 -> 不在这里加锁(使用最初锁外的bool进行判断)
    if(shouldSendHeartbeat) SendHeartbeat();
    if(shouldStartElection) StartElection();
}

void RaftNode::StartElection()
{
    std::cout << "[Node " << nodeId_ << "] StartElection" << std::endl;
    RequestVoteArgs args;
    bool becomeLeader = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // ! 防御：如果已经是 Leader，不需要竞选
        if (role_ == Role::Leader) return;

        BecomeCandidate();                              //内部不加锁，由此处 顶层方法 加锁

        //临界区数据需要加锁
        args.term=currentTerm_;
        args.candidateId=nodeId_;
        args.lastLogIndex=log_.LastIndex();
        args.lastLogTerm=log_.LastTerm();

        //! 特殊情况：单节点，需要在此处检查投票是否过半，否则无法当选Leader
        if(votesReceived_.size() > totalNodes_/2)
        {
            BecomeLeader();
            becomeLeader = true;
        }
    }

    //特殊情况：单节点
    if(becomeLeader)
    {
        SendHeartbeat();            //即使没有follower
        return;
    }

    //发送RPC消息不需要加锁(Sendxxx)
    for(int32_t peerId = 0; peerId<totalNodes_; peerId++)
    {
        if(peerId == nodeId_) continue;
        SendRequestVote(peerId,args);      //发给所有人(除了自己)
    }
}

void RaftNode::BecomeFollower(int64_t newTerm)
{
    std::cout << "[Node " << nodeId_ << "] BecomeFollower, term=" << currentTerm_ 
              << " -> " << newTerm << std::endl;

    // 更新role_
    role_ = Role::Follower;

    // !▲ 关键：重置选举计时器
    electionResetTime_ = std::chrono::steady_clock::now();
    electionTimeoutMs_ = randomTimeoutMs(150, 300);   // 重新随机

    // 更新currentTerm_
    if(newTerm > currentTerm_) 
    {
        currentTerm_ = newTerm;
        votedFor_ = -1;                 //!到达新任期：投票也需要初始化(只有初始化的票才能投出)

        //! 持久化不能少 每次修改meta这两个属性都需要持久化
        PersistMeta();
    }

    // nextIndex_和matchIndex_等待重新成为Leader后再初始化覆盖
}

void RaftNode::BecomeCandidate()
{
    std::cout << "[Node " << nodeId_ << "] BecomeCandidate, term=" 
              << (currentTerm_ + 1) << std::endl;
    // 更新role_
    role_ = Role::Candidate;

    // 每次竞选都需要 自增任期: 到达新的选举轮次->更新votedFor_，在votedFor_==-1时才能进行投票
    currentTerm_++;
    //votedFor_ = -1;       省略 多余的中间态

    // 更新leaderId_
    leaderId_ = -1;

    // 新一轮：给自己投票
    votedFor_ = nodeId_;
    votesReceived_.clear();              //每次竞选 需要重新统计票数
    votesReceived_.insert(nodeId_);
    
    // 持久化meta
    PersistMeta();
    
    // !▲这里不发RPC消息，发送RPC消息(Sendxxx)在`顶层的锁外调用`

    // !▲不需要等待，而是在HandleRequestVoteReply处检查 票数累计是否过半 来判断 -> 是否成为Leader
}



void RaftNode::BecomeLeader()
{
    std::cout << "[Node " << nodeId_ << "] BecomeLeader, term=" << currentTerm_ << std::endl;

    // 更新role_
    role_ = Role::Leader;

    // 更新leaderId_
    leaderId_ = nodeId_;

    // 初始化nextIndex_和matchIndex_
    nextIndex_.assign(totalNodes_,log_.LastIndex()+1);            //! 需要+1，否则最后一条日志会重复发送
    matchIndex_.assign(totalNodes_,0);

    // 将自己的matchIndex设置为最后一条日志的index
    matchIndex_[nodeId_] = log_.LastIndex();

    //! 锁内：此处不能调用SendHeartbeat();
}


//简化版：仅检查Leader的lastIndex()
void RaftNode::UpdateCommitIndex()
{
    //▲! Leader 只能提交属于自己任期的日志：必须等到"自己任期的日志"被大多数确认后，才能顺带提交之前的日志。
    if(currentTerm_ != log_.LastTerm()) return;

    int64_t n = log_.LastIndex();
    if(n == 0) return;              //防御：空日志(不过一般不出现)
    int count = 0;
    for(int64_t matchIndex : matchIndex_)
    {
        if(matchIndex >= n) count++;
    }

    if(count > totalNodes_/2)
    {
        commitIndex_ = n;
        ApplyCommittedEntries();
    }
}

void RaftNode::ApplyCommittedEntries()
{
    while(lastApplied_ < commitIndex_)
    {
        lastApplied_++;
        const auto entryOpt = log_.GetEntry(lastApplied_);         // ! 不能使用 const LogEntry& entry = log_.GetEntry(lastApplied_).value(); 
        if(!entryOpt.has_value())
        {
            //防御：日志突然消失(commitIndex_ > lastIndex())
            break;
        }
        stateMachine_->Apply(entryOpt.value().command);
    }
    /* !
    log_.GetEntry(lastApplied_) 返回一个临时 std::optional<LogEntry>
    .value() 返回这个临时对象内部存储的 LogEntry&
    语句结束（分号处），临时 optional 被销毁
    entry 变成悬空引用
    下一行 entry.command 是未定义行为
    这是 C++ 的经典陷阱  **绑定到引用的临时对象**会延长生命周期，**但 .value() 返回的是引用**，不是临时对象，所以不会延长。
    */
}

//调用者必须加锁
bool RaftNode::PersistMeta()
{
    return storage_->AppendMeta(currentTerm_,votedFor_);
}

//启动时调用：全生命周期仅一次
bool RaftNode::Recover()
{
    //从磁盘读取元状态和日志
    auto [currentTerm,votedFor] = storage_->LoadMeta();
    auto entries = storage_->LoadLogEntries();

    //将元状态和日志存入内存
    currentTerm_=currentTerm;
    votedFor_=votedFor;

    //避免重复恢复日志 防御性编程 去掉也可以
    //log_ = RaftLog();

    return log_.AppendFrom(1,entries);         //日志初始index 从1开始
}

bool RaftNode::IsLeader() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return role_ == Role::Leader;
}

bool RaftNode::IsCandidate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return role_ == Role::Candidate;
}

Role RaftNode::GetRole() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
}

int64_t RaftNode::GetCurrentTerm() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentTerm_;
}

int32_t RaftNode::GetLeaderId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return leaderId_;
}
