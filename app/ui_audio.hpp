#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace lt {
enum class Cue { Click, Complete, Error };
// Original, low-amplitude cues. Finite PCM buffers; no files or background loops.
inline std::vector<unsigned char> cueWave(Cue cue) {
    constexpr unsigned rate=22050;
    unsigned count=cue==Cue::Click?792:cue==Cue::Complete?3969:2205;
    std::vector<unsigned char> out(44+count*2);
    auto word=[&](unsigned at,unsigned value,unsigned bytes){for(unsigned i=0;i<bytes;++i)out[at+i]=static_cast<unsigned char>(value>>(8*i));};
    auto tag=[&](unsigned at,const char*s){for(int i=0;i<4;++i)out[at+i]=s[i];};
    tag(0,"RIFF");word(4,out.size()-8,4);tag(8,"WAVE");tag(12,"fmt ");word(16,16,4);
    word(20,1,2);word(22,1,2);word(24,rate,4);word(28,rate*2,4);word(32,2,2);word(34,16,2);tag(36,"data");word(40,count*2,4);
    uint32_t noise=0x4c554e41;double filtered=0;
    for(unsigned i=0;i<count;++i){
        double t=double(i)/rate,u=double(i)/(count-1);
        noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;
        double random=(double(noise)/UINT32_MAX)*2-1;
        filtered+=.32*(random-filtered);
        // Brief textured impact and a damped low body, with no sustained click tone.
        double envelope=std::min(1.0,t/.0015)*std::pow(1-u,2);
        double sample;
        if(cue==Cue::Click){
            sample=.65*filtered*std::exp(-t*120)+.35*std::sin(6.283185307179586*(210*t-800*t*t))*std::exp(-t*90);
            sample=std::clamp(sample*2.2,-1.0,1.0);
        }else if(cue==Cue::Complete){
            envelope=std::min(1.0,t/.006)*std::pow(1-u,2);
            sample=.55*std::sin(6.283185307179586*520*t)+.25*std::sin(6.283185307179586*780*t);
        }else sample=.7*std::sin(6.283185307179586*260*t)*std::exp(-t*12);
        int16_t value=static_cast<int16_t>(sample*envelope*1638);
        word(44+i*2,static_cast<uint16_t>(value),2);
    }
    if(cue==Cue::Click){
        int peak=1;for(unsigned i=0;i<count;++i){int v=int16_t(out[44+i*2]|(unsigned(out[45+i*2])<<8));peak=std::max(peak,std::abs(v));}
        for(unsigned i=0;i<count;++i){int16_t v=int16_t(out[44+i*2]|(unsigned(out[45+i*2])<<8));
            word(44+i*2,static_cast<uint16_t>(int16_t(int(v)*1450/peak)),2);}
    }
    return out;
}
}
