#include "lsm/db.h"
#include "lsm/sstable.h"
#include <limits>
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

using namespace std::chrono_literals;
namespace {
std::mutex gateMutex;
std::condition_variable gateCv;
std::set<std::string> outputPaths;
int outputStage=0;
std::set<int> releasedStages;
int failedStage=0;
std::atomic<bool> failManifestSync{false};
thread_local bool manifestRenamed=false;
thread_local int currentOutputStage=0;
bool gateOutputs=false, gateCleanup=false, cleanupEntered=false, cleanupReleased=false;
std::atomic<int> compactWaits{0}, flushWaits{0}, cleanupFailures{0};
std::atomic<bool> failUnlink{false}, failCleanupSync{false};
thread_local int waitKind=0;
thread_local bool deletedTable=false;
std::string FdPath(int fd) {
    char name[64],path[4096];
    std::snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
    auto n=readlink(name,path,sizeof(path));
    return n<0?std::string{}:std::string(path,static_cast<size_t>(n));
}
template<class F> bool Until(F predicate) {
    const auto end=std::chrono::steady_clock::now()+5s;
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>=end)return false;
        std::this_thread::yield();
    }
    return true;
}
void ReleaseAll() {
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=false;cleanupReleased=true;}
    gateCv.notify_all();
}
void Need(bool condition) {
    if(!condition) {
        std::fprintf(stderr,"Compaction 验收同步点超时，终止进程避免等待线程析构阻塞\n");
        std::_Exit(2);
    }
    CHECK(condition);
}
void Allow(int stage) {
    {std::lock_guard<std::mutex> lock(gateMutex);releasedStages.insert(stage);}
    gateCv.notify_all();
}
bool WaitOutput(int stage) {
    std::unique_lock<std::mutex> lock(gateMutex);
    return gateCv.wait_for(lock,5s,[&]{return outputStage>=stage;});
}
void Done(std::future<void>& future) {
    Need(future.wait_for(5s)==std::future_status::ready);future.get();
}
struct Fixture {
    std::string dir=testutil::MakeTempDir("db_compaction_acceptance");
    Fixture() {
        std::lock_guard<std::mutex> lock(gateMutex);
        outputPaths.clear();releasedStages.clear();outputStage=failedStage=0;
        failManifestSync=false;
        gateOutputs=gateCleanup=cleanupEntered=cleanupReleased=false;
        compactWaits=flushWaits=cleanupFailures=0;failUnlink=failCleanupSync=false;
    }
    ~Fixture() {ReleaseAll();failUnlink=failCleanupSync=false;testutil::CleanupDir(dir);}
};
}
extern "C" void __real__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(std::condition_variable*,std::unique_lock<std::mutex>&);
extern "C" void __wrap__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(std::condition_variable* cv,std::unique_lock<std::mutex>& lock) {
    // Compact 可能等待空闲请求槽或自身完成；Flush 等待 immutable 发布。
    if(waitKind==1){++compactWaits;waitKind=0;}
    if(waitKind==2){++flushWaits;waitKind=0;}
    __real__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(cv,lock);
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t size) {
    const auto path=FdPath(fd);
    if(path.size()>=8 && path.compare(path.size()-8,8,".sst.tmp")==0) {
        std::unique_lock<std::mutex> lock(gateMutex);
        if(gateOutputs && outputPaths.insert(path).second) {
            const int stage=++outputStage;currentOutputStage=stage;gateCv.notify_all();
            gateCv.wait(lock,[&]{return !gateOutputs || releasedStages.count(stage)>0;});
            if(failedStage==stage){errno=ENOSPC;return -1;}
        }
    }
    return __real_write(fd,data,size);
}
extern "C" int __real_unlink(const char*);
extern "C" int __wrap_unlink(const char* path) {
    const bool table=std::filesystem::path(path).extension()==".sst";
    if(table) {
        std::unique_lock<std::mutex> lock(gateMutex);
        if(gateCleanup && !cleanupEntered) {
            cleanupEntered=true;gateCv.notify_all();
            gateCv.wait(lock,[]{return cleanupReleased;});
        }
    }
    if(table && failUnlink){++cleanupFailures;errno=EACCES;return -1;}
    const int result=__real_unlink(path);
    if(table && result==0)deletedTable=true;
    return result;
}
extern "C" int __real_rename(const char*,const char*);
extern "C" int __wrap_rename(const char* from,const char* to) {
    int result=__real_rename(from,to);
    if(result==0 && std::filesystem::path(to).filename()=="MANIFEST")manifestRenamed=true;
    return result;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    struct stat st{};
    if(manifestRenamed && fstat(fd,&st)==0 && S_ISDIR(st.st_mode)) {
        manifestRenamed=false;
        if(failManifestSync.exchange(false)){errno=EIO;return -1;}
    }
    if(deletedTable && fstat(fd,&st)==0 && S_ISDIR(st.st_mode)) {
        deletedTable=false;
        if(failCleanupSync){++cleanupFailures;errno=EIO;return -1;}
    }
    return __real_fsync(fd);
}
namespace {

void TestConcurrentPublicationOrders() {
    for(bool flushFirst:{false,true}) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
        {
            DB db(f.dir,options);
            db.Put("apple","old");db.Put("deleted","old");db.Flush();
            db.Put("apple","middle");db.Flush();
            const auto before=Manifest::Load(f.dir+"/MANIFEST");
            {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;}
            auto compact=std::async(std::launch::async,[&]{db.CompactLevel(0);});
            Need(WaitOutput(1));
            db.Put("apple","new");db.Delete("deleted");
            auto flush=std::async(std::launch::async,[&]{db.Flush();});
            Need(WaitOutput(2)); // 两个线程均已进入锁外输出，互不等待。
            if(flushFirst) {
                Allow(2);Done(flush);
                CHECK(compact.wait_for(0ms)==std::future_status::timeout);
                db.Put("extra","yes");
                auto nextFlush=std::async(std::launch::async,[&]{db.Flush();});
                Need(WaitOutput(3));Allow(3);Done(nextFlush);
                Allow(1);Done(compact);
            } else {
                Allow(1);Done(compact);
                CHECK(flush.wait_for(0ms)==std::future_status::timeout);
                Allow(2);Done(flush);
            }
            CHECK_STR_EQ(db.Get("apple").value_or(""),"new");
            CHECK(!db.Get("deleted"));
            const auto after=Manifest::Load(f.dir+"/MANIFEST");
            CHECK_EQ(after.Generation(),before.Generation()+(flushFirst?3u:2u));
            CHECK_EQ(after.FlushedSequence(),flushFirst?6u:5u);
            CHECK_EQ(after.Tables().size(),flushFirst?3u:2u);
            if(flushFirst)CHECK_STR_EQ(db.Get("extra").value_or(""),"yes");
            ReleaseAll();
        }
        DB reopened(f.dir,options);
        CHECK_STR_EQ(reopened.Get("apple").value_or(""),"new");CHECK(!reopened.Get("deleted"));
        if(flushFirst)CHECK_STR_EQ(reopened.Get("extra").value_or(""),"yes");
    }
}
void TestRequestBoundary() {
    Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
    DB db(f.dir,options);db.Put("a","old");db.Flush();db.Put("a","new");db.Flush();
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;gateCleanup=true;}
    auto first=std::async(std::launch::async,[&]{waitKind=1;db.CompactLevel(0);});Need(WaitOutput(1));
    auto second=std::async(std::launch::async,[&]{waitKind=1;db.CompactLevel(1);});
    Need(Until([]{return compactWaits.load()>=2;}));
    // 新表由独立刷盘线程发布，确保下一轮仍需实际合并。
    db.Put("b","value");auto flush=std::async(std::launch::async,[&]{db.Flush();});
    Need(WaitOutput(2));Allow(2);Done(flush);Allow(1);
    {std::unique_lock<std::mutex> lock(gateMutex);Need(gateCv.wait_for(lock,5s,[]{return cleanupEntered;}));}
    Done(first);CHECK(second.wait_for(0ms)==std::future_status::timeout);
    {std::lock_guard<std::mutex> lock(gateMutex);cleanupReleased=true;}gateCv.notify_all();
    Need(WaitOutput(3));CHECK(second.wait_for(0ms)==std::future_status::timeout);
    Allow(3);Done(second);ReleaseAll();
    const auto after=Manifest::Load(f.dir+"/MANIFEST");
    CHECK_EQ(after.Tables().size(),2u);
    size_t l0=0,l2=0;
    for(const auto& meta:after.Tables()){l0+=meta.level==0;l2+=meta.level==2;}
    CHECK_EQ(l0,1u);CHECK_EQ(l2,1u);
    CHECK_STR_EQ(db.Get("a").value_or(""),"new");
    CHECK_STR_EQ(db.Get("b").value_or(""),"value");
}
void TestSaveFailureBlocksOtherPublisher() {
    for(bool flushFails:{false,true})for(bool secondBuildFails:{false,true}) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false; // 保留 WAL，以恢复未发布的刷盘任务。
        uint64_t generation=0;
        {
            DB db(f.dir,options);db.Put("a","old");db.Flush();db.Put("a","new");db.Flush();
            generation=Manifest::Load(f.dir+"/MANIFEST").Generation();
            {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;}
            auto submit=[&](bool compact){try{if(compact)db.CompactLevel(0);else db.Flush();}catch(const std::system_error& e){return e.code().value();}catch(...){return -1;}return 0;};
            auto compact=std::async(std::launch::async,[&]{return submit(true);});Need(WaitOutput(1));
            db.Put("b","wal");auto flush=std::async(std::launch::async,[&]{return submit(false);});Need(WaitOutput(2));
            // 前一请求尚未完成，另一个源层请求等待槽位；故障必须同时唤醒它。
            auto queued=std::async(std::launch::async,[&]{
                waitKind=1;
                try{db.CompactLevel(1);}catch(const std::system_error& e){return e.code().value();}catch(...){return -1;}
                return 0;
            });
            Need(Until([]{return compactWaits.load()>=1;}));
            const int first=flushFails?2:1, second=flushFails?1:2;
            if(secondBuildFails){std::lock_guard<std::mutex> lock(gateMutex);failedStage=second;}
            failManifestSync=true;Allow(first);
            auto& failed=flushFails?flush:compact;
            Need(failed.wait_for(5s)==std::future_status::ready);CHECK_EQ(failed.get(),EIO);
            // rename 已成功但目录同步失败：磁盘 MANIFEST 已改变。
            CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").Generation(),generation+1);
            Allow(second);
            auto& other=flushFails?compact:flush;
            Need(other.wait_for(5s)==std::future_status::ready);CHECK_EQ(other.get(),EIO);
            Need(queued.wait_for(5s)==std::future_status::ready);CHECK_EQ(queued.get(),EIO);
            ReleaseAll();
        } // join 后确认第二个任务没有再写 MANIFEST。
        CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").Generation(),generation+1);
        DB recovered(f.dir,options);
        CHECK_STR_EQ(recovered.Get("a").value_or(""),"new");
        CHECK_STR_EQ(recovered.Get("b").value_or(""),"wal");
        recovered.Flush();
    }
}
void TestShutdownJoinsBothTasks() {
    Fixture f;DBOptions options;options.enableAutoCompaction=false;options.memTableSizeLimit=1;
    auto db=std::make_unique<DB>(f.dir,options);
    db->Put("a","old");db->Flush();db->Put("a","new");db->Flush();
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;gateCleanup=true;}
    auto compact=std::async(std::launch::async,[&]{db->CompactLevel(0);});Need(WaitOutput(1));
    Allow(1);
    {std::unique_lock<std::mutex> lock(gateMutex);Need(gateCv.wait_for(lock,5s,[]{return cleanupEntered;}));}
    Done(compact); // 结束所有前台 API 后，才允许析构。
    db->Put("b","flushed");db->Put("c","active");Need(WaitOutput(2));
    auto shutdown=std::async(std::launch::async,[owned=std::move(db)]()mutable{owned.reset();});
    CHECK(shutdown.wait_for(50ms)==std::future_status::timeout);
    Allow(2);
    CHECK(shutdown.wait_for(50ms)==std::future_status::timeout); // 合并清理仍未结束。
    ReleaseAll();Done(shutdown);
    auto m=Manifest::Load(f.dir+"/MANIFEST");CHECK_EQ(m.FlushedSequence(),3u);
    DB recovered(f.dir,options);
    CHECK_STR_EQ(recovered.Get("a").value_or(""),"new");
    CHECK_STR_EQ(recovered.Get("b").value_or(""),"flushed");
    CHECK_STR_EQ(recovered.Get("c").value_or(""),"active");
}
void TestCleanupFailuresRemainHealthy() {
    for(bool syncFailure:{false,true}) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
        std::vector<std::string> oldPaths;
        {
            DB db(f.dir,options);
            db.Put("apple","old");db.Flush();db.Put("apple","new");db.Flush();
            const auto before=Manifest::Load(f.dir+"/MANIFEST");
            for(const auto& meta:before.Tables()) {
                char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)meta.fileNumber);
                oldPaths.push_back(f.dir+name);
            }
            failUnlink=!syncFailure;failCleanupSync=syncFailure;
            db.CompactLevel(0);
            Need(Until([]{return cleanupFailures.load()>0;}));
            // 下一任务只能在上一轮清理结束后执行；它成功表明后台未进入故障。
            db.CompactLevel(0);
            CHECK_STR_EQ(db.Get("apple").value_or(""),"new");
            db.Put("banana","yellow");db.Flush();
            CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),3u);
        }
        failUnlink=failCleanupSync=false;
        CHECK(cleanupFailures.load()>0);
        for(const auto& path:oldPaths)CHECK_EQ(std::filesystem::exists(path),!syncFailure);
        // 删除失败留下的孤儿表不得参与恢复；目录同步失败也不影响正式表。
        DB reopened(f.dir,options);
        CHECK_STR_EQ(reopened.Get("apple").value_or(""),"new");
        CHECK_STR_EQ(reopened.Get("banana").value_or(""),"yellow");
    }
}

