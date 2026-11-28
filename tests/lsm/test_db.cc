#include "lsm/db.h"
#include "lsm/wal.h"
#include "lsm/sstable.h"
#include <map>
#include <set>
#include "wal_test_util.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
namespace {
// 故障注入状态只通过原子变量或专用锁访问，避免测试自身产生数据竞争。
std::mutex gateMutex;
std::condition_variable gateCv;
bool armed=false, entered=false, released=false;
std::atomic<bool> failTable{false}, failDelete{false}, shortWrite{false}, shortRead{false};
std::atomic<int> writeCalls{0}, readCalls{0};
std::string FdPath(int fd) {
    char path[64], target[4096];
    std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    const auto n=readlink(path,target,sizeof(target));
    return n<0 ? std::string{} : std::string(target,static_cast<size_t>(n));
}
bool IsTable(int fd) { return FdPath(fd).find(".sst.tmp")!=std::string::npos; }
}
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" ssize_t __real_read(int,void*,size_t);
extern "C" int __real_unlink(const char*);
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t size) {
    if(IsTable(fd)) {
        std::unique_lock<std::mutex> lock(gateMutex);
        if(armed) {
            entered=true; gateCv.notify_all();
            gateCv.wait(lock,[]{return released;});
        }
        if(failTable) { errno=ENOSPC; return -1; }
    }
    if(shortWrite) {
        if(++writeCalls==1) {errno=EINTR; return -1;}
        size=std::min(size,size_t{3});
    }
    return __real_write(fd,data,size);
}
extern "C" ssize_t __wrap_read(int fd,void* data,size_t size) {
    if(shortRead) {
        if(++readCalls==1) {errno=EINTR; return -1;}
        size=std::min(size,size_t{3});
    }
    return __real_read(fd,data,size);
}
extern "C" int __wrap_unlink(const char* path) {
    if(failDelete && std::string(path).find(".wal")!=std::string::npos) {
        errno=EACCES; return -1;
    }
    return __real_unlink(path);
}
namespace {
void Release() {
    {std::lock_guard<std::mutex> lock(gateMutex); released=true; armed=false;}
    gateCv.notify_all();
}
struct Fixture {
    std::string dir=testutil::MakeTempDir("db_background");
    ~Fixture() {Release(); failTable=false; failDelete=false; shortWrite=false; shortRead=false; testutil::CleanupDir(dir);}
    std::string Wal(unsigned n) const {
        char name[32]; std::snprintf(name,sizeof(name),"%06u.wal",n);
        return dir+"/"+name;
    }
};
struct Gate {
    Gate() {std::lock_guard<std::mutex> lock(gateMutex); armed=true; entered=false; released=false;}
    ~Gate() {Release();}
    bool Wait() {
        std::unique_lock<std::mutex> lock(gateMutex);
        return gateCv.wait_for(lock,3s,[]{return entered;});
    }
};
template<class Error,class F> void Throws(F action) {
    bool caught=false;
    try{action();}catch(const Error&){caught=true;}catch(...){}
    CHECK(caught);
}
void Save(const std::string& path,const std::string& data) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(data.data(),data.size());
    if(!out) throw std::runtime_error("测试数据写入失败");
}
std::string Load(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
lsm::Entry E(uint64_t seq,std::string key="apple",std::string value="red") {
    return {{std::move(key),seq,ValueType::kValue},std::move(value)};
}
void Value(DB& db,std::string_view key,const std::string& expected) {
    auto v=db.Get(key); CHECK(v.has_value()); if(v)CHECK_STR_EQ(*v,expected);
}
size_t WalCount(const Fixture& f) {
    size_t n=0; for(const auto& e:std::filesystem::directory_iterator(f.dir))
        if(e.path().extension()==".wal")++n;
    return n;
}
void TestBasicFlushAndReopen() {
    Fixture f;
    const std::string binary("a\0\xff",3);
    {
        DB db(f.dir); db.Flush(); CHECK(!db.Get("missing"));
        db.Put("apple","red"); db.Put(binary,""); db.Flush();
        Value(db,"apple","red"); Value(db,binary,"");
        db.Delete("apple"); CHECK(!db.Get("apple"));
        db.Flush(); CHECK(!db.Get("apple"));
        db.Put("apple","green"); // 析构不刷active，依赖分段WAL恢复。
    }
    {
        DB db(f.dir); Value(db,"apple","green"); Value(db,binary,"");
        db.Flush();
        auto m=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(m.FlushedSequence(),4u); CHECK_EQ(m.Tables().size(),3u);
    }
    CHECK_EQ(WalCount(f),1u); // 析构join后，已发布任务的清理完成。
}
void TestMultiSegmentRecovery() {
    Fixture f;
    Save(f.Wal(1),waltest::Frame(E(1)));
    Save(f.Wal(3),waltest::Frame(E(7,"banana","yellow")));
    Save(f.Wal(4),"");
    {
        DB db(f.dir); Value(db,"apple","red"); Value(db,"banana","yellow");
        CHECK(std::filesystem::exists(f.Wal(5)));
        db.Put("pear","green"); db.Flush();
        CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),8u);
    }
    {DB db(f.dir); Value(db,"pear","green");}
}
void TestTruncatedSegments() {
    const auto good=waltest::Frame(E(1)), tail=waltest::Frame(E(2,"banana"));
    for(size_t n=1;n<tail.size();++n) {
        Fixture f; Save(f.Wal(1),good+tail.substr(0,n));
        {
            DB db(f.dir); Value(db,"apple","red"); CHECK(!db.Get("banana"));
            CHECK_EQ(std::filesystem::file_size(f.Wal(1)),good.size());
            db.Put("pear","green");
        }
        {DB db(f.dir); Value(db,"pear","green");}
    }
    Fixture f; Save(f.Wal(1),good+"x"); Save(f.Wal(2),"");
    Throws<std::runtime_error>([&]{DB db(f.dir);});
    CHECK_STR_EQ(Load(f.Wal(1)),good+"x");
}
void TestCorruptionAndOrdering() {
    for(int mode=0;mode<3;++mode) {
        Fixture f; auto first=waltest::Frame(E(3));
        if(mode==0) first.back()^=1;
        Save(f.Wal(1),first);
        Save(f.Wal(2),mode==0 ? "" : waltest::Frame(E(mode==1?3:2,"banana")));
        Throws<std::runtime_error>([&]{DB db(f.dir);});
        CHECK_STR_EQ(Load(f.Wal(1)),first);
    }
    Fixture f; Save(f.Wal(1),waltest::Frame(E(UINT64_MAX)));
    DB db(f.dir); Value(db,"apple","red");
    Throws<std::overflow_error>([&]{db.Put("new","x");});
}
void TestWalOverlapAndOrphans() {
    Fixture f; std::string old;
    {
        DB db(f.dir); db.Put("apple","red"); db.Delete("apple");
        old=Load(f.Wal(1)); db.Flush();
    }
    Save(f.Wal(1),old);
    Save(f.Wal(2),waltest::Frame(E(5,"banana","yellow")));
    Save(f.dir+"/000009.sst.tmp","orphan");
    {
        DB db(f.dir); CHECK(!db.Get("apple")); Value(db,"banana","yellow"); db.Flush();
        auto m=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(m.Tables().back().minSequence,5u);
        CHECK_EQ(m.Tables().back().fileNumber,10u);
    }
}
void TestWalModesAndSmallThreshold() {
    for(bool enable:{false,true}) {
        Fixture f;
        {
            DB db(f.dir,DBOptions{enable,1});
            db.Put("apple","red"); db.Delete("apple"); db.Put("banana","");
            db.Flush(); CHECK(!db.Get("apple")); Value(db,"banana","");
        }
        {DB db(f.dir,DBOptions{enable,1}); CHECK(!db.Get("apple")); Value(db,"banana","");}
        CHECK_EQ(WalCount(f)>0,enable);
    }
}
void TestForegroundAndBackpressure() {
    Fixture f; DB db(f.dir,DBOptions{true,1}); db.Put("apple","red");
    Gate gate;
    db.Put("banana","yellow"); // A进入后台，B继续接收本次写入。
    if(!gate.Wait()){Release(); CHECK(false); return;}
    Value(db,"apple","red"); Value(db,"banana","yellow");
    std::promise<void> started;
    auto writer=std::async(std::launch::async,[&]{started.set_value();db.Put("pear","green");});
    started.get_future().wait();
    const bool blocked=writer.wait_for(100ms)==std::future_status::timeout;
    Release(); CHECK(blocked);
    REQUIRE(writer.wait_for(3s)==std::future_status::ready); writer.get();
    db.Flush(); Value(db,"pear","green");
}
void TestExplicitFlushWaits() {
    Fixture f; DB db(f.dir); db.Put("apple","red"); Gate gate;
    auto flush=std::async(std::launch::async,[&]{db.Flush();});
    if(!gate.Wait()){Release(); CHECK(false); flush.get(); return;}
    CHECK(flush.wait_for(0ms)==std::future_status::timeout);
    // 后台停在锁外写SSTable，前台可以读immutable并写新active。
    auto frontend=std::async(std::launch::async,[&]{
        auto v=db.Get("apple"); db.Put("banana","yellow"); return v;
    });
    const bool ready=frontend.wait_for(1s)==std::future_status::ready;
    Release(); CHECK(ready);
    const auto value=frontend.get(); CHECK(value && *value=="red");
    REQUIRE(flush.wait_for(3s)==std::future_status::ready); flush.get();
    db.Flush();
    CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),2u);
}
void TestBackgroundFailureWakesWaiters() {
    Fixture f;
    {
        DB db(f.dir,DBOptions{true,1}); db.Put("apple","red"); Gate gate;
        db.Put("banana","yellow");
        if(!gate.Wait()){Release(); CHECK(false); return;}
        auto writer=std::async(std::launch::async,[&]{
            try{db.Put("pear","green");}catch(const std::system_error&){return true;}return false;
        });
        auto flush=std::async(std::launch::async,[&]{
            try{db.Flush();}catch(const std::system_error&){return true;}return false;
        });
        failTable=true; Release();
        REQUIRE(writer.wait_for(3s)==std::future_status::ready); CHECK(writer.get());
        REQUIRE(flush.wait_for(3s)==std::future_status::ready); CHECK(flush.get());
        Throws<std::system_error>([&]{db.Get("apple");});
    }
    failTable=false;
    {DB db(f.dir); Value(db,"apple","red"); Value(db,"banana","yellow");CHECK(!db.Get("pear"));db.Flush();}
}
void TestCleanupFailureIsNotFlushFailure() {
    Fixture f; failDelete=true;
    {
        DB db(f.dir); db.Put("apple","red"); db.Flush(); db.Put("banana","yellow");
    }
    CHECK(std::filesystem::exists(f.Wal(1)));
    failDelete=false;
    {DB db(f.dir);Value(db,"apple","red");Value(db,"banana","yellow");db.Flush();}
}
void TestShutdownDuringBuild() {
    Fixture f;
    auto db=std::make_unique<DB>(f.dir,DBOptions{true,1});
    db->Put("apple","red"); Gate gate; db->Put("banana","yellow");
    if(!gate.Wait()){Release();CHECK(false);return;}
    auto shutdown=std::async(std::launch::async,[owned=std::move(db)]() mutable {owned.reset();});
    const bool blocked=shutdown.wait_for(100ms)==std::future_status::timeout;
    Release(); CHECK(blocked);
    REQUIRE(shutdown.wait_for(3s)==std::future_status::ready); shutdown.get();
    {DB recovered(f.dir);Value(recovered,"apple","red");Value(recovered,"banana","yellow");}
}
void TestConcurrentCallers() {
    Fixture f; DB db(f.dir,DBOptions{true,2048});
    std::vector<std::future<void>> workers;
    for(int t=0;t<4;++t) workers.push_back(std::async(std::launch::async,[&,t]{
        for(int i=0;i<30;++i) {
            auto key=std::to_string(t)+":"+std::to_string(i);
            db.Put(key,"value");
            if(!db.Get(key))throw std::runtime_error("并发写入后查询失败");
            if(i%3==0)db.Delete(key);
            if(i%10==0)db.Flush();
        }
    }));
    for(auto& w:workers){REQUIRE(w.wait_for(5s)==std::future_status::ready);w.get();}
    db.Flush();
    for(int t=0;t<4;++t)for(int i=0;i<30;++i){
        auto key=std::to_string(t)+":"+std::to_string(i);
        CHECK_EQ(db.Get(key).has_value(),i%3!=0);
    }
}
void TestShortIoAndInvalidInput() {
    Fixture f;
    shortWrite=true; writeCalls=0;
    {
        DB db(f.dir);db.Put("apple","red");db.Flush();
    }
    shortWrite=false;shortRead=true;readCalls=0;
    {
        DB db(f.dir);Value(db,"apple","red");
        const std::string large(lsm::kWalMaxEntrySize,'x');
        Throws<std::invalid_argument>([&]{db.Put("huge",large);});
        db.Put("banana","yellow");db.Flush();
    }
    shortRead=false;
}
// 层间合并独立于刷盘边界：空层直接完成，单表也要下推。
void TestCompactNoOp() {
    Fixture f;
    DB db(f.dir);
    db.CompactLevel(0); db.CompactLevel(0);
    CHECK(!std::filesystem::exists(f.dir+"/MANIFEST"));
    db.Put("apple","red"); db.Flush();
    const auto before=Manifest::Load(f.dir+"/MANIFEST");
    db.CompactLevel(0); db.CompactLevel(0);
    const auto after=Manifest::Load(f.dir+"/MANIFEST");
    CHECK_EQ(after.Generation(),before.Generation()+1);
    CHECK_EQ(after.Tables().front().level,1u);
    CHECK_EQ(after.Tables().size(),1u);
    Value(db,"apple","red");
}
void TestCompactSplitAndReopen() {
    Fixture f;
    DBOptions options;options.enableAutoCompaction=false; options.enableWal=false; options.targetSSTableDataSize=100;
    uint64_t flushed=0;
    {
        DB db(f.dir,options);
        for(int i=0;i<20;++i)db.Put("key"+std::to_string(i),std::string(40,'a'));
        db.Flush();
        for(int i=0;i<10;++i)db.Put("key"+std::to_string(i),std::string(40,'b'));
        db.Delete("key19"); db.Flush();
        const auto before=Manifest::Load(f.dir+"/MANIFEST");
        flushed=before.FlushedSequence();
        // active 中的数据不属于 Compact 的输入，也不能推动刷盘边界。
        db.Put("active","pending");
        db.CompactLevel(0);
        const auto after=Manifest::Load(f.dir+"/MANIFEST");
        CHECK_EQ(after.FlushedSequence(),flushed);
        CHECK_EQ(after.Generation(),before.Generation()+1);
        CHECK(after.Tables().size()>1);
        std::map<std::string,lsm::Entry> records;
        std::set<uint64_t> numbers;
        for(const auto& meta:after.Tables()) {
            numbers.insert(meta.fileNumber);
            char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)meta.fileNumber);
            CHECK_EQ(std::filesystem::file_size(f.dir+name),meta.fileSize);
            SSTableReader reader(f.dir+name);
            uint64_t lo=UINT64_MAX,hi=0;
            for(auto it=reader.Begin();it.Valid();it.Next()) {
                CHECK(records.emplace(it.Key().user_key,lsm::Entry{it.Key(),it.Value()}).second);
                lo=std::min(lo,it.Key().sequence);hi=std::max(hi,it.Key().sequence);
            }
            CHECK_EQ(lo,meta.minSequence);CHECK_EQ(hi,meta.maxSequence);
        }
        CHECK_EQ(records.size(),20u);
        CHECK_EQ(records.at("key0").key.sequence,21u);
        CHECK_EQ(records.at("key19").key.sequence,31u);
        CHECK_EQ(records.at("key19").key.type,ValueType::kDeletion);
        CHECK(!records.count("active"));
        Value(db,"active","pending");
        for(const auto& meta:before.Tables())CHECK(!numbers.count(meta.fileNumber));
    }
    // 析构 join 后旧文件清理已经结束；关闭 WAL 重开只依赖正式表。
    {
        DB db(f.dir,options);
        CHECK(!db.Get("active"));CHECK(!db.Get("key19"));
        for(int i=0;i<19;++i)Value(db,"key"+std::to_string(i),std::string(40,i<10?'b':'a'));
        CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),flushed);
    }
}
void TestCompactConcurrentRequestsAndFlush() {
    Fixture f; DBOptions options;options.enableAutoCompaction=false;options.blockCacheCapacity=0;
    DB db(f.dir,options);
    db.Put("apple","old");db.Flush();
    db.Put("apple","new");db.Flush();
    Gate gate;
    auto first=std::async(std::launch::async,[&]{db.CompactLevel(0);});
    if(!gate.Wait()){Release();CHECK(false);return;}
    // 后台正在锁外构建，此时前台可查询、写入，并提交后续任务。
    Value(db,"apple","new");db.Put("banana","yellow");
    auto second=std::async(std::launch::async,[&]{db.CompactLevel(0);});
    auto flush=std::async(std::launch::async,[&]{db.Flush();});
    CHECK(first.wait_for(50ms)==std::future_status::timeout);
    CHECK(flush.wait_for(50ms)==std::future_status::timeout);
    Release();
    REQUIRE(first.wait_for(3s)==std::future_status::ready);first.get();
    REQUIRE(second.wait_for(3s)==std::future_status::ready);second.get();
    REQUIRE(flush.wait_for(3s)==std::future_status::ready);flush.get();
    Value(db,"apple","new");Value(db,"banana","yellow");
    db.CompactLevel(0);
    const auto after=Manifest::Load(f.dir+"/MANIFEST");
    CHECK(!after.Tables().empty());
    for(const auto& meta:after.Tables())CHECK_EQ(meta.level,1u);
}
void TestCompactFailureRecovery() {
    // 构建失败与 MANIFEST 保存失败均须唤醒调用者、保留正式输入。
    for(bool manifestFailure:{false,true}) {
        Fixture f;
        uint64_t generation=0;
        {
            DBOptions options;options.enableAutoCompaction=false;options.blockCacheCapacity=0;
            DB db(f.dir,options);
            db.Put("apple","old");db.Flush();
            db.Put("apple","new");db.Delete("deleted");db.Flush();
            generation=Manifest::Load(f.dir+"/MANIFEST").Generation();
            if(manifestFailure)std::filesystem::create_directory(f.dir+"/MANIFEST.tmp");
            else failTable=true;
            auto compact=std::async(std::launch::async,[&]{
                try{db.CompactLevel(0);}catch(const std::exception&){return true;}return false;
            });
            REQUIRE(compact.wait_for(3s)==std::future_status::ready);CHECK(compact.get());
            CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").Generation(),generation);
            Throws<std::exception>([&]{db.Get("apple");});
        }
        failTable=false;
        if(manifestFailure)std::filesystem::remove(f.dir+"/MANIFEST.tmp");
        DB db(f.dir);Value(db,"apple","new");CHECK(!db.Get("deleted"));db.CompactLevel(0);
    }
}


