#include "lsm/diagnostics.h"
#include <cstdlib>
#define CHECK(x) do {if(!(x)) std::abort();} while(false)
#include <thread>
#include <vector>
#include <sstream>
int main(){
#ifdef LSM_DIAGNOSTICS
 {lsm_diag::Context ctx(lsm_diag::Verify,lsm_diag::Other);
  auto ec=lsm_diag::measure(lsm_diag::Sync,[]{return std::make_error_code(std::errc::io_error);});CHECK(ec);
  try{lsm_diag::measure(lsm_diag::Append,[]{throw 7;});}catch(int){}
  CHECK(lsm_diag::local_slot->cells[lsm_diag::Verify][lsm_diag::Other][lsm_diag::Sync].failed==1);
  CHECK(lsm_diag::local_slot->cells[lsm_diag::Verify][lsm_diag::Other][lsm_diag::Append].failed==1);
 }
 std::vector<std::thread> ts;
 for(int i=0;i<40;++i)ts.emplace_back([]{lsm_diag::Context ctx(lsm_diag::Request,lsm_diag::Put);lsm_diag::count(lsm_diag::Tables,7);});
 for(auto&t:ts)t.join();
 CHECK(lsm_diag::next_slot==41);CHECK(lsm_diag::dropped_threads==9);
 uint64_t count=0,units=0;for(auto&s:lsm_diag::slots){auto&c=s.cells[lsm_diag::Request][lsm_diag::Put][lsm_diag::Tables];count+=c.count;units+=c.units;}CHECK(count==31&&units==217);
 // OS thread IDs can be reused; each new thread receives a fresh, never-recycled slot.
 std::thread t([]{lsm_diag::count(lsm_diag::Tables);});t.join();CHECK(lsm_diag::dropped_threads==10);
 std::ostringstream out;lsm_diag::dump(out);CHECK(!out.str().empty());
#endif
}
