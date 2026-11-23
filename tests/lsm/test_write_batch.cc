#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "lsm/wal.h"
#include "test_util.h"
#include "wal_test_util.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
std::atomic<int> syncs{0}, writes{0}, reads{0};
std::atomic<bool> failSync{false}, partialWrite{false}, shortIo{false};
std::string FdPath(int fd) {
    char name[64], path[4096];
    std::snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    auto n = readlink(name, path, sizeof(path));
    return n < 0 ? "" : std::string(path, static_cast<size_t>(n));
}
bool IsWal(int fd) { return std::filesystem::path(FdPath(fd)).extension() == ".wal"; }
void Reset() { syncs=0; writes=0; reads=0; failSync=false; partialWrite=false; shortIo=false; }
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    if(IsWal(fd)) {
        const int call=++writes;
        if(partialWrite) {
            if(call==1) return __real_write(fd,data,std::min(size,size_t{10}));
            errno=ENOSPC; return -1;
        }
        if(shortIo) {
            if(call==1) {errno=EINTR; return -1;}
            size=std::min(size,size_t{3});
        }
    }
    return __real_write(fd,data,size);
}
extern "C" ssize_t __wrap_read(int fd, void* data, size_t size) {
    if(IsWal(fd) && shortIo) {
        if(++reads==1) {errno=EINTR; return -1;}
        size=std::min(size,size_t{3});
    }
    return __real_read(fd,data,size);
}
extern "C" int __wrap_fsync(int fd) {
    if(IsWal(fd)) {
        ++syncs;
        if(failSync) {errno=EIO; return -1;}
    }
    return __real_fsync(fd);
}
namespace {
struct Fixture {
    std::string dir=testutil::MakeTempDir("write_batch");
    std::string Wal() const {return dir+"/000001.wal";}
    Fixture() {Reset();}
    ~Fixture() {Reset(); testutil::CleanupDir(dir);}
};
template<class Error, class F> void Throws(F action) {
    bool caught=false;
    try {action();} catch(const Error&) {caught=true;}
    CHECK(caught);
}
lsm::Entry E(uint64_t seq, std::string key="apple", std::string value="red") {
    return {{std::move(key),seq,ValueType::kValue},std::move(value)};
}
void Save(const std::string& path, const std::string& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());
    if(!out) throw std::runtime_error("测试夹具写入失败");
}
std::string Load(const std::string& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),{}};
}
// 独立组装格式，避免只用生产 Writer 验证生产 Reader。
std::string FramePayload(const std::string& payload, bool batch=true) {
    std::string bytes;
    lsm::AppendFixed32(bytes,static_cast<uint32_t>(payload.size()) | (batch ? 0x80000000u : 0u));
    lsm::AppendFixed32(bytes,lsm::Crc32c(bytes+payload));
    return bytes+payload;
}
std::string BatchFrame(const std::vector<lsm::Entry>& entries) {
    std::string payload;
    lsm::AppendFixed32(payload,static_cast<uint32_t>(entries.size()));
    for(const auto& e:entries) payload+=lsm::EncodeEntry(e);
    return FramePayload(payload);
}
void Value(DB& db, const std::string& key, const std::string& expected) {
    auto value=db.Get(key); REQUIRE(value.has_value()); CHECK_STR_EQ(*value,expected);
}
void TestPrefixAndOwnership() {
    auto a=E(7,std::string("a\0b",3),std::string("v\0x",3));
    auto b=E(8); b.key.type=ValueType::kDeletion; b.value.clear();
    auto first=lsm::EncodeEntry(a), second=lsm::EncodeEntry(b);
    lsm::Entry output=E(99); size_t used=999;
    CHECK(lsm::DecodeEntryPrefix(first+second,output,used));
    CHECK_EQ(used,first.size()); CHECK_STR_EQ(output.value,a.value);
    CHECK(!lsm::DecodeEntry(first+second,output));
    CHECK(lsm::DecodeEntryPrefix(second,output,used)); CHECK_EQ(used,second.size());
    for(size_t n=0;n<first.size();++n) {
        output=E(99);used=999;
        CHECK(!lsm::DecodeEntryPrefix(first.substr(0,n),output,used));
        CHECK_EQ(output.key.sequence,99u);CHECK_EQ(used,999u);
    }
    Fixture f; DB db(f.dir); WriteBatch batch;
    std::string key="owned", value="original";
    batch.Put(key,value);key="changed";value="changed";
    batch.Put("apple","red");batch.Delete("apple");batch.Put("apple","green");
    CHECK_EQ(batch.Count(),4u);db.Write(batch);CHECK_EQ(batch.Count(),4u);
    Value(db,"owned","original");Value(db,"apple","green");
    batch.Clear();CHECK(batch.Empty());CHECK_EQ(batch.Count(),0u);
}
void TestWalMixedAndTruncation() {
    Fixture f;
    const auto old=waltest::Frame(E(1));
    const auto batch=BatchFrame({E(2,"b"),E(3,"c")});
    const auto tail=BatchFrame({E(4,"d"),E(5,"e")});
    {
        WalWriter writer(f.Wal());
        CHECK(!writer.Append(E(1)));
        CHECK(!writer.Append(std::vector<lsm::Entry>{E(2,"b"),E(3,"c")}));
    }
    CHECK_STR_EQ(Load(f.Wal()),old+batch);
    for(size_t n=0;n<tail.size();++n) {
        Save(f.Wal(),old+batch+tail.substr(0,n));
        WalReader reader(f.Wal());std::vector<lsm::Entry> out;
        REQUIRE(reader.ReadNext(out)==WalReadStatus::kRecord);CHECK_EQ(out.size(),1u);
        REQUIRE(reader.ReadNext(out)==WalReadStatus::kRecord);REQUIRE(out.size()==2);
        CHECK_EQ(out[0].key.sequence,2u);CHECK_EQ(out[1].key.sequence,3u);
        CHECK_EQ(reader.ReadNext(out),n==0 ? WalReadStatus::kEndOfFile : WalReadStatus::kTruncatedTail);
        CHECK_EQ(out.size(),2u);CHECK_EQ(out[0].key.sequence,2u);
        CHECK_EQ(reader.LastValidOffset(),old.size()+batch.size());
    }
}
void TestMalformedBatch() {
    Fixture f;const auto valid=BatchFrame({E(2),E(3)});
    std::vector<std::string> cases;
    auto bad=valid;bad[4]^=1;cases.push_back(bad);
    bad=valid;bad[3]^=static_cast<char>(0x80);cases.push_back(bad); // 类型位受 CRC 保护
    bad=valid;bad.back()^=1;cases.push_back(bad);
    auto payload=valid.substr(8);payload[0]=0;cases.push_back(FramePayload(payload));
    payload=valid.substr(8);payload[0]=1;cases.push_back(FramePayload(payload));
    payload=valid.substr(8);payload[0]=3;cases.push_back(FramePayload(payload));
    payload=valid.substr(8);payload.append("x");cases.push_back(FramePayload(payload));
    cases.push_back(BatchFrame({E(2),E(4)}));
    cases.push_back(BatchFrame({E(2),E(2)}));
    cases.push_back(BatchFrame({E(UINT64_MAX),E(1)}));
    payload=valid.substr(8);payload[4+8]=static_cast<char>(255);cases.push_back(FramePayload(payload));
    for(auto length:{0x80000000u,0x80000014u,0x81000001u}) {
        std::string header;lsm::AppendFixed32(header,length);lsm::AppendFixed32(header,0);cases.push_back(header);
    }
    for(const auto& bytes:cases) {
        const auto prefix=waltest::Frame(E(1));Save(f.Wal(),prefix+bytes);
        WalReader reader(f.Wal());std::vector<lsm::Entry> out;
        REQUIRE(reader.ReadNext(out)==WalReadStatus::kRecord);
        CHECK_EQ(reader.ReadNext(out),WalReadStatus::kCorruption);
        CHECK_EQ(out.size(),1u);CHECK_EQ(out[0].key.sequence,1u);
        CHECK_EQ(reader.LastValidOffset(),prefix.size());
    }
}
void TestBatchLimitsAndWriterFailure() {
    Fixture f;WalWriter writer(f.Wal());
    auto reject=[&](const std::vector<lsm::Entry>& entries) {
        auto before=Load(f.Wal());Throws<std::invalid_argument>([&]{writer.Append(entries);});
        CHECK_STR_EQ(Load(f.Wal()),before);
    };
    reject({});reject({E(0)});reject({E(2),E(4)});reject({E(UINT64_MAX),E(1)});
    auto bad=E(1);bad.key.type=ValueType::kDeletion;reject({bad});
    // 单条各自合法，但整批超过 16 MiB。
    auto large=E(1,"",std::string(8*1024*1024,'x')), other=large;other.key.sequence=2;
    reject({large,other});
    auto edge=E(UINT64_MAX,"",std::string(16*1024*1024-4-17,'x'));
    CHECK_EQ(lsm::EncodeBatchPayloadSize({edge}),16u*1024*1024);
    CHECK(!writer.Append(std::vector<lsm::Entry>{edge}));
    {WalReader reader(f.Wal());std::vector<lsm::Entry> out;REQUIRE(reader.ReadNext(out)==WalReadStatus::kRecord);CHECK_EQ(out[0].key.sequence,UINT64_MAX);}
    edge.value.push_back('x');reject({edge});
}
void TestShortIo() {
    Fixture f;shortIo=true;
    {WalWriter writer(f.Wal());CHECK(!writer.Append(std::vector<lsm::Entry>{E(1),E(2)}));CHECK(!writer.Sync());}
    {WalReader reader(f.Wal());std::vector<lsm::Entry> out;REQUIRE(reader.ReadNext(out)==WalReadStatus::kRecord);CHECK_EQ(out.size(),2u);}
    CHECK(writes>1);CHECK(reads>1);
}
void TestDbWriteFlushAndReuse() {
    for(bool wal:{false,true}) {
        Fixture f;WriteBatch batch;
        batch.Put("apple","red");batch.Delete("apple");batch.Put("pear","yellow");
        {
            DB db(f.dir,DBOptions{wal,1});Reset();db.Write(batch);
            CHECK_EQ(syncs.load(),wal?1:0);CHECK(!db.Get("apple"));Value(db,"pear","yellow");
            CHECK(!std::filesystem::exists(f.dir+"/MANIFEST")); // 批内不切表
            db.Write(batch);db.Put("single","v");db.Delete("single");db.Flush();
            CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),8u);
        }
        {DB db(f.dir,DBOptions{false,1});CHECK(!db.Get("apple"));Value(db,"pear","yellow");CHECK(!db.Get("single"));}
    }
}
void TestDbRejectedBatchAndSequenceLimit() {
    for(bool wal:{false,true}) {
        Fixture f;DB db(f.dir,DBOptions{wal});WriteBatch empty;
        Throws<std::invalid_argument>([&]{db.Write(empty);});
        WriteBatch large;std::string value(8*1024*1024,'x');large.Put("a",value);large.Put("b",value);
        Throws<std::invalid_argument>([&]{db.Write(large);});CHECK(!db.Get("a"));CHECK(!db.Get("b"));
        db.Put("ok","v");db.Flush();CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),1u);
    }
    Fixture f;Save(f.Wal(),waltest::Frame(E(UINT64_MAX-1)));
    DB db(f.dir);WriteBatch two;two.Put("a","v");two.Put("b","v");
    Throws<std::overflow_error>([&]{db.Write(two);});CHECK(!db.Get("a"));
    db.Put("last","v");Value(db,"last","v");
    Throws<std::overflow_error>([&]{db.Put("overflow","v");});
    CHECK_EQ(lsm::EncodeBatchPayloadSize({E(UINT64_MAX)}),4u+lsm::EncodedEntrySize(E(UINT64_MAX)));
}
void TestDbRecoveryAndFailure() {
    const auto old=waltest::Frame(E(1,"old"));
    const auto frame=BatchFrame({E(2,"a"),E(3,"b")});
    for(size_t n=0;n<=frame.size();++n) {
        Fixture f;Save(f.Wal(),old+frame.substr(0,n));
        DB db(f.dir);Value(db,"old","red");
        CHECK_EQ(db.Get("a").has_value(),n==frame.size());CHECK_EQ(db.Get("b").has_value(),n==frame.size());
        CHECK_EQ(std::filesystem::file_size(f.Wal()),old.size()+(n==frame.size()?frame.size():0));
    }
    for(bool syncFailure:{false,true}) {
        Fixture f;
        {
            DB db(f.dir);db.Put("old","v");Reset();failSync=syncFailure;partialWrite=!syncFailure;
            WriteBatch batch;batch.Put("a","red");batch.Put("b","red");
            Throws<std::system_error>([&]{db.Write(batch);});
            Reset();Throws<std::runtime_error>([&]{db.Get("old");});
            Throws<std::runtime_error>([&]{db.Flush();});Throws<std::runtime_error>([&]{db.Put("later","v");});
        }
        // Sync 失败不等于记录不存在：注入错误前已完整写到文件，重开可恢复整批。
        {DB db(f.dir);Value(db,"old","v");CHECK_EQ(db.Get("a").has_value(),syncFailure);CHECK_EQ(db.Get("b").has_value(),syncFailure);CHECK(!db.Get("later"));}
    }
    Fixture f;Save(f.Wal(),frame.substr(0,frame.size()-1));Save(f.dir+"/000002.wal","");
    Throws<std::runtime_error>([&]{DB db(f.dir);}); // 非最新段的半批不得截断
}
void TestFlushedBatchOverlap() {
    Fixture f;std::string stale;
    {
        DB db(f.dir);WriteBatch batch;
        batch.Put("apple","red");batch.Delete("apple");batch.Put("pear","yellow");
        db.Write(batch);stale=Load(f.Wal());db.Flush();
    }
    // 模拟发布成功但旧 WAL 清理未完成，整批记录应被跳过。
    Save(f.Wal(),stale);
    {
        DB db(f.dir);CHECK(!db.Get("apple"));Value(db,"pear","yellow");
        db.Put("new","v");db.Flush();
        CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),4u);
    }
    {DB db(f.dir,DBOptions{false});CHECK(!db.Get("apple"));Value(db,"new","v");}
}
void TestConcurrentBatches() {
    Fixture f;DB db(f.dir,DBOptions{true,1024});std::vector<std::future<void>> workers;
    for(int t=0;t<4;++t)workers.push_back(std::async(std::launch::async,[&,t]{
        for(int i=0;i<12;++i){WriteBatch batch;auto prefix=std::to_string(t)+":"+std::to_string(i);
            batch.Put(prefix,"old");batch.Delete(prefix);batch.Put(prefix,"new");db.Write(batch);}
    }));
    for(auto& worker:workers)worker.get();
    db.Flush();CHECK_EQ(Manifest::Load(f.dir+"/MANIFEST").FlushedSequence(),144u);
    for(int t=0;t<4;++t)for(int i=0;i<12;++i)Value(db,std::to_string(t)+":"+std::to_string(i),"new");
}
}
int main() {
    RUN_TEST(TestPrefixAndOwnership);
    RUN_TEST(TestWalMixedAndTruncation);
    RUN_TEST(TestMalformedBatch);
    RUN_TEST(TestBatchLimitsAndWriterFailure);
    RUN_TEST(TestShortIo);
    RUN_TEST(TestDbWriteFlushAndReuse);
    RUN_TEST(TestDbRejectedBatchAndSequenceLimit);
    RUN_TEST(TestDbRecoveryAndFailure);
    RUN_TEST(TestFlushedBatchOverlap);
    RUN_TEST(TestConcurrentBatches);
    TEST_SUMMARY_RETURN();
}
