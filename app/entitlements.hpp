#pragma once
#include <stdexcept>
#include <string>
namespace lt {
inline int paidPlanValue(const std::string &name){
    if(name=="Pro"||name=="Ultimate")return 2;
    if(name=="Basic"||name=="Free")throw std::runtime_error("Basic is now Free. Choose Continue with Free; no key is needed.");
    throw std::runtime_error("Unknown license tier");
}
template<class Authorize> void authorizeEntry(int required,Authorize authorize){
    if(required!=0&&required!=2)throw std::runtime_error("Invalid feature tier");
    if(required==0)return;
    if(authorize()!=2)throw std::runtime_error("A Pro license is required for this feature");
}
}
