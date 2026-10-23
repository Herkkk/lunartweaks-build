#pragma once
#ifndef LUNAR_POWER_TEST
#include "util.hpp"
#include <powrprof.h>
#endif
#include "power_policy.hpp"
#include <cstring>
#include <array>
namespace lt::adaptive {
inline const GUID energy={0x36687f9e,0xe3a5,0x4dbf,{0xb1,0xdc,0x15,0xeb,0x38,0x1c,0x68,0x63}};
inline const GUID coreParkingMin={0x0cc5b647,0xc1df,0x4637,{0x89,0x1a,0xde,0xc3,0x5c,0x31,0x85,0x83}};
inline const GUID usbSubgroup={0x2a737441,0x1930,0x4402,{0x8d,0x77,0xb2,0xbe,0xbb,0xa3,0x08,0xa3}};
inline const GUID usbSelectiveSuspend={0x48e6b7a6,0x50f5,0x4782,{0xa5,0xd4,0x53,0xbb,0x8f,0x07,0xe2,0x26}};
inline const GUID &setting(Key k) {
    switch(k) {
    case Key::Minimum:return GUID_PROCESSOR_THROTTLE_MINIMUM;
    case Key::Maximum:return GUID_PROCESSOR_THROTTLE_MAXIMUM;
    case Key::Cooling:return GUID_SYSTEM_COOLING_POLICY;
    case Key::Energy:return energy;
    case Key::Boost:return GUID_PROCESSOR_PERF_BOOST_MODE;
    case Key::Pcie:return GUID_PCIEXPRESS_ASPM_POLICY;
    case Key::CoreParking:return coreParkingMin;
    case Key::UsbSuspend:return usbSelectiveSuspend;
    }
    throw std::runtime_error("Unknown setting");
}
inline const GUID &group(Key k){return k==Key::Pcie?GUID_PCIEXPRESS_SETTINGS_SUBGROUP:k==Key::UsbSuspend?usbSubgroup:GUID_PROCESSOR_SETTINGS_SUBGROUP;}
inline const wchar_t *label(Key k){
    switch(k){case Key::Minimum:return L"CPU minimum (%)";case Key::Maximum:return L"CPU maximum (%)";
    case Key::Cooling:return L"Cooling (1 = active)";case Key::Energy:return L"Energy preference (0 = performance)";
    case Key::Boost:return L"Boost (0 = off, 1 = on, 2 = aggressive)";case Key::Pcie:return L"PCIe savings (0 = off, 1 = moderate)";
    case Key::CoreParking:return L"Unparked cores (%)";case Key::UsbSuspend:return L"USB selective suspend (0 = off)";}
    return L"Setting";
}
inline GUID active(){GUID *p=nullptr;if(PowerGetActiveScheme(nullptr,&p)!=0||!p)throw std::runtime_error("Active power plan unavailable");GUID g=*p;LocalFree(p);return g;}
inline bool equal(const GUID&a,const GUID&b){return memcmp(&a,&b,sizeof(a))==0;}
struct Machine{bool portable=false,ac=false;};
inline Machine machine(){
    SYSTEM_POWER_CAPABILITIES c{};SYSTEM_POWER_STATUS s{};
    if(!GetPwrCapabilities(&c)||!GetSystemPowerStatus(&s)||s.BatteryFlag==255)
        throw std::runtime_error("Power capabilities unavailable. No policy will be guessed.");
    // Older MinGW headers call the Windows 8+ AoAc ABI byte spare2[2].
    static_assert(offsetof(SYSTEM_POWER_CAPABILITIES,DiskSpinDown)==21);
    bool modernStandby=reinterpret_cast<const BYTE*>(&c)[20]!=0;
    if(modernStandby)throw std::runtime_error("This PC uses Modern Standby. Use its OEM Windows power mode; legacy custom plans are not offered.");
    return {s.BatteryFlag!=128,s.ACLineStatus==1};
}
struct Value{DWORD key=0,before=0,dc=0,target=0;};
struct Record{DWORD magic=0x4c545031,version=2,id=0,ready=0;GUID original{},owned{};DWORD count=0;Value values[maxValues]{};BYTE hash[32]{};};
// 1.11 recovery records stay restorable after the update.
struct RecordV1{DWORD magic=0x4c545031,version=1,id=0,ready=0;GUID original{},owned{};DWORD count=0;Value values[6]{};BYTE hash[32]{};};
inline std::wstring path(int id){return dataDir()+L"\\backups\\adaptive-"+std::to_wstring(id)+L".ltp";}
inline bool has(int id){return std::filesystem::exists(path(id));}
inline void seal(Record&r){auto h=sha(std::string((const char*)&r,offsetof(Record,hash)));memcpy(r.hash,h.data(),32);}
inline void markReady(Record&r){
    r.ready=1;seal(r);auto temporary=path(r.id)+L".pending";
    durableWrite(temporary,&r,sizeof(r));
    if(!MoveFileExW(temporary.c_str(),path(r.id).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temporary.c_str());throw std::runtime_error("Could not commit the power recovery record");
    }
}
inline Record load(int id){auto raw=readFile(path(id));Record r{};
    if(raw.size()==sizeof(RecordV1)){RecordV1 v{};memcpy(&v,raw.data(),sizeof(v));auto h=sha(raw.substr(0,offsetof(RecordV1,hash)));
        if(v.magic!=0x4c545031||v.version!=1||v.id!=DWORD(id)||v.count==0||v.count>6||v.ready>1||memcmp(v.hash,h.data(),32))throw std::runtime_error("Power recovery validation failed; original preserved");
        r.version=1;r.id=v.id;r.ready=v.ready;r.original=v.original;r.owned=v.owned;r.count=v.count;for(DWORD i=0;i<v.count;i++)r.values[i]=v.values[i];
    }else{
        if(raw.size()!=sizeof(Record))throw std::runtime_error("Power recovery record is incomplete");
        memcpy(&r,raw.data(),sizeof(r));auto h=sha(raw.substr(0,offsetof(Record,hash)));
        if(r.magic!=0x4c545031||r.version!=2||r.id!=DWORD(id)||r.count==0||r.count>maxValues||r.ready>1||memcmp(r.hash,h.data(),32))throw std::runtime_error("Power recovery validation failed; original preserved");
    }
    for(DWORD i=0;i<r.count;i++) {
        if(r.values[i].key>=keyCount||!allowedValue(Key(r.values[i].key),r.values[i].target))throw std::runtime_error("Invalid power recovery value");
    }
    return r;
}
inline Record proposal(int id,unsigned mode){
    auto m=machine();Record r;r.id=id;r.original=active();
    for(auto p:policy(mode,m.portable)){
        DWORD ac=0,dc=0;auto k=p.key;
        bool ok=PowerReadACValueIndex(nullptr,&r.original,&group(k),&setting(k),&ac)==0 && PowerReadDCValueIndex(nullptr,&r.original,&group(k),&setting(k),&dc)==0;
        if(ok){DWORD minimum=0,maximum=0;
            if(PowerReadValueMin(nullptr,&group(k),&setting(k),&minimum)==0 && PowerReadValueMax(nullptr,&group(k),&setting(k),&maximum)==0 && maximum>minimum)
                ok=p.value>=minimum&&p.value<=maximum;
        }
        if(!ok){if(p.required)throw std::runtime_error("This power policy is not exposed by the current Windows/OEM plan. Use the device power settings instead.");continue;}
        if(r.count>=maxValues)break;
        r.values[r.count++]={DWORD(k),ac,dc,p.value};
    }
    if(!r.count)throw std::runtime_error("No supported power settings found");
    return r;
}
inline bool matches(const Record&r){
    for(DWORD i=0;i<r.count;i++){auto &v=r.values[i];auto k=Key(v.key);DWORD ac=0,dc=0;
        if(PowerReadACValueIndex(nullptr,&r.owned,&group(k),&setting(k),&ac)!=0||PowerReadDCValueIndex(nullptr,&r.owned,&group(k),&setting(k),&dc)!=0||ac!=v.target||dc!=v.dc)return false;
    }return true;
}
inline int status(int id,unsigned mode){
    if(has(id)){auto r=load(id);return equal(active(),r.owned)&&r.ready&&matches(r)?2:3;}
    proposal(id,mode);return 1;
}
inline std::wstring describe(int id,unsigned mode){
    try{auto m=machine();auto r=proposal(id,mode);
        std::wstring s=L"Matched to this PC\n"+std::wstring(m.portable?L"Laptop / battery PC":L"Desktop PC")+L" · "+std::to_wstring(r.count)+L" power settings matched.\n\n";
        for(DWORD i=0;i<r.count;i++){auto&v=r.values[i];s+=std::wstring(label(Key(v.key)))+L": "+std::to_wstring(v.before)+L" → "+std::to_wstring(v.target)+L" (AC)\n";}
        s+=L"\nCreates a copy of your active plan. Battery settings and all other values are inherited unchanged. Requires AC power. Firmware and Windows power modes may override these policies. Compare your workload and temperatures; these are starting policies, not a proven optimum. Restore power changes newest first.";
        return s;
    }catch(const std::exception&e){return L"Unavailable on this PC\n"+wide(e.what());}
}
inline void apply(int id,unsigned mode,const wchar_t *name){
    if(has(id))throw std::runtime_error("This power feature already has a recovery record. Restore it in Undo before applying again.");
    auto m=machine();if(!m.ac)throw std::runtime_error("Connect AC power before applying a power policy");
    auto r=proposal(id,mode);if(FAILED(CoCreateGuid(&r.owned)))throw std::runtime_error("Could not allocate a plan identifier");
    seal(r);durableWrite(path(id),&r,sizeof(r),true); // Original recorded before creating a plan.
    bool created=false;
    try{
        GUID *target=&r.owned;if(PowerDuplicateScheme(nullptr,&r.original,&target)!=0)throw std::runtime_error("Windows could not duplicate the active plan");created=true;
        std::wstring title=L"LunarTweaks · "+std::wstring(name);
        if(PowerWriteFriendlyName(nullptr,&r.owned,nullptr,nullptr,(UCHAR*)title.c_str(),DWORD((title.size()+1)*sizeof(wchar_t)))!=0)throw std::runtime_error("Could not name the custom plan");
        for(DWORD i=0;i<r.count;i++){auto &v=r.values[i];auto k=Key(v.key);if(PowerWriteACValueIndex(nullptr,&r.owned,&group(k),&setting(k),v.target)!=0)throw std::runtime_error("Windows rejected a power value");}
        if(!matches(r))throw std::runtime_error("Power value verification failed");
        markReady(r);
        if(!equal(active(),r.original))throw std::runtime_error("The active plan changed during preparation; no plan was activated");
        if(!machine().ac)throw std::runtime_error("AC power was disconnected during preparation");
        if(PowerSetActiveScheme(nullptr,&r.owned)!=0||!equal(active(),r.owned))throw std::runtime_error("Windows did not activate the custom plan");
    }catch(...){
        bool safe=false;try{auto a=active();if(equal(a,r.owned))safe=PowerSetActiveScheme(nullptr,&r.original)==0&&equal(active(),r.original);else safe=!equal(a,r.owned);}catch(...){}
        if(safe&&(!created||PowerDeleteScheme(nullptr,&r.owned)==0))DeleteFileW(path(id).c_str());
        throw;
    }
}
inline void restore(int id){
    auto r=load(id);auto a=active();
    if(!equal(a,r.owned)&&!equal(a,r.original))throw std::runtime_error("A different plan is active. Restore newer LunarTweaks power changes first, or review the active plan in Windows.");
    if(equal(a,r.owned)&&r.ready&&!matches(r))throw std::runtime_error("The custom plan was edited outside LunarTweaks. Review it before restoring.");
    for(DWORD i=0;i<r.count;i++){
        auto &v=r.values[i];auto k=Key(v.key);DWORD ac=0,dc=0;
        if(PowerReadACValueIndex(nullptr,&r.original,&group(k),&setting(k),&ac)!=0||PowerReadDCValueIndex(nullptr,&r.original,&group(k),&setting(k),&dc)!=0||ac!=v.before||dc!=v.dc)
            throw std::runtime_error("The original plan is missing or its saved settings changed. Review it before restoring; recovery record preserved.");
    }
    if(PowerSetActiveScheme(nullptr,&r.original)!=0||!equal(active(),r.original))throw std::runtime_error("The original power plan is unavailable; recovery record preserved");
    DWORD result=PowerDeleteScheme(nullptr,&r.owned);
    if(result!=0&&result!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Original restored, but custom plan cleanup failed; recovery record preserved");
    if(!DeleteFileW(path(id).c_str()))throw std::runtime_error("Restored, but recovery record cleanup failed");
}
}
