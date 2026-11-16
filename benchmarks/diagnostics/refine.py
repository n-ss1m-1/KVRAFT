from pathlib import Path
r=Path('/home/truth/Code/my_kvraft')
p=r/'src/lsm/include/lsm/diagnostics.h';s=p.read_text().replace('#include <ostream>','#include <ostream>\n#include <system_error>');s=s.replace('template<class F> decltype(auto) measure(Metric m,F&&f){Timer t(m);return std::forward<F>(f)();}','template<class F> decltype(auto) measure(Metric m,F&&f){Timer t(m);if constexpr(std::is_same_v<std::invoke_result_t<F>,std::error_code>){auto ec=std::forward<F>(f)();t.bad=static_cast<bool>(ec);return ec;}else return std::forward<F>(f)();}')
s=s.replace("if(!first)out<<',';first=false;out<<", "if(!first){out<<',';}first=false;out<<")
p.write_text(s)
p=r/'benchmarks/diagnostics/stats_check.cc';s=p.read_text().replace('#include <cassert>','#include <cstdlib>\n#define CHECK(x) do {if(!(x)) std::abort();} while(false)').replace('assert(','CHECK(');p.write_text(s)
p=r/'benchmarks/diagnostics/diagnose.cc';s=p.read_text();s=s.replace(' std::vector<Result> results;',' std::vector<Result> results;\n double flush_seconds=0,close_seconds=0,total_seconds=0;long peak=0;')
s=s.replace('  auto end=Clock::now();double elapsed=sec(end-begin);','  auto end=Clock::now();double elapsed=sec(end-begin);\n  if(writing){lsm_diag::Context final(lsm_diag::FinalFlush,lsm_diag::Flush);auto fb=Clock::now();db->Flush();auto fe=Clock::now();flush_seconds=sec(fe-fb);total_seconds=sec(fe-begin);peak=rss();}')
s=s.replace(' double flush_seconds=0,close_seconds=0,total_seconds=0;long peak=0;\n if(mode=="write")',' if(mode=="write")')
a=s.index('  // Enclose only request');b=s.index(' }else if(mode=="query")',a);s=s[:a]+'  run("put",order,false,true);\n'+s[b:]
p.write_text(s)
p=r/'src/lsm/lib/db.cc';s=p.read_text();s=s.replace('    lsm::Entry entry;','    LSM_SCOPE(Api);\n    lsm::Entry entry;',2)
s=s.replace('std::optional<std::string> DB::Get(std::string_view key) const \n{','std::optional<std::string> DB::Get(std::string_view key) const \n{\n    LSM_SCOPE(Api);').replace('void DB::Flush() \n{','void DB::Flush() \n{\n    LSM_SCOPE(Api);')
p.write_text(s)
