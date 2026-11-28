# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import shutil,subprocess
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/crc-table-acceptance-20261118';o.mkdir(exist_ok=False)
(o/'git-before.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
shutil.copytree(r/'src/lsm',o/'snapshots/new/lsm');shutil.copytree(r/'src/lsm',o/'snapshots/old/lsm')
shutil.copy2(r/'tests/lsm/test_coding.cc',o/'test_coding.before.cc')
p=o/'snapshots/old/lsm/lib/coding.cc';s=p.read_text();a=s.index('namespace ');b=s.index('void lsm::AppendFixed32',a);s=s[:a]+s[b:];a=s.index('// !注意实现思路');b=s.index('size_t lsm::EncodedInternalKeySize',a)
old='''uint32_t lsm::Crc32c(std::string_view bytes) {
 uint32_t crc=0xffffffffu;
 for(char ch:bytes){crc^=static_cast<unsigned char>(ch);for(int bit=0;bit<8;++bit){if(crc&1)crc=(crc>>1)^0x82f63b78u;else crc>>=1;}}
 return ~crc;
}

'''
s=s[:a]+old+s[b:];p.write_text(s)
p=o/'test_coding.prepared.cc';shutil.copy2(r/'tests/lsm/test_coding.cc',p);s=p.read_text();a=s.index('void TestCrc32c()');b=s.index('\nvoid TestEntryEncoding',a)
s=s[:a]+'''// Test-only compatibility oracle: original reflected, bit-at-a-time CRC32C.
uint32_t ReferenceCrc32c(std::string_view bytes) {
    uint32_t crc = 0xffffffffu;
    for (char ch : bytes) {
        crc ^= static_cast<unsigned char>(ch);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) crc = (crc >> 1) ^ 0x82f63b78u;
            else crc >>= 1;
        }
    }
    return ~crc;
}
void TestCrc32c() {
    CHECK_EQ(lsm::Crc32c(std::string_view{}), 0u);
    CHECK_EQ(lsm::Crc32c("123456789"), 0xE3069283u);
    for (unsigned a = 0; a < 256; ++a) {
        const char one = static_cast<char>(a);
        CHECK_EQ(lsm::Crc32c(std::string_view(&one, 1)), ReferenceCrc32c(std::string_view(&one, 1)));
        for (unsigned b = 0; b < 256; ++b) {
            const char two[] = {static_cast<char>(a), static_cast<char>(b)};
            CHECK_EQ(lsm::Crc32c(std::string_view(two, 2)), ReferenceCrc32c(std::string_view(two, 2)));
        }
    }
    std::mt19937_64 rng(20261115);
    for (size_t n : {size_t(3), size_t(7), size_t(8), size_t(9), size_t(255), size_t(256), size_t(257), size_t(4025), size_t(4096), size_t(65536), size_t(1048576)}) {
        for (int pattern = 0; pattern < 4; ++pattern) {
            std::string data(n, '\\0');
            for (size_t i = 0; i < n; ++i) {
                if (pattern == 1) data[i] = static_cast<char>(0xff);
                if (pattern == 2) data[i] = static_cast<char>(i % 256);
                if (pattern == 3) data[i] = static_cast<char>(rng());
            }
            const auto expected = ReferenceCrc32c(data);
            CHECK_EQ(lsm::Crc32c(data), expected);
            std::string surrounded = std::string("prefix\\xff\\0", 8) + data + std::string("\\0suffix\\x80", 8);
            auto part = std::string_view(surrounded).substr(8, n);
            CHECK_EQ(lsm::Crc32c(part), expected);
            surrounded[0] ^= 0x55;
            surrounded.back() ^= 0x55;
            CHECK_EQ(lsm::Crc32c(std::string_view(surrounded).substr(8, n)), expected);
        }
    }
}
''' +s[b:];p.write_text(s)