// 构造多层夹具，独立验证恢复排序、范围定位与目标层重叠选择。
TableMeta AddTable(const Fixture& f,uint64_t number,uint32_t level,
                   const std::vector<lsm::Entry>& entries) {
    SSTableBuilder builder(entries.size());
    TableMeta meta;meta.fileNumber=number;meta.level=level;
    meta.smallest=entries.front().key;meta.largest=entries.back().key;
    meta.minSequence=UINT64_MAX;
    for(const auto& entry:entries) {
        builder.Add(entry);
        meta.minSequence=std::min(meta.minSequence,entry.key.sequence);
        meta.maxSequence=std::max(meta.maxSequence,entry.key.sequence);
    }
    builder.Finish();
    char name[32];std::snprintf(name,sizeof(name),"/%06llu.sst",(unsigned long long)number);
    meta.fileSize=SSTableWriter::Write(f.dir+name,builder);
    return meta;
}
void StoreManifest(const Fixture& f,const std::vector<TableMeta>& metas) {
    Manifest manifest;uint64_t sequence=0;
    for(const auto& meta:metas){manifest.AddTable(meta);sequence=std::max(sequence,meta.maxSequence);}
    manifest.SetLastSequence(sequence);manifest.SetFlushedSequence(sequence);
    manifest.SetGeneration(1);manifest.Save(f.dir+"/MANIFEST");
}
void TestLevelRecoveryAndLookup() {
    Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
    auto low=AddTable(f,1,2,{E(1,"apple","old"),E(2,"kiwi","lower"),E(3,"zebra","old")});
    auto left=AddTable(f,2,1,{E(4,"apple","middle"),E(5,"grape","green")});
    auto right=AddTable(f,3,1,{E(6,"lemon","yellow"),E(7,"pear","sweet")});
    auto old=AddTable(f,4,0,{{{"apple",8,ValueType::kDeletion},""},E(9,"zebra","fresh")});
    auto latest=AddTable(f,5,0,{E(10,"banana","new"),E(11,"mango","new")});
    // 高层元数据故意逆序、交错；L0 相对顺序仍是 old、latest。
    StoreManifest(f,{right,old,low,left,latest});
    {
        DB db(f.dir,options);
        CHECK(!db.Get("apple"));Value(db,"banana","new");Value(db,"zebra","fresh");
        Value(db,"grape","green");Value(db,"lemon","yellow");Value(db,"pear","sweet");
        Value(db,"kiwi","lower"); // L1 表间空隙，继续查 L2。
        CHECK(!db.Get("aardvark"));CHECK(!db.Get("zzzz"));CHECK(!db.Get("orange"));
        db.CompactLevel(0);db.CompactLevel(1);
        CHECK(!db.Get("apple"));Value(db,"kiwi","lower");Value(db,"zebra","fresh");
    }
    {DB db(f.dir,options);CHECK(!db.Get("apple"));Value(db,"zebra","fresh");Value(db,"kiwi","lower");}
}
void TestLevelInputBudgetAndOverlap() {
    Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;options.maxCompactionSourceBytes=1;
    auto beforeLeft=AddTable(f,1,1,{E(1,"a","keep")});
    auto overlap=AddTable(f,2,1,{E(2,"d","old"),E(3,"p","old")});
    auto afterRight=AddTable(f,3,1,{E(4,"z","keep")});
    // d、p 均为相等边界，必须包括目标表；下一张 L0 超预算，仍留在 L0。
    auto first=AddTable(f,4,0,{E(5,"d","middle"),E(6,"p","middle")});
    auto next=AddTable(f,5,0,{E(7,"d","latest")});
    StoreManifest(f,{beforeLeft,overlap,afterRight,first,next});
    {
        DB db(f.dir,options);db.CompactLevel(0);
        auto after=Manifest::Load(f.dir+"/MANIFEST");std::set<uint64_t> ids;
        size_t l0=0;for(const auto& meta:after.Tables()){ids.insert(meta.fileNumber);l0+=meta.level==0;}
        CHECK_EQ(l0,1u);CHECK(ids.count(5));CHECK(ids.count(1));CHECK(ids.count(3));
        CHECK(!ids.count(4));CHECK(!ids.count(2));
        CHECK_EQ(after.FlushedSequence(),7u);
        Value(db,"d","latest");Value(db,"p","middle");Value(db,"a","keep");Value(db,"z","keep");
        // 第二轮包含单张源表，且将其与第一轮 L1 输出再次归并。
        db.CompactLevel(0);Value(db,"d","latest");
        after=Manifest::Load(f.dir+"/MANIFEST");
        for(const auto& meta:after.Tables())CHECK_EQ(meta.level,1u);
    }
    {DB db(f.dir,options);Value(db,"d","latest");Value(db,"p","middle");}
}
void TestSourceBudgetBoundary() {
    for(bool exact:{false,true}) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
        auto a=AddTable(f,1,0,{E(1,"a","old")});
        auto b=AddTable(f,2,0,{E(2,"a","middle")});
        auto c=AddTable(f,3,0,{E(3,"a","latest")});
        options.maxCompactionSourceBytes=a.fileSize+b.fileSize-(exact?0:1);
        StoreManifest(f,{a,b,c});
        {
            DB db(f.dir,options);db.CompactLevel(0);
            const auto after=Manifest::Load(f.dir+"/MANIFEST");
            size_t l0=0;std::set<uint64_t> ids;
            for(const auto& meta:after.Tables()){l0+=meta.level==0;ids.insert(meta.fileNumber);}
            CHECK_EQ(l0,exact?1u:2u);CHECK(!ids.count(1));
            CHECK_EQ(ids.count(2),exact?0u:1u);CHECK(ids.count(3));
            Value(db,"a","latest");
        }
        {DB db(f.dir,options);Value(db,"a","latest");}
    }
}
void TestInvalidLevelsAndRecoveryLayout() {
    {
        Fixture f;DB db(f.dir);
        Throws<std::invalid_argument>([&]{db.CompactLevel(6);});
        Throws<std::invalid_argument>([&]{db.CompactLevel(UINT32_MAX);});
        db.Put("a","healthy");db.Flush();Value(db,"a","healthy");
    }
    {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;options.maxLevels=1;
        DB db(f.dir,options);Throws<std::invalid_argument>([&]{db.CompactLevel(0);});
        db.Put("a","value");db.Flush();Value(db,"a","value");
    }
    for(int mode=0;mode<3;++mode) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;options.enableWal=false;
        if(mode==2) {
            StoreManifest(f,{AddTable(f,1,7,{E(1,"a")})});
        } else {
            auto first=AddTable(f,1,1,{E(1,"a"),E(2,"m")});
            auto second=AddTable(f,2,1,{E(3,mode==0?"m":"b"),E(4,"z")});
            StoreManifest(f,{second,first});
        }
        Throws<std::runtime_error>([&]{DB db(f.dir,options);});
    }
    for(int mode=0;mode<2;++mode) {
        Fixture f;DBOptions options;options.enableAutoCompaction=false;
        if(mode==0)options.maxLevels=0;else options.maxCompactionSourceBytes=0;
        Throws<std::invalid_argument>([&]{DB db(f.dir,options);});
    }
}


