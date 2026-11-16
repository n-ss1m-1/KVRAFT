#include "lsm/manifest.h"
#include "test_util.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>
#include <system_error>

namespace {
enum class Mode { kNormal, kShort, kError, kZero };
Mode readMode = Mode::kNormal, writeMode = Mode::kNormal;
int reads = 0, writes = 0, syncs = 0, failSyncAt = 0;
bool failRename = false, inflatedSize = false;
void Reset() {
    readMode = writeMode = Mode::kNormal;
    reads = writes = syncs = failSyncAt = 0;
    failRename = inflatedSize = false;
}
}
extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_rename(const char*, const char*);
extern "C" int __real_fstat(int, struct stat*);
extern "C" ssize_t __wrap_read(int fd, void* p, size_t n) {
    ++reads;
    if(readMode == Mode::kError) { errno = EIO; return -1; }
    if(readMode == Mode::kZero) return 0;
    if(readMode == Mode::kShort) {
        if(reads == 1) { errno = EINTR; return -1; }
        n = std::min(n, size_t{3});
    }
    return __real_read(fd, p, n);
}
extern "C" ssize_t __wrap_write(int fd, const void* p, size_t n) {
    ++writes;
    if(writeMode == Mode::kError) { errno = ENOSPC; return -1; }
    if(writeMode == Mode::kZero) return 0;
    if(writeMode == Mode::kShort) {
        if(writes == 1) { errno = EINTR; return -1; }
        n = std::min(n, size_t{3});
    }
    return __real_write(fd, p, n);
}
extern "C" int __wrap_fsync(int fd) {
    if(++syncs == failSyncAt) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __wrap_rename(const char* a, const char* b) {
    if(failRename) { errno = EIO; return -1; }
    return __real_rename(a, b);
}
extern "C" int __wrap_fstat(int fd, struct stat* st) {
    int result = __real_fstat(fd, st);
    // 模拟fstat之后文件缩短：未读到的末尾恰好是四个零CRC字节。
    if(result == 0 && inflatedSize) st->st_size += 4;
    return result;
}
namespace {
struct Fixture {
    std::string dir = testutil::MakeTempDir("manifest");
    std::string path = dir + "/MANIFEST";
    Fixture() { Reset(); }
    ~Fixture() { Reset(); testutil::CleanupDir(dir); }
};
template<class Error, class F> void Throws(F action) {
    bool caught = false;
    try { action(); } catch(const Error&) { caught = true; } catch(...) {}
    CHECK(caught);
}
std::string Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Write(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    if(!out) throw std::runtime_error("测试数据写入失败");
}
void Put32(std::string& s, size_t offset, uint32_t value) {
    std::string field; lsm::AppendFixed32(field, value); s.replace(offset, 4, field);
}
void Put64(std::string& s, size_t offset, uint64_t value) {
    std::string field; lsm::AppendFixed64(field, value); s.replace(offset, 8, field);
}
void FixCrc(std::string& s) {
    Put32(s, s.size()-4, lsm::Crc32c(std::string_view(s).substr(0, s.size()-4)));
}
TableMeta Table(uint64_t number = 1) {
    return {number, 0, 512, 10, 20,
            {"apple", 20, ValueType::kValue},
            {"zebra", 10, ValueType::kDeletion}};
}
Manifest Sample() {
    Manifest m; m.SetGeneration(7); m.SetLastSequence(30); m.SetFlushedSequence(20);
    m.AddTable(Table()); return m;
}
void TestRoundTrip() {
    Fixture f;
    Manifest{}.Save(f.path);
    CHECK_EQ(Read(f.path).size(), 52u);
    CHECK(Manifest::Load(f.path).Tables().empty());
    auto m = Sample();
    auto single = Table(2);
    single.minSequence = single.maxSequence = 15;
    single.smallest = single.largest = {"", 15, ValueType::kValue};
    m.AddTable(single);
    auto binary = Table(3);
    binary.smallest.user_key = std::string("a\0\xff", 3);
    binary.largest.user_key = std::string("z\0", 2);
    m.AddTable(binary);
    m.Save(f.path);
    CHECK(!std::filesystem::exists(f.path + ".tmp"));
    auto loaded = Manifest::Load(f.path);
    CHECK_EQ(loaded.Generation(), 7u);
    CHECK_EQ(loaded.LastSequence(), 30u);
    CHECK_EQ(loaded.FlushedSequence(), 20u);
    REQUIRE(loaded.Tables().size() == 3);
    for(size_t i=0; i<3; ++i) {
        const auto& a=m.Tables()[i]; const auto& b=loaded.Tables()[i];
        CHECK_EQ(a.fileNumber,b.fileNumber); CHECK_EQ(a.level,b.level);
        CHECK_EQ(a.fileSize,b.fileSize); CHECK_EQ(a.minSequence,b.minSequence);
        CHECK_EQ(a.maxSequence,b.maxSequence);
        CHECK_STR_EQ(a.smallest.user_key,b.smallest.user_key);
        CHECK_STR_EQ(a.largest.user_key,b.largest.user_key);
        CHECK_EQ(a.smallest.sequence,b.smallest.sequence);
        CHECK_EQ(a.largest.type,b.largest.type);
    }
    m.RemoveTable(2); m.RemoveTable(999); m.Save(f.path);
    CHECK_EQ(Manifest::Load(f.path).Tables().size(), 2u);
}
void TestMalformedFiles() {
    Fixture f; Sample().Save(f.path); const auto good = Read(f.path);
    for(size_t n=0; n<good.size(); ++n) {
        Write(f.path,good.substr(0,n));
        Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    }
    Write(f.path,good+"x");
    Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    for(size_t offset : {size_t{0},size_t{8},size_t{12},size_t{16},size_t{44},
                          size_t{48},size_t{76},size_t{92},good.size()-1}) {
        auto bad=good; bad[offset]^=1;
        if(offset != good.size()-1) FixCrc(bad);
        Write(f.path,bad); Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    }
    auto bad=good; Put64(bad,36,31); FixCrc(bad);
    Write(f.path,bad); Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
}
void TestInvalidObjects() {
    Fixture f;
    const auto reject=[&](TableMeta t) {
        Manifest m; m.SetLastSequence(100); m.SetFlushedSequence(100); m.AddTable(t);
        Throws<std::logic_error>([&]{m.Save(f.path);});
    };
    auto t=Table(); t.minSequence=t.maxSequence=0; reject(t);
    t=Table(); t.fileNumber=0; reject(t);
    t=Table(); t.fileSize=0; reject(t);
    t=Table(); t.smallest.sequence=21; reject(t); // 首键超过上限
    t=Table(); t.largest.sequence=9; reject(t); // 尾键低于下限
    t=Table(); t.smallest.user_key="zz"; reject(t);
    auto m=Sample(); m.AddTable(Table());
    Throws<std::logic_error>([&]{m.Save(f.path);});
}
void TestDecodeRangeChecks() {
    Fixture f; Sample().Save(f.path); const auto good=Read(f.path);
    // TableMeta起点48；首键sequence偏移84，尾键sequence偏移102。
    for(const auto change : {std::pair<size_t,uint64_t>{84,21},{102,9}}) {
        auto bad=good; Put64(bad,change.first,change.second); FixCrc(bad);
        Write(f.path,bad);
        Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    }
}
void TestReadWriteFaults() {
    Fixture f; auto m=Sample();
    writeMode=Mode::kShort; m.Save(f.path); CHECK(writes>2);
    Reset(); readMode=Mode::kShort;
    CHECK_EQ(Manifest::Load(f.path).Generation(),7u); CHECK(reads>2);
    Reset(); const auto original=Read(f.path);
    for(auto mode : {Mode::kError,Mode::kZero}) {
        writeMode=mode;
        Throws<std::system_error>([&]{m.Save(f.path);});
        Reset(); CHECK_STR_EQ(Read(f.path),original);
    }
    readMode=Mode::kError;
    Throws<std::system_error>([&]{Manifest::Load(f.path);}); Reset();
    Throws<std::system_error>([&]{Manifest::Load(f.path+".missing");});
    for(int failAt : {1,2}) {
        failSyncAt=failAt; m.SetGeneration(8+failAt);
        Throws<std::system_error>([&]{m.Save(f.path);});
        Reset();
        // 文件同步失败时旧快照不变；目录同步失败时rename已经发生。
        CHECK_EQ(Manifest::Load(f.path).Generation(),failAt==1 ? 7u : 10u);
    }
    const auto before=Read(f.path); failRename=true;
    Throws<std::system_error>([&]{m.Save(f.path);});
    Reset(); CHECK_STR_EQ(Read(f.path),before);
}
void TestPrematureEof() {
    Fixture f;
    Manifest{}.Save(f.path);
    // 模拟获取大小后提前EOF：读取必须失败，不能把缓冲区中的零当作文件内容。
    inflatedSize=true;
    Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    Reset();
    readMode=Mode::kZero;
    Throws<std::runtime_error>([&]{Manifest::Load(f.path);});
    Reset();
}
}
int main() {
    RUN_TEST(TestRoundTrip);
    RUN_TEST(TestMalformedFiles);
    RUN_TEST(TestInvalidObjects);
    RUN_TEST(TestDecodeRangeChecks);
    RUN_TEST(TestReadWriteFaults);
    RUN_TEST(TestPrematureEof);
    TEST_SUMMARY_RETURN();
}
