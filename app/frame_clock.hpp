#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
namespace lt {
// Only posts a coalesced notification. GDI and all UI state stay on the UI thread.
class FrameClock {
    HANDLE halt_=nullptr,change_=nullptr,timer_=nullptr;
    std::thread thread_;
    std::atomic<int> period_{0};
    std::atomic_bool pending_{false};
public:
    ~FrameClock(){stop();}
    bool running()const{return thread_.joinable();}
    void acknowledge(){pending_=false;}
    void interval(int ms){if(period_.exchange(ms)!=ms&&change_)SetEvent(change_);}
    void start(HWND hwnd,UINT message){
        halt_=CreateEventW(nullptr,TRUE,FALSE,nullptr);change_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        timer_=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
        if(!timer_)timer_=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!halt_||!change_||!timer_){stop();return;}
        try{thread_=std::thread([this,hwnd,message]{
            HANDLE handles[]={halt_,change_,timer_};
            for(;;){
                DWORD event=WaitForMultipleObjects(3,handles,FALSE,INFINITE);
                if(event==WAIT_OBJECT_0||event==WAIT_FAILED)break;
                if(event==WAIT_OBJECT_0+1){
                    CancelWaitableTimer(timer_);int period=period_.load();
                    if(period){LARGE_INTEGER due;due.QuadPart=-LONGLONG(period)*10000;SetWaitableTimer(timer_,&due,period,nullptr,nullptr,FALSE);}
                }else if(period_>0&&!pending_.exchange(true)){
                    if(!PostMessageW(hwnd,message,0,0))pending_=false;
                }
            }
        });}catch(...){stop();}
    }
    void stop(){
        if(halt_)SetEvent(halt_);
        if(thread_.joinable())thread_.join();
        for(auto h:{halt_,change_,timer_})if(h)CloseHandle(h);
        halt_=change_=timer_=nullptr;period_=0;pending_=false;
    }
};
}