// 扩展预算按配置计算；通过正式 MANIFEST 检查真正选中的文件。
void TestSourceExpansionLimits() {
    for(int mode=0;mode<6;++mode) {
        Fixture f;DBOptions options;options.enableWal=false;options.enableAutoCompaction=false;
        auto target=AddTable(f,1,1,{E(1,"a","base"),E(2,"z","base")});
        auto first=AddTable(f,2,0,{E(3,"m","old")});
        auto second=AddTable(f,3,0,{E(4,"m","new")});
        auto third=AddTable(f,4,0,{E(5,"m","end")});
        CHECK_EQ(first.fileSize,second.fileSize);
        CHECK_EQ(second.fileSize,third.fileSize);
        options.maxCompactionSourceBytes=first.fileSize;
        options.maxCompactionSourceExpansionFactor=2;
        // 0 禁止扩展；1 两张恰好；2 第二张差一字节；3 首张超过扩展上限；
        // 4 扩展乘法饱和；5 允许三张，验证更新后的累计字节。
        if(mode==0)options.maxCompactionSourceExpansionFactor=1;
        if(mode==2) {
            // 目标上限 = 2*first.fileSize，令第二张增长一个字节。
            std::filesystem::remove(f.dir+"/000003.sst");
            second=AddTable(f,3,0,{E(4,"m","new!")});
            CHECK_EQ(second.fileSize,first.fileSize+1);
        }
        if(mode==3)options.maxCompactionSourceBytes=1;
        if(mode==4) {
            options.maxCompactionSourceBytes=std::numeric_limits<size_t>::max()/2+1;
            options.maxCompactionSourceExpansionFactor=UINT32_MAX;
        }
        if(mode==5)options.maxCompactionSourceExpansionFactor=3;
        StoreManifest(f,{target,first,second,third});
        {
            DB db(f.dir,options);db.CompactLevel(0);
            const auto after=Manifest::Load(f.dir+"/MANIFEST");
            std::set<uint64_t> retained;size_t l0=0;
            for(const auto& meta:after.Tables()){retained.insert(meta.fileNumber);l0+=meta.level==0;}
            const size_t selected=(mode==4||mode==5)?3:(mode==1?2:1);
            CHECK_EQ(l0,3-selected);CHECK(!retained.count(1));CHECK(!retained.count(2));
            CHECK_EQ(retained.count(3),selected<2?1u:0u);
            CHECK_EQ(retained.count(4),selected<3?1u:0u);
            Value(db,"m","end");Value(db,"a","base");Value(db,"z","base");
        }
        {DB reopened(f.dir,options);Value(reopened,"m","end");}
    }
    Fixture invalid;DBOptions options;options.maxCompactionSourceExpansionFactor=0;
    Throws<std::invalid_argument>([&]{DB db(invalid.dir,options);});
}
void TestSourceExpansionStopsAtNewTarget() {
    Fixture f;DBOptions options;options.enableWal=false;options.enableAutoCompaction=false;
    auto left=AddTable(f,1,1,{E(1,"b","base"),E(2,"f","base")});
    auto right=AddTable(f,2,1,{E(3,"h","base"),E(4,"z","base")});
    auto first=AddTable(f,3,0,{E(5,"c","first")});
    auto crossing=AddTable(f,4,0,{E(6,"e","cross"),E(7,"h","cross")});
    auto inside=AddTable(f,5,0,{E(8,"d","inside")});
    options.maxCompactionSourceBytes=first.fileSize;
    options.maxCompactionSourceExpansionFactor=100;
    StoreManifest(f,{left,right,first,crossing,inside});
    DB db(f.dir,options);db.CompactLevel(0);
    const auto after=Manifest::Load(f.dir+"/MANIFEST");std::set<uint64_t> ids;
    for(const auto& meta:after.Tables())ids.insert(meta.fileNumber);
    CHECK(!ids.count(1));CHECK(ids.count(2));CHECK(!ids.count(3));
    CHECK(ids.count(4));CHECK(ids.count(5)); // 不得越过 crossing 吸收 inside。
    Value(db,"h","cross");Value(db,"d","inside");
}
void TestSourceExpansionEmptyTargetAndHigherLevel() {
    for(bool higher:{false,true}) {
        Fixture f;DBOptions options;options.enableWal=false;options.enableAutoCompaction=false;
        const uint32_t source=higher?1:0;
        auto first=AddTable(f,2,source,{E(3,"c","first")});
        auto second=AddTable(f,3,source,{E(4,"e","second")});
        options.maxCompactionSourceBytes=first.fileSize;
        options.maxCompactionSourceExpansionFactor=100;
        std::vector<TableMeta> metas{first,second};
        if(higher)metas.insert(metas.begin(),AddTable(f,1,2,{E(1,"a","base"),E(2,"z","base")}));
        StoreManifest(f,metas);
        {
            DB db(f.dir,options);db.CompactLevel(source);
            const auto after=Manifest::Load(f.dir+"/MANIFEST");size_t remaining=0;
            for(const auto& meta:after.Tables())remaining+=meta.level==source;
            CHECK_EQ(remaining,higher?0u:1u);
            Value(db,"c","first");Value(db,"e","second");
        }
        {DB reopened(f.dir,options);Value(reopened,"e","second");}
    }
}
// 范围伸入空隙，但未触及新目标文件：仍应允许扩展。
void TestSourceExpansionIntoGap() {
    Fixture f;DBOptions options;options.enableWal=false;options.enableAutoCompaction=false;
    auto target=AddTable(f,1,1,{E(1,"b","base"),E(2,"f","base")});
    auto untouched=AddTable(f,2,1,{E(3,"m","base"),E(4,"z","base")});
    auto first=AddTable(f,3,0,{E(5,"d","first")});
    auto gap=AddTable(f,4,0,{E(6,"h","gap")});
    options.maxCompactionSourceBytes=first.fileSize;
    options.maxCompactionSourceExpansionFactor=10;
    StoreManifest(f,{target,untouched,first,gap});
    {
        DB db(f.dir,options);db.CompactLevel(0);
        const auto after=Manifest::Load(f.dir+"/MANIFEST");std::set<uint64_t> ids;
        for(const auto& meta:after.Tables())ids.insert(meta.fileNumber);
        CHECK(!ids.count(1));CHECK(ids.count(2));CHECK(!ids.count(3));
        CHECK(!ids.count(4)); // 合同：不增加目标集合的 gap 应被吸收。
        Value(db,"d","first");Value(db,"h","gap");Value(db,"m","base");
    }
    {DB reopened(f.dir,options);Value(reopened,"h","gap");}
}

}
int main(){
    RUN_TEST(TestSourceExpansionLimits);
    RUN_TEST(TestSourceExpansionStopsAtNewTarget);
    RUN_TEST(TestSourceExpansionEmptyTargetAndHigherLevel);
    RUN_TEST(TestSourceExpansionIntoGap);
    RUN_TEST(TestLevelRecoveryAndLookup);
    RUN_TEST(TestLevelInputBudgetAndOverlap);
    RUN_TEST(TestInvalidLevelsAndRecoveryLayout);
    RUN_TEST(TestSourceBudgetBoundary);
    RUN_TEST(TestCompactNoOp);
    RUN_TEST(TestCompactSplitAndReopen);
    RUN_TEST(TestCompactConcurrentRequestsAndFlush);
    RUN_TEST(TestCompactFailureRecovery);
    RUN_TEST(TestBasicFlushAndReopen);
    RUN_TEST(TestMultiSegmentRecovery);
    RUN_TEST(TestTruncatedSegments);
    RUN_TEST(TestCorruptionAndOrdering);
    RUN_TEST(TestWalOverlapAndOrphans);
    RUN_TEST(TestWalModesAndSmallThreshold);
    RUN_TEST(TestForegroundAndBackpressure);
    RUN_TEST(TestExplicitFlushWaits);
    RUN_TEST(TestBackgroundFailureWakesWaiters);
    RUN_TEST(TestCleanupFailureIsNotFlushFailure);
    RUN_TEST(TestShutdownDuringBuild);
    RUN_TEST(TestConcurrentCallers);
    RUN_TEST(TestShortIoAndInvalidInput);
    TEST_SUMMARY_RETURN();
}
