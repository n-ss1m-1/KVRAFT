#include "lsm/bloom_filter.h"
#include "test_util.h"
#include <limits>
#include <stdexcept>
#include <string>
namespace {
void TestSizing() { BloomFilter f(10,10,3); CHECK_EQ(f.BitCount(),100u); CHECK_EQ(f.ByteSize(),13u); BloomFilter e(0,10,3); CHECK_EQ(e.ByteSize(),0u); CHECK(!e.MayContain("x")); e.Add("x"); CHECK(!e.MayContain("x")); }
void TestKeys() { BloomFilter f(64,10,3); std::string b("a\0b\xFF",4); f.Add("apple"); f.Add(b); CHECK(f.MayContain("apple")); CHECK(f.MayContain(b)); BloomFilter e(64,10,3); CHECK(!e.MayContain("apple")); CHECK(!e.MayContain(b)); }
void TestParameters() { BloomFilter f(16,1,1); f.Add("same"); f.Add("same"); CHECK(f.MayContain("same")); bool t=false; try { BloomFilter x(1,0,3); } catch(const std::invalid_argument&) {t=true;} CHECK(t); t=false; try { BloomFilter x(1,10,0); } catch(const std::invalid_argument&) {t=true;} CHECK(t); t=false; try { BloomFilter x(std::numeric_limits<size_t>::max(),2,3); } catch(const std::length_error&) {t=true;} CHECK(t); }
void TestFalsePositiveRate() { BloomFilter f(10000,10,3); for(int i=0;i<1000;++i) f.Add("present-"+std::to_string(i)); for(int i=0;i<1000;++i) CHECK(f.MayContain("present-"+std::to_string(i))); int fp=0; for(int i=0;i<1000;++i) fp += f.MayContain("absent-"+std::to_string(i)); CHECK(fp<100); }
}
int main(){RUN_TEST(TestSizing);RUN_TEST(TestKeys);RUN_TEST(TestParameters);RUN_TEST(TestFalsePositiveRate);TEST_SUMMARY_RETURN();}
