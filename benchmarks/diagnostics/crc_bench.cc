#include "lsm/coding.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <string>
volatile uint32_t crc_sink=0;
int main(){
 if(lsm::Crc32c("123456789")!=0xe3069283u)return 1;
 // Same implementation compiled separately, no LTO; runtime-generated input is observable.
 std::mt19937_64 rng(20261115);
 for(size_t bytes:{size_t(483),size_t(4025),size_t(4096),size_t(8050)}){
  std::vector<std::string> data(128,std::string(bytes,'\0'));for(auto&s:data)for(auto&c:s)c=static_cast<char>(rng());
  for(auto&s:data)crc_sink=lsm::Crc32c(s);
  size_t iterations=20000;uint32_t sum=0;auto begin=std::chrono::steady_clock::now();
  for(size_t i=0;i<iterations;++i)sum+=lsm::Crc32c(data[i%data.size()]);
  auto end=std::chrono::steady_clock::now();crc_sink=sum;
  double sec=std::chrono::duration<double>(end-begin).count();
  std::cout<<std::setprecision(12)<<"{\"bytes\":"<<bytes<<",\"iterations\":"<<iterations<<",\"seconds\":"<<sec<<",\"us_per_block\":"<<sec*1e6/iterations<<",\"MiB_per_second\":"<<bytes*iterations/sec/1048576.0<<",\"checksum\":"<<sum<<"}\n";
 }
}
