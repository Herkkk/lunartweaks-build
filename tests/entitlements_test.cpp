#include "../app/entitlements.hpp"
#include <cassert>
int main(){
    bool called=false;
    lt::authorizeEntry(0,[&]()->int{called=true;throw std::runtime_error("Offline");});assert(!called);
    for(int required=-1;required<=4;required++)for(int granted=-1;granted<=4;granted++){
        called=false;bool passed=true;
        try{lt::authorizeEntry(required,[&]{called=true;return granted;});}catch(...){passed=false;}
        assert(passed==(required==0||(required==2&&granted==2)));
        assert(called==(required==2));
    }
    assert(lt::paidPlanValue("Pro")==2);assert(lt::paidPlanValue("Ultimate")==2);
    for(auto name:{"Basic","Free","Unknown","pro",""}){bool blocked=false;try{lt::paidPlanValue(name);}catch(...){blocked=true;}assert(blocked);}
    bool blocked=false;try{lt::authorizeEntry(2,[]()->int{throw std::runtime_error("Offline");});}catch(...){blocked=true;}assert(blocked);
}
