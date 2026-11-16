#include "lsm/db.h"
#include "lsm/wal.h"
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
void TestDisabledWalAndZeroThreshold() {
    for(bool enable:{false,true}) {
        Fixture f;
        {
            DB db(f.dir,DBOptions{enable,0});
            db.Put("apple","red"); db.Delete("apple"); db.Put("banana","");
            db.Flush(); CHECK(!db.Get("apple")); Value(db,"banana","");
        }
        {DB db(f.dir,DBOptions{enable,0}); CHECK(!db.Get("apple")); Value(db,"banana","");}
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
}
int main(){
    RUN_TEST(TestBasicFlushAndReopen);
    RUN_TEST(TestMultiSegmentRecovery);
    RUN_TEST(TestTruncatedSegments);
    RUN_TEST(TestCorruptionAndOrdering);
    RUN_TEST(TestWalOverlapAndOrphans);
    RUN_TEST(TestDisabledWalAndZeroThreshold);
    RUN_TEST(TestForegroundAndBackpressure);
    RUN_TEST(TestExplicitFlushWaits);
    RUN_TEST(TestBackgroundFailureWakesWaiters);
    RUN_TEST(TestCleanupFailureIsNotFlushFailure);
    RUN_TEST(TestShutdownDuringBuild);
    RUN_TEST(TestConcurrentCallers);
    RUN_TEST(TestShortIoAndInvalidInput);
    TEST_SUMMARY_RETURN();
}
