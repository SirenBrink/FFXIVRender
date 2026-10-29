#include "xivrender/ffxiv_signatures.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
int wmain(int argc,wchar_t**argv){
 if(argc!=2)return 2;std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary);std::vector<std::uint8_t>bytes((std::istreambuf_iterator<char>(input)),{});
 auto report=xivrender::ffxiv::inspect_executable(bytes);for(auto&s:report.evidence)std::cout<<s<<'\n';for(auto&s:report.errors)std::cout<<"ERROR: "<<s<<'\n';
 std::cout<<"Simulation tick RVA=0x"<<std::hex<<report.addresses.simulation_tick<<std::dec<<'\n';return report.supported&&report.addresses.simulation_tick?0:1;
}
