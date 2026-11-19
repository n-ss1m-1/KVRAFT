#include "lsm/block_cache.h"
#include "lsm/sstable.h"
#include "lsm/db.h"
#include "test_util.h"
#include <atomic>
#include <cerrno>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace {
std::atomic<int> reads{0};
std::atomic<bool> failReads{false};
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t offset) {
    ++reads;
    if (failReads) { errno=EIO; return -1; }
    return __real_pread(fd,buf,n,offset);
}
namespace {
struct Fixture {
    std::string dir=testutil::MakeTempDir("block_cache");
    ~Fixture(){failReads=false;testutil::CleanupDir(dir);}
};
lsm::Entry Entry(uint64_t seq, std::string value,
                 ValueType type=ValueType::kValue) {
    lsm::Entry e; e.key.user_key="apple";e.key.sequence=seq;
    e.key.type=type;e.value=std::move(value);return e;
}
std::shared_ptr<const DataBlockReader> Reader(std::string value="red") {
    return std::make_shared<const DataBlockReader>(lsm::EncodeEntry(Entry(10,std::move(value))));
}
template<class E,class F> void Throws(F f) {
    bool caught=false;
    try{f();}catch(const E&){caught=true;}catch(...){}
    CHECK(caught);
}
void TestLruAndDuplicate() {
    BlockCache c(10);auto a=Reader(),b=Reader("blue"),d=Reader("green");
    CHECK(!c.Get({1,0}));CHECK_EQ(c.CapacityBytes(),10u);
    c.Insert({1,0},a,4);c.Insert({2,0},b,6);
    CHECK_EQ(c.Usage(),10u);CHECK(c.Get({1,0})==a);
    c.Insert({3,0},d,6); // 命中 A 后，淘汰更旧的 B。
    CHECK(!c.Get({2,0}));CHECK(c.Get({1,0})==a);CHECK_EQ(c.Usage(),10u);
    c.Insert({3,0},b,1); // 重复 key 保留旧对象与计费，并提升热度。
    CHECK(c.Get({3,0})==d);CHECK_EQ(c.Usage(),10u);
    c.Insert({4,0},b,4);CHECK(!c.Get({1,0}));CHECK_EQ(c.Usage(),10u);
    CHECK(c.Get({4,0})==b);CHECK(c.Get({3,0})==d);
}
void TestCapacityAndLifetime() {
    auto a=Reader();BlockCache disabled(0);
    disabled.Insert({1,0},a,0);CHECK(!disabled.Get({1,0}));CHECK_EQ(disabled.Usage(),0u);
    BlockCache c(8);c.Insert({1,0},a,4);
    c.Insert({2,0},a,std::numeric_limits<size_t>::max());
    CHECK(c.Get({1,0})==a);CHECK_EQ(c.Usage(),4u);
    Throws<std::invalid_argument>([&]{c.Insert({2,0},nullptr,1);});
    CHECK_EQ(c.Usage(),4u);
    c.Insert({2,0},a,4);c.Insert({3,0},a,8);
    CHECK(!c.Get({1,0}));CHECK(!c.Get({2,0}));CHECK_EQ(c.Usage(),8u);
    auto b=Reader("held");std::weak_ptr<const DataBlockReader> weak=b;
    BlockCache life(1);life.Insert({1,0},b,1);auto held=life.Get({1,0});b.reset();
    life.Insert({2,0},a,1);CHECK(!weak.expired());
    CHECK_STR_EQ(held->Get(10,"apple").value,"held");
    held.reset();CHECK(weak.expired());
    BlockCache huge(std::numeric_limits<size_t>::max());
    huge.Insert({1,0},a,std::numeric_limits<size_t>::max());
    huge.Insert({2,0},a,1);CHECK(!huge.Get({1,0}));CHECK_EQ(huge.Usage(),1u);
}
void TestConcurrent() {
    BlockCache c(8);auto a=Reader();std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for(int t=0;t<4;++t)threads.emplace_back([&,t]{
        try{for(int i=0;i<2000;++i){
            BlockCacheKey k{static_cast<uint64_t>((i+t)%16),static_cast<uint64_t>(i%3)};
            c.Insert(k,a,1);auto p=c.Get(k);
            if(p && p->Get(10,"apple").value!="red")ok=false;
            if(c.Usage()>8)ok=false;
        }}catch(...){ok=false;}
    });
    for(auto& t:threads)t.join();CHECK(ok.load());CHECK(c.Usage()<=8);
}
void WriteTable(const std::string& path,std::string value) {
    SSTableBuilder builder(1);builder.Add(Entry(10,std::move(value)));builder.Finish();
    SSTableWriter::Write(path,builder);
}
void TestReaderHitAndIdentity() {
    Fixture f;auto a=f.dir+"/a.sst",b=f.dir+"/b.sst";
    WriteTable(a,"red");WriteTable(b,"blue");
    auto cache=std::make_shared<BlockCache>(4096);
    SSTableReader first(a,1,cache),second(b,2,cache),plain(a);
    reads=0;CHECK_STR_EQ(first.Get(10,"apple").value,"red");CHECK(reads>0);
    auto charged=cache->Usage();CHECK(charged>0);
    reads=0;failReads=true;
    CHECK_STR_EQ(first.Get(10,"apple").value,"red");CHECK_EQ(reads.load(),0);
    CHECK(first.Get(5,"apple").state==LookupState::kNotFound);
    CHECK_EQ(reads.load(),0);CHECK_EQ(cache->Usage(),charged);
    Throws<std::system_error>([&]{second.Get(10,"apple");});
    Throws<std::system_error>([&]{plain.Get(10,"apple");});
    failReads=false;CHECK_STR_EQ(second.Get(10,"apple").value,"blue");
    for(size_t capacity:{size_t{0},size_t{1}}){
        auto small=std::make_shared<BlockCache>(capacity);
        SSTableReader r(a,1,small);CHECK_STR_EQ(r.Get(10,"apple").value,"red");
        CHECK_EQ(small->Usage(),0u);failReads=true;
        Throws<std::system_error>([&]{r.Get(10,"apple");});failReads=false;
    }
}
void TestCrossBlockVersions() {
    Fixture f;SSTableBuilder b(3,1);
    b.Add(Entry(30,"",ValueType::kDeletion));b.Add(Entry(20,"green"));b.Add(Entry(10,"red"));b.Finish();
    auto path=f.dir+"/versions.sst";SSTableWriter::Write(path,b);
    auto cache=std::make_shared<BlockCache>(4096);SSTableReader r(path,7,cache);
    for(int run=0;run<2;++run){
        CHECK(r.Get(35,"apple").state==LookupState::kDeleted);
        CHECK_STR_EQ(r.Get(25,"apple").value,"green");
        CHECK_STR_EQ(r.Get(15,"apple").value,"red");
        CHECK(r.Get(5,"apple").state==LookupState::kNotFound);
        CHECK(r.Get(35,"banana").state==LookupState::kNotFound);
        if(run==0){reads=0;failReads=true;}
    }
    CHECK_EQ(reads.load(),0);failReads=false;
}
void TestCorruptionNotCached() {
    Fixture f;auto path=f.dir+"/bad.sst";WriteTable(path,"red");
    auto cache=std::make_shared<BlockCache>(4096);SSTableReader r(path,1,cache);
    {std::fstream out(path,std::ios::binary|std::ios::in|std::ios::out);char byte=0;out.read(&byte,1);byte^=1;out.seekp(0);out.write(&byte,1);}
    Throws<std::runtime_error>([&]{r.Get(10,"apple");});CHECK_EQ(cache->Usage(),0u);
    CHECK(!cache->Get({1,0}));
}
void TestDbVersionsAndReopen() {
    for(size_t capacity:{size_t{0},size_t{1},size_t{4096}}){
        Fixture f;DBOptions options;options.BlockCacheCapacity=capacity;options.enableWal=false;
        {
            DB db(f.dir,options);db.Put("apple","red");db.Flush();
            CHECK_STR_EQ(db.Get("apple").value_or("missing"),"red"); // 预热旧表
            db.Put("apple","green");CHECK_STR_EQ(db.Get("apple").value_or("missing"),"green");
            db.Flush();CHECK_STR_EQ(db.Get("apple").value_or("missing"),"green");
            db.Delete("apple");CHECK(!db.Get("apple"));db.Flush();CHECK(!db.Get("apple"));
            db.Put("banana","yellow");db.Flush();CHECK(!db.Get("apple"));
        }
        {DB db(f.dir,options);CHECK(!db.Get("apple"));CHECK_STR_EQ(db.Get("banana").value_or("missing"),"yellow");}
    }
}
}
int main(){
    RUN_TEST(TestLruAndDuplicate);RUN_TEST(TestCapacityAndLifetime);RUN_TEST(TestConcurrent);
    RUN_TEST(TestReaderHitAndIdentity);RUN_TEST(TestCrossBlockVersions);
    RUN_TEST(TestCorruptionNotCached);RUN_TEST(TestDbVersionsAndReopen);
    TEST_SUMMARY_RETURN();
}
