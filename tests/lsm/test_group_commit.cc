#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "lsm/wal.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <future>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {
std::atomic<int> arrivals{0}, syncs{0}, appends{0};
std::atomic<bool> armed{false}, entered{false}, releaseSync{false};
std::atomic<int> fault{0};
thread_local bool trackFirstLock = false;
bool IsWal(int fd) {
    char name[64], path[4096];
    std::snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
    auto n=readlink(name,path,sizeof(path));
    return n>0 && std::filesystem::path(std::string(path,n)).extension()==".wal";
}
template<class F> bool Until(F predicate) {
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>end) return false;
        std::this_thread::yield();
    }
    return true;
}
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    int result=__real_pthread_mutex_lock(mutex);
    // Write入队时首次取得队列锁；组长恢复执行前，必须等待此锁释放。
    if(result==0 && trackFirstLock) {trackFirstLock=false; ++arrivals;}
    return result;
}
extern "C" int __real_fsync(int);
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __wrap_fsync(int fd) {
    if(IsWal(fd)) {
        ++syncs;
        if(armed.exchange(false)) {
            entered=true;
            if(!Until([]{return releaseSync.load();})) {errno=ETIMEDOUT;return -1;}
        }
        if(fault==1 && syncs>1) {errno=EIO;return -1;}
    }
    return __real_fsync(fd);
}
extern "C" ssize_t __wrap_write(int fd,const void* data,size_t size) {
    if(IsWal(fd)) {
        int call=++appends;
        // 首组成功，下一组第一条完整写入，第二条只写半截后失败。
        if(fault==2 && call==3) return __real_write(fd,data,std::min(size,size_t{10}));
        if(fault==2 && call>=4) {errno=ENOSPC;return -1;}
    }
    return __real_write(fd,data,size);
}
namespace {
struct Fixture {
    std::string dir=testutil::MakeTempDir("group_commit");
    Fixture(){arrivals=0;syncs=0;appends=0;armed=false;entered=false;releaseSync=false;fault=0;}
    ~Fixture(){fault=0;armed=false;releaseSync=true;testutil::CleanupDir(dir);}
};
// 返回0成功，1参数错误，2执行错误。断言只在主线程执行。
int Submit(DB& db,int id,bool invalid=false) {
    WriteBatch batch;
    if(!invalid) {batch.Put("k"+std::to_string(id),"old");batch.Delete("k"+std::to_string(id));batch.Put("k"+std::to_string(id),"new");}
    trackFirstLock=true;
    try {db.Write(batch);trackFirstLock=false;return 0;}
    catch(const std::invalid_argument&){trackFirstLock=false;return 1;}
    catch(...){trackFirstLock=false;return 2;}
}
int Result(std::future<int>& f) {
    if(f.wait_for(std::chrono::seconds(8))!=std::future_status::ready) {
        std::fprintf(stderr,"组提交线程未退出，终止测试以避免future析构永久等待\n");
        std::_Exit(2);
    }
    return f.get();
}
void RunQueued(int mode,bool invalid,size_t limit,size_t budget=1024*1024) {
    Fixture f;DBOptions options;options.memTableSizeLimit=1024*1024;options.maxGroupRequests=limit;options.maxGroupBytes=budget;
    {
        DB db(f.dir,options);syncs=0;appends=0;armed=true;
        auto leader=std::async(std::launch::async,[&]{return Submit(db,0);});
        bool blocked=Until([]{return entered.load();});CHECK(blocked);
        std::vector<std::future<int>> followers;
        for(int i=1;i<=6;++i) {
            followers.push_back(std::async(std::launch::async,[&,i]{return Submit(db,i,invalid && i==2);}));
            CHECK(Until([&,i]{return arrivals.load()>=i+1;}));
        }
        fault=mode;releaseSync=true;
        CHECK_EQ(Result(leader),0);
        for(int i=1;i<=6;++i) CHECK_EQ(Result(followers[i-1]),invalid && i==2 ? 1 : mode ? 2 : 0);
        if(mode==0) {
            // 每个三操作请求为 8+4+(17+2+3)+(17+2)+(17+2+3)=75 字节。
            const size_t perGroup=std::min(limit,std::max(size_t{1},budget/75));
            const int expected=1+(6+static_cast<int>(perGroup)-1)/static_cast<int>(perGroup);
            CHECK_EQ(syncs.load(),expected);
            for(int i=0;i<=6;++i) {
                auto value=db.Get("k"+std::to_string(i));
                if(invalid && i==2) CHECK(!value);
                else {REQUIRE(value);CHECK_STR_EQ(*value,"new");}
            }
            db.Put("after","ok"); // 非法请求不得阻止后续交接。
            db.Flush();
            CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),(invalid?18u:21u)+1);
        } else {
            bool getFailed=false;try{db.Get("k0");}catch(...){getFailed=true;}CHECK(getFailed);
            CHECK_EQ(Submit(db,99),2); // 故障后的新请求也必须结束等待。
        }
        fault=0;
    }
    {
        DB db(f.dir,DBOptions{mode!=0});
        CHECK_STR_EQ(db.Get("k0").value_or(""),"new");
        if(mode==1) for(int i=1;i<=6;++i) CHECK_STR_EQ(db.Get("k"+std::to_string(i)).value_or(""),"new");
        if(mode==2) {
            CHECK_STR_EQ(db.Get("k1").value_or(""),"new");
            for(int i=2;i<=6;++i) CHECK(!db.Get("k"+std::to_string(i)));
        }
        if(mode==0) CHECK_STR_EQ(db.Get("after").value_or(""),"ok");
    }
}
void InvalidOptions(){
    Fixture f;
    const auto path=f.dir+"/not_created";
    for(int field=0;field<3;++field){
        DBOptions options;
        if(field==0) options.memTableSizeLimit=0;
        if(field==1) options.maxGroupRequests=0;
        if(field==2) options.maxGroupBytes=0;
        bool rejected=false;
        try{DB db(path,options);}catch(const std::invalid_argument&){rejected=true;}
        CHECK(rejected);CHECK(!std::filesystem::exists(path));
    }
    DBOptions options;options.blockCacheCapacity=0;
    DB db(path,options);db.Put("ok","value");
    CHECK_STR_EQ(db.Get("ok").value_or(""),"value");
}
void SharedSync(){RunQueued(0,false,32);}
void InvalidIsolation(){RunQueued(0,true,32);}
void GroupLimit(){RunQueued(0,false,2);}
void ExactByteBudget(){RunQueued(0,false,32,150);}
void BelowByteBudget(){RunQueued(0,false,32,149);}
void OversizedFirstRequest(){RunQueued(0,false,32,74);}
void RequestAndByteLimits(){RunQueued(0,false,2,225);}
void EstimatedSizeMatchesWal(){
    Fixture f;WriteBatch batch;
    CHECK_EQ(batch.EstimatedWalRecordSize(),12u);
    batch.Put(std::string("a\0b",3),std::string("v\0",2));batch.Delete("gone");
    CHECK_EQ(batch.EstimatedWalRecordSize(),55u);
    {DB db(f.dir);db.Write(batch);}
    CHECK_EQ(std::filesystem::file_size(f.dir+"/000001.wal"),batch.EstimatedWalRecordSize());
    batch.Clear();CHECK_EQ(batch.EstimatedWalRecordSize(),12u);
}
void SyncFailure(){RunQueued(1,false,32);}
void PartialAppendFailure(){RunQueued(2,false,32);}
}
int main(){
    RUN_TEST(InvalidOptions);RUN_TEST(SharedSync);RUN_TEST(InvalidIsolation);RUN_TEST(GroupLimit);
    RUN_TEST(ExactByteBudget);RUN_TEST(BelowByteBudget);RUN_TEST(OversizedFirstRequest);
    RUN_TEST(RequestAndByteLimits);RUN_TEST(EstimatedSizeMatchesWal);
    RUN_TEST(SyncFailure);RUN_TEST(PartialAppendFailure);
    TEST_SUMMARY_RETURN();
}
