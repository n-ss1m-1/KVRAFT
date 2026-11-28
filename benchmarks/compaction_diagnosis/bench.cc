#include "dd.h"
#include "base.cc"
void hot(const std::string& d,int variant,const std::string& out){auto m=loadOracle(d);std::vector<std::string> live;for(auto& p:m)if(p.second.key.type==ValueType::kValue)live.push_back(p.first);std::vector<std::string> pool;if(variant==0)pool.assign(live.begin(),live.begin()+32);else{std::mt19937_64 r(20261115+variant);for(int i=0;i<32;++i)pool.push_back(live[r()%live.size()]);}std::mt19937_64 rng(20261115);std::vector<std::string> trace;for(int i=0;i<8192;++i)trace.push_back(pool[rng()%32]);{std::ofstream f(out+"-trace.txt");for(auto&k:trace)f<<k<<'\n';}prewarm(d);auto db=std::make_unique<DB>(d,options(40531456));std::vector<Sample> a(trace.size());std::vector<std::optional<std::string>> results(trace.size());std::cout<<"{\"mode\":\"hot\",\"variant\":"<<variant<<",\"capacity\":40531456,\"passes\":[";for(int pass=0;pass<2;++pass){DD_PHASE(pass+1);auto start=Clock::now();for(size_t i=0;i<trace.size();++i){auto x=Clock::now();
#ifdef PROBE
 dd::Query q;
#endif
 results[i]=db->Get(trace[i]);auto y=Clock::now();a[i]={ns(start,x),ns(x,y)};}auto end=Clock::now();for(size_t i=0;i<trace.size();++i)need(results[i]&&*results[i]==m.at(trace[i]).value,"hot result");if(pass)std::cout<<',';emitLatency("hot-"+std::to_string(pass),a,ns(start,end),out+"-"+std::to_string(pass)+".csv");}db.reset();{DD_PHASE(7);DB r(d,options(0));verify(r,m);}std::cout<<"],\"verified\":true}\n";}
int main(int argc,char** argv){std::cout<<std::setprecision(17);try{need(argc>=3,"arguments");std::string mode=argv[1];if(mode=="hot")hot(argv[2],std::stoi(argv[3]),argv[4]);else if(mode=="write")impact(argv[2],std::stoi(argv[3]),true,256,argv[4],543750000);else if(mode=="compact")compact(argv[2],true,4194304);else throw std::runtime_error("mode");
#ifdef PROBE
 if(auto* f=std::getenv("DD_OUT"))dd::dump(f);
#endif
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
