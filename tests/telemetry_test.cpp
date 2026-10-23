#include "../app/telemetry.hpp"
#include <cassert>
#include <limits>
int main() {
    lt::CpuCounter c;
    assert(!c.update(100,200,100));
    auto n=c.update(120,250,150); // 100 ticks total, 20 idle.
    assert(n && std::abs(*n-80)<.001f);
    assert(!c.update(120,250,150)); // zero interval
    assert(!c.update(0,0,0)); // counters reset
    assert(c.update(100,100,0).value()==0); // idle is included in kernel
    assert(c.update(100,150,50).value()==100);
    lt::ActivityHistory h;
    for(int i=0;i<100;i++)h.push({float(i),50,double(i)});
    assert(h.count==61 && h.samples[0].seconds==39 && h.samples[60].seconds==99);
    h.push({0,0,99}); assert(h.samples[60].cpu==99);
    h.clear();assert(h.count==0);
    // No invented line before consecutive valid readings arrive.
    assert(!lt::plotHistory(h,false,100).hasSegment);
    h.push({24,42,100});
    auto p=lt::plotHistory(h,false,100);
    assert(p.count==1 && !p.hasSegment && p.points[0].value==24);
    h.push({30,44,101});
    p=lt::plotHistory(h,false,101);
    assert(p.count==2 && p.hasSegment && p.points[1].connected);
    auto ram=lt::plotHistory(h,true,101);
    assert(ram.points[0].value==42 && ram.points[1].value==44);
    // A failed CPU reading breaks only the CPU trace, not RAM or earlier history.
    h.push({-1,45,102});h.push({40,46,103});h.push({50,47,104});
    p=lt::plotHistory(h,false,104);
    assert(p.count==4 && p.points[1].connected && !p.points[2].connected && p.points[3].connected);
    ram=lt::plotHistory(h,true,104);
    assert(ram.count==5 && ram.points[2].connected && ram.points[3].connected);
    // Missing time is visible; never connect across a pause.
    h.push({60,48,110});h.push({61,49,111});
    p=lt::plotHistory(h,false,111);
    assert(!p.points[4].connected && p.points[5].connected);
    // Ignore expired/future/out-of-range/nonfinite samples. Do not clamp them into data.
    h.clear();
    h.push({5,10,39});h.push({0,100,40});h.push({100,0,41});
    h.push({std::numeric_limits<float>::quiet_NaN(),5,42});
    h.push({101,5,43});h.push({-1,5,44});h.push({30,50,99});
    h.push({40,60,100});h.push({50,70,101});
    p=lt::plotHistory(h,false,100);
    assert(p.count==4 && p.points[0].seconds==40 && p.points[3].seconds==100);
    assert(p.points[0].value==0 && p.points[1].value==100);
    assert(p.points[1].connected && !p.points[2].connected && p.points[3].connected);
    assert(lt::easeOut(-1)==0 && lt::easeOut(1)==1 && lt::easeOut(.5f)>.5f);
}
