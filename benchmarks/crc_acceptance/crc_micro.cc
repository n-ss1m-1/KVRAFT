#include "lsm/coding.h"
#include <chrono>
#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <string>
volatile uint32_t sink=0;
int main(){
 std::mt19937_64 rng(20261115);
 for(size_t bytes:{size_t(128),size_t(4025),size_t(4096),size_t(65536)}){
  std::vector<std::string> data(128,std::string(bytes,'\0'));for(auto&s:data)for(auto&c:s)c=static_cast<char>(rng());
  for(auto&s:data)sink=lsm::Crc32c(s);
  const size_t n=bytes==65536?2000:20000;uint32_t sum=0;
  auto b=std::chrono::steady_clock::now();
  for(size_t i=0;i<n;++i)sum+=lsm::Crc32c(data[i%128]);
  auto e=std::chrono::steady_clock::now();sink=sum;
  double sec=std::chrono::duration<double>(e-b).count();
  std::cout<<std::setprecision(12)<<"{\"bytes\":"<<bytes<<",\"calls\":"<<n<<",\"us_per_block\":"<<sec*1e6/n<<",\"MiB_per_second\":"<<bytes*n/sec/1048576.0<<",\"checksum\":"<<sum<<"}\n";
 }
}
