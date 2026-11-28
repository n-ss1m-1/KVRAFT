#define main old_benchmark_main
#include "../source_expansion_resume/bench.cc"
#undef main
int main(int argc,char**argv){try{
 std::string mode=argv[1],d=argv[2];
 if(mode=="check"){
 size_t n=131072;Oracle oracle;std::vector<size_t> ids(n);std::iota(ids.begin(),ids.end(),0);std::mt19937_64 rng(20261115);std::shuffle(ids.begin(),ids.end(),rng);
 for(size_t i=0;i<n;++i){size_t id=ids[i];auto k=key(id);oracle[k]={{k,i+1,ValueType::kValue},value(id,0)};}
 fullcheck(d,oracle);std::cout<<"FULLCHECK PASS\n";return 0;}
 auto mf=Manifest::Load(d+"/MANIFEST");std::cout<<"{\"last\":"<<mf.LastSequence()<<",\"flushed\":"<<mf.FlushedSequence()<<",\"tables\":";layout(mf.Tables());std::cout<<"}\n";
 return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}