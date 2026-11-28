#include "lsm/db.h"
#include "lsm/sstable.h"
#include "lsm/manifest.h"
#include "lsm/write_batch.h"
#include "lsm/coding.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
using Oracle=std::map<std::string,lsm::Entry>;
double ns(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::nano>(b-a).count();}
void need(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
std::string key(size_t i){char s[17];std::snprintf(s,sizeof(s),"%016llx",static_cast<unsigned long long>(i*2));return s;}
std::string value(size_t i,size_t version){std::string s(128,'a');for(size_t j=0;j<s.size();++j)s[j]=char('!'+(i*13+version*7+j*17)%90);return s;}
std::string path(const std::string& d,uint64_t n){char s[32];std::snprintf(s,sizeof(s),"%06llu.sst",static_cast<unsigned long long>(n));return d+"/"+s;}
DBOptions options(size_t cap,size_t target=4*1024*1024){DBOptions o;o.enableWal=false;o.blockCacheCapacity=cap;o.memTableSizeLimit=1024*1024;o.targetSSTableDataSize=target;return o;}
void saveOracle(const std::string& d,const Oracle& m){std::ofstream f(d+"/oracle.bin",std::ios::binary);for(const auto& p:m){auto s=lsm::EncodeEntry(p.second);std::string n;lsm::AppendFixed32(n,s.size());f.write(n.data(),4);f.write(s.data(),s.size());}need(bool(f),"save oracle");}
Oracle loadOracle(const std::string& d){Oracle m;std::ifstream f(d+"/oracle.bin",std::ios::binary);char n[4];while(f.read(n,4)){std::string s(lsm::DecodeFixed32(n),'\0');need(bool(f.read(s.data(),s.size())),"oracle read");lsm::Entry e;need(lsm::DecodeEntry(s,e),"oracle decode");m.emplace(e.key.user_key,e);}return m;}
void verify(DB& db,const Oracle& m){for(const auto& p:m){auto v=db.Get(p.first);if(p.second.key.type==ValueType::kDeletion)need(!v,"deleted key exists");else need(v&&*v==p.second.value,"value mismatch");}need(!db.Get("ffffffffffffffff"),"missing key exists");}
struct Stats{uint64_t tables=0,bytes=0,records=0,encoded=0,tombstones=0,flushed=0,physical=0;};
Stats stats(const std::string& d,const Oracle& m,bool exact){auto mf=Manifest::Load(d+"/MANIFEST");Stats s;s.tables=mf.Tables().size();s.flushed=mf.FlushedSequence();std::map<std::string,size_t> counts;for(const auto& t:mf.Tables()){s.bytes+=t.fileSize;SSTableReader r(path(d,t.fileNumber));for(auto it=r.Begin();it.Valid();it.Next()){++s.records;s.encoded+=lsm::EncodedEntrySize({it.Key(),it.Value()});s.tombstones+=it.Key().type==ValueType::kDeletion;if(exact){auto p=m.find(it.Key().user_key);need(p!=m.end(),"unexpected key");need(p->second.key.sequence==it.Key().sequence&&p->second.key.type==it.Key().type&&p->second.value==it.Value(),"physical newest/sequence mismatch");need(++counts[it.Key().user_key]==1,"duplicate compact key");}}}if(exact)need(counts.size()==m.size(),"compact key lost");for(const auto& p:fs::directory_iterator(d))if(p.path().extension()==".sst")++s.physical;return s;}
void emitStats(const Stats& s){std::cout<<"{\"tables\":"<<s.tables<<",\"bytes\":"<<s.bytes<<",\"records\":"<<s.records<<",\"encoded\":"<<s.encoded<<",\"tombstones\":"<<s.tombstones<<",\"flushed\":"<<s.flushed<<",\"physical\":"<<s.physical<<"}";}
long rss(){rusage u{};getrusage(RUSAGE_SELF,&u);return u.ru_maxrss;}
void prewarm(const std::string& d){auto mf=Manifest::Load(d+"/MANIFEST");for(const auto& t:mf.Tables()){SSTableReader r(path(d,t.fileNumber));for(auto it=r.Begin();it.Valid();it.Next()){} }}
