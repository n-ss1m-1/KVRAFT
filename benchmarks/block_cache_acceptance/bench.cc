#include "lsm/db.h"
#include "lsm/sstable.h"
#include "lsm/manifest.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>
#include <unistd.h>
namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
static bool tracking=false;static uint64_t lookups=0,hits=0,misses=0,loads=0,bytes=0,preads=0;static BlockCache* observed=nullptr;
// Constructor-only observation exposes DB-owned Usage without touching request hot paths.
extern "C" void realCtor(BlockCache*,size_t) asm("__real__ZN10BlockCacheC1Em");
extern "C" void wrapCtor(BlockCache*,size_t) asm("__wrap__ZN10BlockCacheC1Em");
extern "C" void wrapCtor(BlockCache*c,size_t n){realCtor(c,n);observed=c;}
#ifdef MECHANISM
extern "C" std::shared_ptr<const DataBlockReader> realGet(BlockCache*,const BlockCacheKey&) asm("__real__ZN10BlockCache3GetERK13BlockCacheKey");
extern "C" std::shared_ptr<const DataBlockReader> wrapGet(BlockCache*,const BlockCacheKey&) asm("__wrap__ZN10BlockCache3GetERK13BlockCacheKey");
extern "C" std::shared_ptr<const DataBlockReader> wrapGet(BlockCache*c,const BlockCacheKey&k){auto p=realGet(c,k);if(tracking){observed=c;++lookups;if(p)++hits;else ++misses;}return p;}
extern "C" void realInsert(BlockCache*,const BlockCacheKey&,std::shared_ptr<const DataBlockReader>,size_t) asm("__real__ZN10BlockCache6InsertERK13BlockCacheKeySt10shared_ptrIK15DataBlockReaderEm");
extern "C" void wrapInsert(BlockCache*,const BlockCacheKey&,std::shared_ptr<const DataBlockReader>,size_t) asm("__wrap__ZN10BlockCache6InsertERK13BlockCacheKeySt10shared_ptrIK15DataBlockReaderEm");
extern "C" void wrapInsert(BlockCache*c,const BlockCacheKey&k,std::shared_ptr<const DataBlockReader>p,size_t charge){if(tracking){++loads;bytes+=charge+4;}realInsert(c,k,std::move(p),charge);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void*b,size_t n,off_t o){if(tracking)++preads;return __real_pread(fd,b,n,o);}
#endif
static void check(bool b){if(!b)throw std::runtime_error("verification failed");}
static std::string key(size_t i){auto s=std::to_string(i*2);return std::string(16-s.size(),'0')+s;}
static std::string val(size_t i){auto s=std::to_string(i);return s+std::string(128-s.size(),'v');}
static long rss(){std::ifstream f("/proc/self/status");std::string s;while(std::getline(f,s))if(s.rfind("VmHWM:",0)==0)return std::stol(s.substr(6));return 0;}
int main(int argc,char**argv)try{
 if(argc!=8)throw std::runtime_error("DIR NT CAP PATH KIND N PREFIX");fs::path dir=argv[1];size_t nt=std::stoull(argv[2]),cap=std::stoull(argv[3]),n=std::stoull(argv[6]);std::string path=argv[4],kind=argv[5],prefix=argv[7];const size_t keys=8192;
 if(path=="generate"){
  check(!fs::exists(dir));DBOptions opt;opt.enableWal=false;opt.memTableSizeLimit=SIZE_MAX;opt.BlockCacheCapacity=0;
  DB db(dir.string(),opt);for(size_t t=0;t<nt;++t){for(size_t i=t;i<keys;i+=nt)db.Put(key(i),val(i));db.Flush();}for(size_t i=0;i<keys;++i)check(db.Get(key(i))==std::optional<std::string>{val(i)});return 0;
 }
 auto manifest=Manifest::Load((dir/"MANIFEST").string());check(manifest.Tables().size()==nt);
 auto openReaders=[&](std::shared_ptr<BlockCache> cache){std::vector<std::unique_ptr<SSTableReader>> v;for(auto&m:manifest.Tables()){char name[32];snprintf(name,sizeof(name),"%06llu.sst",static_cast<unsigned long long>(m.fileNumber));v.push_back(std::make_unique<SSTableReader>((dir/name).string(),m.fileNumber,cache));}return v;};
 auto lookup=[&](const auto&readers,const std::string&k)->std::optional<std::string>{for(auto it=readers.rbegin();it!=readers.rend();++it){auto r=(*it)->Get(UINT64_MAX,k);if(r.state==LookupState::kValue)return r.value;if(r.state==LookupState::kDeleted)return std::nullopt;}return std::nullopt;};
 // Independent disabled cache/Readers warm every physical table's own entries and validate.
 {auto warm=openReaders(std::make_shared<BlockCache>(0));for(size_t t=0;t<nt;++t)for(size_t i=t;i<keys;i+=nt){auto result=warm[t]->Get(UINT64_MAX,key(i));check(result.state==LookupState::kValue&&result.value==val(i));}}
 std::mt19937_64 rng(20261115);std::vector<size_t> order(keys);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
 std::vector<std::string> queries,expected;std::vector<bool> present;
 for(size_t j=0;j<n;++j){size_t k=order[j%keys];bool exists=kind!="missing";
  if(kind=="latest"||kind=="oldest"){size_t t=kind=="latest"?nt-1:0;size_t count=(keys-1-t)/nt+1;k=t+(k%count)*nt;}
  if(kind=="hot")k=nt-1+(j%8)*nt; // 8 nearby records in latest table: one block
  if(kind=="uniform")k=rng()%keys;
  if(exists){queries.push_back(key(k));expected.push_back(val(k));}else{auto s=std::to_string(2*k+1);queries.push_back(std::string(16-s.size(),'0')+s);expected.emplace_back();}present.push_back(exists);
 }
 auto cache=std::make_shared<BlockCache>(cap);std::vector<std::unique_ptr<SSTableReader>> readers;std::unique_ptr<DB> db;
 if(path=="db"){DBOptions opt;opt.enableWal=false;opt.BlockCacheCapacity=cap;db=std::make_unique<DB>(dir.string(),opt);}else readers=openReaders(cache);
 struct Row{double sec,p50,p95,p99,max;long peak;int64_t usage;uint64_t q,h,m,l,b,p;};std::vector<Row> rows;std::vector<std::vector<double>> latency(2,std::vector<double>(n));std::vector<std::optional<std::string>> results(n);
 for(size_t pass=0;pass<2;++pass){lookups=hits=misses=loads=bytes=preads=0;tracking=true;auto b=Clock::now();
  for(size_t j=0;j<n;++j){auto t=Clock::now();results[j]=db?db->Get(queries[j]):lookup(readers,queries[j]);latency[pass][j]=std::chrono::duration<double,std::micro>(Clock::now()-t).count();}
  auto e=Clock::now();tracking=false;int64_t usage=path=="db"?-1:static_cast<int64_t>(cache->Usage());if(observed)usage=observed->Usage();
  for(size_t j=0;j<n;++j)check(present[j]?(results[j]&&*results[j]==expected[j]):!results[j]);
  auto sorted=latency[pass];std::sort(sorted.begin(),sorted.end());auto p=[&](size_t v){return sorted[(n*v+99)/100-1];};rows.push_back({std::chrono::duration<double>(e-b).count(),p(50),p(95),p(99),sorted.back(),rss(),usage,lookups,hits,misses,loads,bytes,preads});
 }
 db.reset();readers.clear();cache.reset();std::ofstream raw(prefix+".latency.csv");raw<<"pass,index,us\n"<<std::setprecision(12);for(size_t p=0;p<2;++p)for(size_t i=0;i<n;++i)raw<<p<<','<<i<<','<<latency[p][i]<<'\n';
 for(size_t p=0;p<2;++p){auto&r=rows[p];std::cout<<std::setprecision(12)<<"{\"pass\":"<<p<<",\"operations\":"<<n<<",\"capacity\":"<<cap<<",\"usage\":"<<r.usage<<",\"ops\":"<<n/r.sec<<",\"seconds\":"<<r.sec<<",\"p50_us\":"<<r.p50<<",\"p95_us\":"<<r.p95<<",\"p99_us\":"<<r.p99<<",\"max_us\":"<<r.max<<",\"rss_kib\":"<<r.peak<<",\"cache_lookups\":"<<r.q<<",\"hits\":"<<r.h<<",\"misses\":"<<r.m<<",\"loads\":"<<r.l<<",\"load_bytes_with_crc\":"<<r.b<<",\"pread_calls\":"<<r.p<<"}\n";}
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
