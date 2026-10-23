#pragma once
#include <vector>
#include <stdexcept>
namespace lt::adaptive {
enum class Key { Minimum, Maximum, Cooling, Energy, Boost, Pcie, CoreParking, UsbSuspend };
constexpr unsigned keyCount = 8;
constexpr unsigned maxValues = 10;
struct Choice { Key key; unsigned value; bool required; };
// Workload policies matched to the PC's power source. "portable" = battery-equipped PC.
// Only AC values are written; battery (DC) values are always inherited unchanged.
inline std::vector<Choice> policy(unsigned mode, bool portable) {
    using K=Key;
    switch(mode) {
    case 0:return {{K::Minimum,5,true},{K::Maximum,100,true},{K::Cooling,1,false},{K::Energy,portable?40u:25u,false},{K::Boost,1,false}};
    case 1:return {{K::Minimum,5,true},{K::Maximum,100,true},{K::Energy,portable?25u:10u,true},{K::Boost,1,false}};
    case 2:return {{K::Minimum,5,true},{K::Maximum,100,true},{K::Energy,portable?60u:45u,false},{K::Boost,0,true}};
    // Lunar Performance plan: the full Pro power stack in one reversible plan.
    case 3:return {{K::Minimum,portable?5u:10u,true},{K::Maximum,100,true},{K::Cooling,1,false},
                   {K::Energy,portable?15u:0u,false},{K::Boost,portable?1u:2u,false},
                   {K::CoreParking,portable?50u:100u,false},{K::Pcie,portable?1u:0u,false},{K::UsbSuspend,0,false}};
    case 4:return {{K::Cooling,1,true}};
    case 5:return {{K::Maximum,100,true}};
    case 6:return {{K::Minimum,5,true}};
    case 7:return {{K::Energy,portable?25u:0u,true}};
    case 8:return {{K::Boost,portable?1u:2u,true}};
    case 9:return {{K::Pcie,portable?1u:0u,true}};
    case 10:return {{K::CoreParking,portable?50u:100u,true}};
    case 11:return {{K::UsbSuspend,0,true}};
    default:throw std::runtime_error("Unknown power policy");
    }
}
inline bool allowedValue(Key k,unsigned value) {
    switch(k) {
    case Key::Minimum:case Key::Maximum:case Key::Energy:case Key::CoreParking:return value<=100;
    case Key::Cooling:case Key::UsbSuspend:return value<=1;
    case Key::Boost:return value<=6;
    case Key::Pcie:return value<=2;
    }
    return false;
}
}
