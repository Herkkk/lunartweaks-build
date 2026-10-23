// Executes the production transaction code against a deterministic Windows API fake.
// Does not change the host power plan; native Windows integration is a separate gate.
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <stdexcept>
#include <iostream>
#include <chrono>
using DWORD=uint32_t;using BYTE=unsigned char;using UCHAR=unsigned char;
struct GUID{uint32_t a=0;uint16_t b=0,c=0;BYTE d[8]{};};
const GUID GUID_PROCESSOR_THROTTLE_MINIMUM{1},GUID_PROCESSOR_THROTTLE_MAXIMUM{2},GUID_SYSTEM_COOLING_POLICY{3},GUID_PROCESSOR_PERF_BOOST_MODE{4},GUID_PCIEXPRESS_ASPM_POLICY{5},GUID_PCIEXPRESS_SETTINGS_SUBGROUP{6},GUID_PROCESSOR_SETTINGS_SUBGROUP{7};
struct SYSTEM_POWER_CAPABILITIES{BYTE first[21]{};BYTE DiskSpinDown=0;};
struct SYSTEM_POWER_STATUS{BYTE BatteryFlag=128,ACLineStatus=1;};
constexpr DWORD ERROR_FILE_NOT_FOUND=2,MOVEFILE_REPLACE_EXISTING=1,MOVEFILE_WRITE_THROUGH=8;
#define FAILED(x) ((x)<0)
struct Plan{std::map<uint32_t,DWORD>ac,dc;};
std::map<uint32_t,Plan>plans;uint32_t activeId=10,nextId=100;int writes=0,failWrite=0;bool failActivate=false,failDelete=false,failRecord=false,modern=false,battery=false,onAC=true,unavailable=false;
std::filesystem::path testDir;
bool GetPwrCapabilities(SYSTEM_POWER_CAPABILITIES*c){c->first[20]=modern;return true;}
bool GetSystemPowerStatus(SYSTEM_POWER_STATUS*s){s->BatteryFlag=battery?1:128;s->ACLineStatus=onAC?1:0;return true;}
DWORD PowerGetActiveScheme(void*,GUID**g){*g=new GUID{activeId};return 0;}
void LocalFree(GUID*g){delete g;}
int CoCreateGuid(GUID*g){*g=GUID{nextId++};return 0;}
DWORD PowerReadACValueIndex(void*,const GUID*g,const GUID*,const GUID*k,DWORD*v){if(unavailable||!plans.count(g->a)||!plans.at(g->a).ac.count(k->a))return 2;*v=plans.at(g->a).ac.at(k->a);return 0;}
DWORD PowerReadDCValueIndex(void*,const GUID*g,const GUID*,const GUID*k,DWORD*v){if(!plans.count(g->a)||!plans.at(g->a).dc.count(k->a))return 2;*v=plans.at(g->a).dc.at(k->a);return 0;}
DWORD PowerReadValueMin(void*,const GUID*,const GUID*,DWORD*v){*v=0;return 0;}
DWORD PowerReadValueMax(void*,const GUID*,const GUID*k,DWORD*v){*v=k->a==3?1:k->a==4?4:k->a==5?2:100;return 0;}
DWORD PowerDuplicateScheme(void*,const GUID*g,GUID**dest){if(plans.count((*dest)->a))return 183;plans[(*dest)->a]=plans.at(g->a);return 0;}
DWORD PowerWriteFriendlyName(void*,const GUID*,void*,void*,UCHAR*,DWORD){return 0;}
DWORD PowerWriteACValueIndex(void*,const GUID*g,const GUID*,const GUID*k,DWORD v){writes++;if(failWrite==writes)return 5;plans.at(g->a).ac[k->a]=v;return 0;}
DWORD PowerSetActiveScheme(void*,const GUID*g){if(failActivate||!plans.count(g->a))return 5;activeId=g->a;return 0;}
DWORD PowerDeleteScheme(void*,const GUID*g){if(failDelete||g->a==activeId)return 5;return plans.erase(g->a)?0:2;}
bool DeleteFileW(const wchar_t*p){return std::filesystem::remove(std::filesystem::path(p));}
bool MoveFileExW(const wchar_t*a,const wchar_t*b,DWORD){if(failRecord)return false;std::filesystem::rename(std::filesystem::path(a),std::filesystem::path(b));return true;}
namespace lt {
std::wstring dataDir(){return testDir.wstring()+L"/";}
std::wstring wide(const std::string&s){return {s.begin(),s.end()};}
std::array<BYTE,32>sha(const std::string&s){std::array<BYTE,32>r{};uint32_t h=2166136261u;for(unsigned char c:s)h=(h^c)*16777619u;for(int i=0;i<32;i++)r[i]=BYTE((h>>(i%4*8))^i);return r;}
void durableWrite(const std::wstring&p,const void*d,DWORD n,bool exclusive=false){if(exclusive&&std::filesystem::exists(p))throw std::runtime_error("exists");std::ofstream f(std::filesystem::path(p),std::ios::binary);f.write((const char*)d,n);f.close();if(!f)throw std::runtime_error("write failed");}
std::string readFile(const std::wstring&p){std::ifstream f(std::filesystem::path(p),std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
}
#define LUNAR_POWER_TEST
#include "../app/adaptive_power.hpp"
template<class F>void throws(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}assert(caught);}
void reset(){std::filesystem::remove_all(testDir);std::filesystem::create_directories(testDir);plans.clear();activeId=10;nextId=100;writes=failWrite=0;failActivate=failDelete=failRecord=modern=battery=unavailable=false;onAC=true;
plans[10]={{{1,10},{2,90},{3,0},{lt::adaptive::energy.a,50},{4,0},{5,2},{lt::adaptive::coreParkingMin.a,10},{lt::adaptive::usbSelectiveSuspend.a,1}},{{1,5},{2,80},{3,0},{lt::adaptive::energy.a,70},{4,0},{5,2},{lt::adaptive::coreParkingMin.a,10},{lt::adaptive::usbSelectiveSuspend.a,1}}};}
int main(){using namespace lt::adaptive;testDir=std::filesystem::temp_directory_path()/("lunar-power-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));reset();
auto original=plans[10];
// All ten choices: exact writes to a clone, unchanged OEM/DC values, verified activation and restore.
for(unsigned mode=0;mode<12;mode++){reset();apply(101+mode,mode,L"Test");auto r=load(101+mode);assert(r.ready==1&&has(101+mode)&&status(101+mode,mode)==2);assert(plans[10].ac==original.ac&&plans[10].dc==original.dc);assert(plans[activeId].dc==original.dc);for(DWORD i=0;i<r.count;i++)assert(plans[activeId].ac[setting(Key(r.values[i].key)).a]==r.values[i].target);restore(101+mode);assert(activeId==10&&plans.size()==1&&!has(101+mode));}
// Portable branch must be observable, with battery policies untouched.
reset();battery=true;auto p=proposal(101,0);assert(p.values[3].target==40);apply(101,0,L"Portable");assert(plans[activeId].dc==original.dc);restore(101);
// Unsupported, Modern Standby and unplugged PCs receive no mutation or recovery artifact.
reset();modern=true;throws([]{apply(101,0,L"Blocked");});assert(plans.size()==1&&!has(101));
reset();onAC=false;throws([]{apply(101,0,L"Blocked");});assert(plans.size()==1&&!has(101));
reset();unavailable=true;throws([]{apply(101,0,L"Blocked");});assert(plans.size()==1&&!has(101));
// Partial write and commit failures clean up the clone and keep the active OEM plan.
reset();failWrite=3;throws([]{apply(101,0,L"Fail");});assert(activeId==10&&plans.size()==1&&!has(101));
reset();failRecord=true;throws([]{apply(101,0,L"Fail");});assert(activeId==10&&plans.size()==1&&!has(101));
reset();failActivate=true;throws([]{apply(101,0,L"Fail");});assert(activeId==10&&plans.size()==1&&!has(101));
// Failed cleanup leaves a valid original record that local Undo can recover later.
reset();failWrite=2;failDelete=true;throws([]{apply(101,0,L"Fail");});assert(has(101)&&load(101).original.a==10);failDelete=false;restore(101);assert(!has(101)&&plans.size()==1);
// Multiple power policies restore in reverse order; never overwrite a different active plan.
reset();apply(101,0,L"First");auto first=activeId;apply(102,1,L"Second");throws([]{restore(101);});restore(102);assert(activeId==first);restore(101);assert(activeId==10);
// External edits and corrupt records are detected, retained, and never silently overwritten.
reset();apply(101,0,L"Edit");plans[activeId].ac[1]=44;throws([]{restore(101);});assert(has(101));
reset();apply(101,0,L"Original edit");plans[10].ac[1]=44;throws([]{restore(101);});assert(has(101)&&activeId!=10);
reset();apply(101,0,L"Corrupt");auto raw=lt::readFile(path(101));raw[20]^=1;lt::durableWrite(path(101),raw.data(),DWORD(raw.size()));throws([]{restore(101);});throws([]{apply(101,0,L"Again");});assert(has(101));
std::filesystem::remove_all(testDir);std::cout<<"PASS: all 12 policies, OEM/DC preservation, portable values, capability gates, failure rollback, durable recovery, ordering and tamper detection.\n";
}
