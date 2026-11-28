# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
p=Path('/home/truth/Code/my_kvraft/benchmarks/block_cache_acceptance/bench.cc');s=p.read_text();needle='#ifdef MECHANISM';pos=s.index(needle);s=s[:pos]+'''// Constructor-only observation exposes DB-owned Usage without touching request hot paths.
extern "C" void realCtor(BlockCache*,size_t) asm("__real__ZN10BlockCacheC1Em");
extern "C" void wrapCtor(BlockCache*,size_t) asm("__wrap__ZN10BlockCacheC1Em");
extern "C" void wrapCtor(BlockCache*c,size_t n){realCtor(c,n);observed=c;}
'''+s[pos:];s=s.replace('observed=nullptr;tracking=true','tracking=true');p.write_text(s)
p=p.parent/'CMakeLists.txt';s=p.read_text();s+='\ntarget_link_options(cache-bench PRIVATE "-Wl,--wrap=_ZN10BlockCacheC1Em")\ntarget_link_options(cache-mechanism PRIVATE "-Wl,--wrap=_ZN10BlockCacheC1Em")\n';p.write_text(s)
