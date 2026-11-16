#include "lsm/data_block.h"
#include "test_util.h"
#include <stdexcept>
#include <string>
namespace {
lsm::Entry Entry(uint64_t seq, const char* key, const char* value, ValueType type=ValueType::kValue) { lsm::Entry e; e.key.sequence=seq; e.key.user_key=key; e.key.type=type; if(type==ValueType::kValue)e.value=value; return e; }
void TestBuilderAndIterator(){DataBlockBuilder b;b.Add(Entry(20,"apple","red"));b.Add(Entry(10,"apple","green"));b.Add(Entry(8,"banana","yellow"));DataBlockReader r(std::string(b.Data()));auto i=r.Begin();REQUIRE(i.Valid());CHECK_EQ(i.Key().sequence,20u);i.Next();REQUIRE(i.Valid());CHECK_EQ(i.Key().sequence,10u);CHECK_STR_EQ(i.Value(),"green");i.Next();REQUIRE(i.Valid());CHECK_STR_EQ(i.Key().user_key,"banana");i.Next();CHECK(!i.Valid());}
void TestGet(){DataBlockBuilder b;b.Add(Entry(30,"apple","red"));b.Add(Entry(20,"apple","",ValueType::kDeletion));b.Add(Entry(10,"apple","green"));b.Add(Entry(8,"banana","yellow"));DataBlockReader r(std::string(b.Data()));CHECK_EQ(r.Get(25,"apple").state,LookupState::kDeleted);CHECK_EQ(r.Get(15,"apple").state,LookupState::kValue);CHECK_STR_EQ(r.Get(15,"apple").value,"green");CHECK_EQ(r.Get(100,"cherry").state,LookupState::kNotFound);}
void TestValidation(){DataBlockBuilder b;b.Add(Entry(2,"key","v"));bool t=false;try{b.Add(Entry(1,"aaa","v"));}catch(const std::invalid_argument&){t=true;}CHECK(t);b.Reset();CHECK(b.Empty());b.Add(Entry(1,"aaa","v"));t=false;try{b.Add(Entry(1,"aaa","other"));}catch(const std::invalid_argument&){t=true;}CHECK(t);}
void TestCorruption(){const auto a=lsm::EncodeEntry(Entry(1,"b","v"));const auto z=lsm::EncodeEntry(Entry(2,"a","v"));bool t=false;try{DataBlockReader r(a+z);}catch(const std::runtime_error&){t=true;}CHECK(t);t=false;try{DataBlockReader r(a.substr(0,a.size()-1));}catch(const std::runtime_error&){t=true;}CHECK(t);}
}
int main(){RUN_TEST(TestBuilderAndIterator);RUN_TEST(TestGet);RUN_TEST(TestValidation);RUN_TEST(TestCorruption);TEST_SUMMARY_RETURN();}
