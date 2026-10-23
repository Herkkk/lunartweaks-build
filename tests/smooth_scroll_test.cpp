#include "../app/smooth_scroll.hpp"
#include <cassert>
#include <iostream>
#include <vector>
static void finish(lt::SmoothScroll &s, uint64_t now) {
    for (unsigned n=0;n<200 && s.moving();++n) { now+=16; s.advance(now); }
    assert(!s.moving()); assert(s.position==s.target);
}
int main() {
    lt::SmoothScroll s; s.configure(1280,384); assert(s.limit==896);
    assert(s.to(s.wheelTarget(-120,3),100)); assert(s.target==72); assert(s.position==0);
    float previous=0;
    for(uint64_t t=116;t<500;t+=16) {s.advance(t); assert(s.position>=previous && s.position<=72);previous=s.position;}
    finish(s,500); assert(s.position==72);
    // Four small touchpad deltas have the same target as one wheel detent.
    lt::SmoothScroll fine;fine.configure(1280,384);
    for(int i=0;i<4;++i)fine.to(fine.wheelTarget(-30,3),100+i*8);
    assert(fine.target==72);finish(fine,200);assert(fine.position==72);
    // Rapid retarget/reversal never jumps the displayed position or overshoots.
    s.reset();s.to(700,1000);s.advance(1016);float mid=s.position;
    s.to(0,1016);assert(s.position==mid);s.advance(1032);assert(s.position<mid && s.position>=0);
    finish(s,1050);assert(s.position==0);
    assert(s.to(99999,2000,true));assert(s.target==896 && s.position==896);
    assert(!s.to(99999,2001));assert(s.position==896);
    s.to(-100,2002,true);assert(s.position==0);
    // Reduced motion and dragging land immediately. Pauses settle without catch-up.
    s.to(400,3000,true);assert(s.position==400 && !s.moving());
    s.to(896,3016);s.settle();assert(s.position==896 && !s.moving());
    assert(s.wheelTarget(-120,0)==896);assert(s.wheelTarget(120,UINT32_MAX)==512);
    // Easing has the same result at different frame rates.
    lt::SmoothScroll a,b;a.configure(1280,384);b.configure(1280,384);a.to(700,100);b.to(700,100);
    for(int i=1;i<=12;i++)a.advance(100+i*16);
    for(int i=1;i<=6;i++)b.advance(100+i*32);
    assert(std::abs(a.position-b.position)<.001f);
    // Final view contains six whole entries and the last control, with no blank page.
    s.to(896,4000,true);assert(int(s.position/64)==14);assert((s.position+384)/64==20);
    s.configure(200,384);assert(s.position==0 && s.target==0 && s.limit==0);
    // Desktop lists show seven complete rows at each settled stop.
    for(int count:{20,30}){
        lt::SmoothScroll rows;rows.configure(count*64,448);
        for(int delta:{-1,-30,-120,-960,120,960}){
            rows.to(rows.wheelTarget(delta,3),5000,true);
            rows.to(std::round(rows.target/64)*64,5010);finish(rows,5010);
            assert(int(rows.position)%64==0);
            assert(rows.position+448<=count*64);
        }
    }
    lt::SmoothScroll oneLine;oneLine.configure(1280,448);assert(oneLine.wheelTarget(-120,1,64)==64);assert(oneLine.wheelTarget(-120,0,64)==0);
    // Independent inspector can scroll without moving or retargeting the list.
    lt::SmoothScroll inspector;inspector.configure(1200,312);inspector.to(888,1,true);
    assert(inspector.position==888&&s.position==0);
    inspector.configure(100,312);assert(inspector.position==0&&inspector.limit==0);
    std::cout<<"PASS: wheel deltas, smooth motion, reversal, clamping, reduced motion, pause, frame-rate independence and final viewport.\n";
}
