#ifdef PROBE
#include "ap.h"
#endif
#include "base.cc"
struct BenchAccess{
 static std::vector<TableMeta> layout(DB& d){std::lock_guard<std::mutex> l(d.dbMutex_);d.CheckHealthLocked();return d.manifest_.Tables();}
 static size_t usage(DB& d){return d.blockCache_->Usage();}
#if defined(PROBE) || defined(OBSERVE)
 static void settle(DB& d){std::unique_lock<std::mutex> l(d.dbMutex_);bool ok=d.backgroundCv_.wait_for(l,std::chrono::seconds(120),[&]{d.CheckHealthLocked();return !d.benchBusy_&&!d.immutable_&&!d.PickAutoCompactionLevelLocked()&&d.compactionRequested_==d.compactionCompleted_;});need(ok,"convergence timeout");}
#endif
};
DBOptions opts(bool a,bool w,size_t cache=0){auto o=options(cache,262144);o.enableAutoCompaction=a;o.enableWal=w;o.maxLevels=3;o.level0CompactionTrigger=4;o.level1TargetBytes=1048576;o.levelSizeMultiplier=4;o.maxCompactionSourceBytes=1048576;return o;}
struct Lat{uint64_t start,end;};
uint64_t tick(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
void emit(const std::vector<Lat>& v,uint64_t wall,const std::string& file,size_t operations=1){std::vector<uint64_t> a;std::ofstream f(file);f<<"index,start_ns,end_ns,elapsed_ns\n";for(size_t i=0;i<v.size();++i){auto& x=v[i];a.push_back(x.end-x.start);f<<i<<','<<x.start<<','<<x.end<<','<<x.end-x.start<<'\n';}std::sort(a.begin(),a.end());auto q=[&](double p){return a.empty()?0:a[std::min(a.size()-1,size_t(p*(a.size()-1)))];};std::cout<<"{\"count\":"<<v.size()<<",\"wall_ns\":"<<wall<<",\"logical_ops_s\":"<<(wall?double(v.size()*operations)*1e9/wall:0)<<",\"api_s\":"<<(wall?double(v.size())*1e9/wall:0)<<",\"p50_ns\":"<<q(.5)<<",\"p95_ns\":"<<q(.95)<<",\"p99_ns\":"<<q(.99)<<",\"max_ns\":"<<q(1)<<'}';}
void layout(const std::vector<TableMeta>& ts){std::cout<<'[';bool first=true;for(auto&t:ts){if(!first)std::cout<<',';first=false;std::cout<<"{\"file\":"<<t.fileNumber<<",\"level\":"<<t.level<<",\"bytes\":"<<t.fileSize<<'}';}std::cout<<']';}
void fullcheck(const std::string& d,const Oracle& oracle){DB db(d,opts(false,false));verify(db,oracle);auto mf=Manifest::Load(d+"/MANIFEST");std::vector<std::vector<TableMeta>> levels(3);Oracle actual;for(auto&t:mf.Tables()){need(t.level<3,"illegal level");levels[t.level].push_back(t);SSTableReader r(path(d,t.fileNumber));for(auto it=r.Begin();it.Valid();it.Next()){auto k=it.Key();auto p=actual.find(k.user_key);if(p==actual.end()||p->second.key.sequence<k.sequence)actual[k.user_key]={k,it.Value()};}}need(actual.size()==oracle.size(),"physical key mismatch");for(auto&p:oracle){auto&q=actual.at(p.first);need(q.key.sequence==p.second.key.sequence&&q.key.type==p.second.key.type&&q.value==p.second.value,"physical latest mismatch");}for(size_t l=1;l<3;++l){auto&ts=levels[l];std::sort(ts.begin(),ts.end(),[](auto&a,auto&b){return a.smallest.user_key<b.smallest.user_key;});for(size_t i=1;i<ts.size();++i)need(ts[i-1].largest.user_key<ts[i].smallest.user_key,"overlap");}need(mf.FlushedSequence()==mf.LastSequence(),"flushed sequence mismatch");}
void writeRun(const std::string& d,const std::string& out,bool automatic,bool wal,const std::string& type,size_t n,bool converge){need(!fs::exists(d),"directory exists");Oracle oracle;auto db=std::make_unique<DB>(d,opts(type=="mixed"?false:automatic,wal));bool mixed=type=="mixed";uint64_t seq=0;
// Mixed fixture immutable read keys interleave numerically with odd write keys.
std::vector<std::string> readKeys;if(mixed){for(size_t i=0;i<8192;i++){auto k=key(i*2);auto v=value(i,99);db->Put(k,v);oracle[k]={{k,++seq,ValueType::kValue},v};readKeys.push_back(k);}db->Flush();db.reset();db=std::make_unique<DB>(d,opts(automatic,wal));
#if defined(PROBE) || defined(OBSERVE)
if(converge)BenchAccess::settle(*db);
#endif
}
auto initial=BenchAccess::layout(*db);size_t space=type=="unique"?n:32768;std::vector<size_t> ids(space);std::iota(ids.begin(),ids.end(),0);std::mt19937_64 rng(20261115);std::vector<WriteBatch> batches(n/64);uint64_t logicalBytes=0;
for(size_t base=0;base<n;base+=space){std::shuffle(ids.begin(),ids.end(),rng);for(size_t j=0;j<space&&base+j<n;++j){size_t index=base+j,id=ids[j];auto k=key(mixed?id*2+1:id);bool del=type!="unique"&&id%20==0;auto v=del?std::string{}:value(id,base/space);if(del)batches[index/64].Delete(k);else batches[index/64].Put(k,v);lsm::Entry e{{k,++seq,del?ValueType::kDeletion:ValueType::kValue},v};logicalBytes+=lsm::EncodedEntrySize(e);oracle[k]=e;}}
std::vector<Lat> samples(batches.size());std::vector<Lat> reads;if(mixed)reads.reserve(1000000);std::atomic<bool> go{false},stop{false};std::exception_ptr error;uint64_t readStart=0,readEnd=0;std::thread reader;if(mixed)reader=std::thread([&]{try{std::mt19937_64 rr(20261115);while(!go.load())std::this_thread::yield();readStart=tick();while(!stop.load()){auto i=rr()%readKeys.size();auto a=tick();auto v=db->Get(readKeys[i]);auto b=tick();need(v&&*v==value(i,99),"mixed read mismatch");if(reads.size()<1000000)reads.push_back({a,b});else throw std::runtime_error("read sample overflow");}readEnd=tick();}catch(...){error=std::current_exception();}});
auto begin=tick();go=true;for(size_t i=0;i<batches.size();++i){auto a=tick();db->Write(batches[i]);samples[i]={a,tick()};}auto requestEnd=tick();stop=true;if(mixed)reader.join();if(error)std::rethrow_exception(error);
auto f=tick();db->Flush();auto fe=tick();uint64_t settleNs=0;
#if defined(PROBE) || defined(OBSERVE)
if(converge){auto a=tick();BenchAccess::settle(*db);settleNs=tick()-a;}
#endif
auto closeStart=tick();db.reset();auto closed=tick();auto peak=rss();
fullcheck(d,oracle);saveOracle(d,oracle);auto mf=Manifest::Load(d+"/MANIFEST");auto st=stats(d,oracle,false);
std::cout<<"{\"verified\":true,\"auto\":"<<automatic<<",\"wal\":"<<wal<<",\"workload\":\""<<type<<"\",\"logical_ops\":"<<n<<",\"logical_encoded_bytes\":"<<logicalBytes<<",\"begin_ns\":"<<begin<<",\"request_end_ns\":"<<requestEnd<<",\"flush_start_ns\":"<<f<<",\"flush_end_ns\":"<<fe<<",\"flush_ns\":"<<fe-f<<",\"request_to_flush_ns\":"<<fe-begin<<",\"close_ns\":"<<closed-closeStart<<",\"converge_ns\":"<<settleNs<<",\"rss_kib\":"<<peak<<",\"initial\":";layout(initial);std::cout<<",\"final\":";layout(mf.Tables());std::cout<<",\"storage\":";emitStats(st);std::cout<<",\"writes\":";emit(samples,requestEnd-begin,out+"-writes.csv",64);std::cout<<",\"reads\":";emit(reads,readEnd-readStart,out+"-reads.csv");
#ifdef PROBE
std::cout<<",\"mechanism\":{\"flush_bytes\":"<<ap::flushBytes<<",\"flush_count\":"<<ap::flushCount<<",\"backpressure_count\":"<<ap::bpCount<<",\"backpressure_ns\":"<<ap::bpNs<<",\"dropped\":"<<ap::dropped<<'}';ap::dump(out+"-tasks.csv");
#endif
std::cout<<"}\n";}
void queryRun(const std::string& d,const std::string& out,size_t cap,const std::string& workload,size_t n){auto oracle=loadOracle(d);prewarm(d);std::vector<std::string> live,deleted;for(auto&p:oracle)(p.second.key.type==ValueType::kValue?live:deleted).push_back(p.first);std::mt19937_64 rng(20261115);std::vector<std::string> hotspot=live;std::shuffle(hotspot.begin(),hotspot.end(),rng);hotspot.resize(std::min<size_t>(256,hotspot.size()));std::vector<std::string> trace;for(size_t i=0;i<n;++i){if(workload=="live")trace.push_back(live[rng()%live.size()]);else if(workload=="hot")trace.push_back(hotspot[rng()%hotspot.size()]);else if(workload=="deleted"){need(!deleted.empty(),"no deleted keys");trace.push_back(deleted[rng()%deleted.size()]);}else{auto k=key(rng()%oracle.size());k.back()=(k.back()=='e'?'f':char(k.back()+1));trace.push_back(k);need(!oracle.count(k),"missing trace exists");}}
auto db=std::make_unique<DB>(d,opts(false,false,cap));std::vector<Lat> first(n),steady(n);std::vector<std::optional<std::string>> result1(n),result2(n);auto a=tick();for(size_t i=0;i<n;++i){auto t=tick();result1[i]=db->Get(trace[i]);first[i]={t,tick()};}auto b=tick();size_t u1=BenchAccess::usage(*db);auto c=tick();for(size_t i=0;i<n;++i){auto t=tick();result2[i]=db->Get(trace[i]);steady[i]={t,tick()};}auto e=tick();size_t u2=BenchAccess::usage(*db);db.reset();auto peak=rss();for(size_t i=0;i<n;++i){auto p=oracle.find(trace[i]);std::optional<std::string> expected;if(p!=oracle.end()&&p->second.key.type==ValueType::kValue)expected=p->second.value;need(result1[i]==expected&&result2[i]==expected,"query mismatch");}
std::cout<<"{\"verified\":true,\"cache_capacity\":"<<cap<<",\"usage_first\":"<<u1<<",\"usage_steady\":"<<u2<<",\"rss_kib\":"<<peak<<",\"first\":";emit(first,b-a,out+"-first.csv");std::cout<<",\"steady\":";emit(steady,e-c,out+"-steady.csv");
#ifdef PROBE
std::cout<<",\"candidate_tables\":"<<ap::tables;
#endif
std::cout<<"}\n";}
int main(int argc,char**argv){try{std::cout<<std::setprecision(17);std::string mode=argv[1];if(mode=="query")queryRun(argv[2],argv[3],std::stoull(argv[4]),argv[5],std::stoull(argv[6]));else writeRun(argv[2],argv[3],std::stoi(argv[4]),std::stoi(argv[5]),argv[6],std::stoull(argv[7]),mode=="settle");}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}