// 自动调度测试沿用系统调用门控，不向引擎增加测试接口。
TableMeta SeedTable(const Fixture& f,uint64_t number,uint32_t level,
                    const std::string& key,uint64_t sequence=1) {
    SSTableBuilder builder(1);
    lsm::Entry entry{{key,sequence,ValueType::kValue},"value"};
    builder.Add(entry);builder.Finish();
    TableMeta meta;meta.fileNumber=number;meta.level=level;
    meta.smallest=meta.largest=entry.key;meta.minSequence=meta.maxSequence=sequence;
    char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)number);
    meta.fileSize=SSTableWriter::Write(f.dir+name,builder);
    return meta;
}
void SeedManifest(const Fixture& f,const std::vector<TableMeta>& tables) {
    Manifest m;uint64_t sequence=0;
    for(const auto& t:tables){m.AddTable(t);sequence=std::max(sequence,t.maxSequence);}
    m.SetLastSequence(sequence);m.SetFlushedSequence(sequence);m.SetGeneration(1);
    m.Save(f.dir+"/MANIFEST");
}
size_t CountLevel(const Manifest& m,uint32_t level) {
    size_t n=0;for(const auto& t:m.Tables())n+=t.level==level;return n;
}
void WaitCleanup() {
    std::unique_lock<std::mutex> lock(gateMutex);
    Need(gateCv.wait_for(lock,5s,[]{return cleanupEntered;}));
}
DBOptions AutoOptions() {
    DBOptions options;options.enableWal=false;options.maxLevels=3;
    options.enableAutoCompaction=true;options.targetSSTableDataSize=1024;
    options.level1TargetBytes=UINT64_MAX;
    return options;
}
void TestAutoFlushTriggerAndFlushBoundary() {
    Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=2;
    {
        DB db(f.dir,options);
        db.Put("a","old");db.Flush();
        auto before=Manifest::Load(f.dir+"/MANIFEST");CHECK_EQ(CountLevel(before,0),1u);
        {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;}
        db.Put("a","new");db.Delete("gone");
        auto flush=std::async(std::launch::async,[&]{db.Flush();});
        Need(WaitOutput(1));Allow(1);Done(flush);
        Need(WaitOutput(2)); // 恰好达到阈值，自动任务已领取；Flush 未等待它完成。
        auto during=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(CountLevel(during,0),2u);CHECK_EQ(during.FlushedSequence(),3u);
        CHECK_STR_EQ(db.Get("a").value_or(""),"new");CHECK(!db.Get("gone"));
        {std::lock_guard<std::mutex> lock(gateMutex);gateCleanup=true;}
        Allow(2);WaitCleanup();
        auto after=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(CountLevel(after,0),0u);CHECK_EQ(CountLevel(after,1),1u);
        CHECK_EQ(after.FlushedSequence(),3u);
        ReleaseAll();
    }
    options.enableAutoCompaction=false;DB reopened(f.dir,options);
    CHECK_STR_EQ(reopened.Get("a").value_or(""),"new");CHECK(!reopened.Get("gone"));
    const auto m=Manifest::Load(f.dir+"/MANIFEST");
    for(const auto& t:m.Tables()) {
        char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)t.fileNumber);
        SSTableReader reader(f.dir+name);
        for(auto it=reader.Begin();it.Valid();it.Next()) {
            if(it.Key().user_key=="gone") {CHECK(it.Key().type==ValueType::kDeletion);CHECK_EQ(it.Key().sequence,3u);}
        }
    }
}
void TestAutoRecoveryScoreAndTie() {
    for(bool higher:{false,true}) {
        Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=2;
        auto a=SeedTable(f,1,0,"a",2),b=SeedTable(f,2,0,"b",3),z=SeedTable(f,3,1,"z",1);
        options.level1TargetBytes=higher?z.fileSize/2:z.fileSize;
        SeedManifest(f,{a,b,z});
        {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;gateCleanup=true;}
        DB db(f.dir,options);Need(WaitOutput(1)); // 无前台调用、无额外通知也能启动。
        Allow(1);WaitCleanup();
        const auto first=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(first.Generation(),2u);
        CHECK_EQ(CountLevel(first,0),higher?2u:0u);
        CHECK_EQ(CountLevel(first,2),higher?1u:0u);
        CHECK_STR_EQ(db.Get("a").value_or(""),"value");
        ReleaseAll();
    }
}
void TestAutoCascadeAndLastLevel() {
    Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=1;
    options.level1TargetBytes=1;options.maxCompactionSourceBytes=1;options.maxLevels=4;
    SeedManifest(f,{SeedTable(f,1,0,"a")});
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;gateCleanup=true;}
    {
        DB db(f.dir,options);
        for(int stage=1;stage<=3;++stage) {
            Need(WaitOutput(stage));
            if(stage==1) {
                Allow(stage);WaitCleanup();
                {std::lock_guard<std::mutex> lock(gateMutex);cleanupReleased=true;}gateCv.notify_all();
            } else Allow(stage);
        }
        // 空源层手动请求是后台串行栅栏，不改变已经开始的自动任务。
        db.CompactLevel(0);
        const auto m=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(m.Generation(),4u);CHECK_EQ(CountLevel(m,3),1u);CHECK_EQ(m.Tables().size(),1u);
        CHECK_STR_EQ(db.Get("a").value_or(""),"value");ReleaseAll();
    }
    DB reopened(f.dir,options);CHECK_STR_EQ(reopened.Get("a").value_or(""),"value");
}
void TestAutoDisabledAndSingleLevel() {
    for(bool single:{false,true}) {
        Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=1;
        options.enableAutoCompaction=single;options.maxLevels=single?1:3;
        SeedManifest(f,{SeedTable(f,1,0,"a")});
        {
            DB db(f.dir,options);db.Put("b","value");db.Flush();
            if(!single)db.CompactLevel(1); // 队列已被处理，L0仍不应自动下推。
            CHECK_STR_EQ(db.Get("a").value_or(""),"value");
        }
        auto m=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(m.Generation(),2u);CHECK_EQ(CountLevel(m,0),2u);
    }
}
void TestAutoCapacitySaturation() {
    Fixture f;auto options=AutoOptions();options.maxLevels=5;
    options.level1TargetBytes=UINT64_MAX/2+1;options.levelSizeMultiplier=2;
    SeedManifest(f,{SeedTable(f,1,2,"a"),SeedTable(f,2,3,"b",2)});
    {
        DB db(f.dir,options);db.CompactLevel(0);
        CHECK_STR_EQ(db.Get("a").value_or(""),"value");
    }
    const auto m=Manifest::Load(f.dir+"/MANIFEST");
    CHECK_EQ(m.Generation(),1u);CHECK_EQ(CountLevel(m,2),1u);CHECK_EQ(CountLevel(m,3),1u);
    for(int mode=0;mode<4;++mode) {
        Fixture invalid;auto opts=AutoOptions();
        if(mode==0)opts.level0CompactionTrigger=0;
        if(mode==1)opts.level1TargetBytes=0;
        if(mode>=2)opts.levelSizeMultiplier=mode-2;
        bool caught=false;try{DB db(invalid.dir,opts);}catch(const std::invalid_argument&){caught=true;}
        CHECK(caught);
    }
}
void TestAutoManualPriorityAndConcurrentFlush() {
    Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=1;options.maxCompactionSourceBytes=1;
    SeedManifest(f,{SeedTable(f,1,0,"a"),SeedTable(f,2,0,"b",2)});
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;gateCleanup=true;}
    DB db(f.dir,options);Need(WaitOutput(1));
    auto manual=std::async(std::launch::async,[&]{waitKind=1;db.CompactLevel(1);});
    Need(Until([]{return compactWaits.load()>=1;}));
    Allow(1);WaitCleanup();
    CHECK(manual.wait_for(0ms)==std::future_status::timeout); // 自动完成不能完成手动编号。
    {std::lock_guard<std::mutex> lock(gateMutex);cleanupReleased=true;}gateCv.notify_all();
    Need(WaitOutput(2));CHECK(manual.wait_for(0ms)==std::future_status::timeout);
    db.Put("c","fresh");auto flush=std::async(std::launch::async,[&]{db.Flush();});
    Need(WaitOutput(3));Allow(3);Done(flush); // 合并期间 Flush 独立发布 L0。
    Allow(2);Done(manual);
    const auto m=Manifest::Load(f.dir+"/MANIFEST");
    CHECK_EQ(CountLevel(m,2),1u); // 第二轮优先手动 L1，而非剩余 L0 自动任务。
    CHECK_STR_EQ(db.Get("a").value_or(""),"value");
    CHECK_STR_EQ(db.Get("b").value_or(""),"value");
    CHECK_STR_EQ(db.Get("c").value_or(""),"fresh");ReleaseAll();
}
void TestAutoFailureWakesManualAndRecovery() {
    for(bool saveFailure:{false,true}) {
        Fixture f;auto options=AutoOptions();options.enableWal=true;options.level0CompactionTrigger=1;
        {
            DB db(f.dir,options);
            {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;}
            db.Put("a","value");auto flush=std::async(std::launch::async,[&]{db.Flush();});
            Need(WaitOutput(1));Allow(1);Done(flush);Need(WaitOutput(2));
            db.Put("b","wal-only");
            auto manual=std::async(std::launch::async,[&]{
                waitKind=1;try{db.CompactLevel(1);}catch(const std::system_error& e){return e.code().value();}
                return 0;
            });
            Need(Until([]{return compactWaits.load()>=1;}));
            if(saveFailure)failManifestSync=true;
            else {std::lock_guard<std::mutex> lock(gateMutex);failedStage=2;}
            Allow(2);Need(manual.wait_for(5s)==std::future_status::ready);
            CHECK_EQ(manual.get(),saveFailure?EIO:ENOSPC);
            bool failed=false;try{db.Get("a");}catch(const std::system_error&){failed=true;}CHECK(failed);
            ReleaseAll();
        }
        options.enableAutoCompaction=false;DB recovered(f.dir,options);
        CHECK_STR_EQ(recovered.Get("a").value_or(""),"value");
        CHECK_STR_EQ(recovered.Get("b").value_or(""),"wal-only");
    }
}
void TestAutoShutdownJoinsInFlight() {
    Fixture f;auto options=AutoOptions();options.level0CompactionTrigger=1;
    SeedManifest(f,{SeedTable(f,1,0,"a")});
    {std::lock_guard<std::mutex> lock(gateMutex);gateOutputs=true;}
    auto db=std::make_unique<DB>(f.dir,options);Need(WaitOutput(1));
    auto shutdown=std::async(std::launch::async,[owned=std::move(db)]()mutable{owned.reset();});
    CHECK(shutdown.wait_for(50ms)==std::future_status::timeout);
    Allow(1);Done(shutdown);ReleaseAll();
    const auto m=Manifest::Load(f.dir+"/MANIFEST");CHECK_EQ(CountLevel(m,1),1u);
    options.enableAutoCompaction=false;DB reopened(f.dir,options);
    CHECK_STR_EQ(reopened.Get("a").value_or(""),"value");
}

}
int main() {
    RUN_TEST(TestAutoFlushTriggerAndFlushBoundary);
    RUN_TEST(TestAutoRecoveryScoreAndTie);
    RUN_TEST(TestAutoCascadeAndLastLevel);
    RUN_TEST(TestAutoDisabledAndSingleLevel);
    RUN_TEST(TestAutoCapacitySaturation);
    RUN_TEST(TestAutoManualPriorityAndConcurrentFlush);
    RUN_TEST(TestAutoFailureWakesManualAndRecovery);
    RUN_TEST(TestAutoShutdownJoinsInFlight);
    RUN_TEST(TestConcurrentPublicationOrders);
    RUN_TEST(TestRequestBoundary);
    RUN_TEST(TestSaveFailureBlocksOtherPublisher);
    RUN_TEST(TestShutdownJoinsBothTasks);
    RUN_TEST(TestCleanupFailuresRemainHealthy);
    TEST_SUMMARY_RETURN();
}
