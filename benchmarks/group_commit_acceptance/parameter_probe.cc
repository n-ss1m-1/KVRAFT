#include "lsm/db.h"
#include <iostream>
int main(int argc,char**argv){
 DBOptions options;options.enableWal=false;options.kMaxGroupRequests=std::stoull(argv[2]);
 std::cerr<<"construct\n";DB db(argv[1],options);std::cerr<<"put-start\n";
 try{db.Put("key","value");std::cerr<<"put-return\n";db.Flush();
 if(db.Get("key")!=std::optional<std::string>("value"))return 3;
 std::cerr<<"verified\n";return 0;
 }catch(const std::exception&e){std::cerr<<"exception: "<<e.what()<<"\n";return 2;}
}
