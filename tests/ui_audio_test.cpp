#include "../app/ui_audio.hpp"
#include <cassert>
#include <cstring>
int main(){
    for(auto cue:{lt::Cue::Click,lt::Cue::Complete,lt::Cue::Error}){
        auto wav=lt::cueWave(cue);
        auto read=[&](int at){return unsigned(wav[at])|(unsigned(wav[at+1])<<8)|(unsigned(wav[at+2])<<16)|(unsigned(wav[at+3])<<24);};
        assert(!std::memcmp(wav.data(),"RIFF",4)&&!std::memcmp(wav.data()+8,"WAVE",4));
        assert(read(4)==wav.size()-8&&read(40)==wav.size()-44&&read(24)==22050);
        assert(read(40)>0&&read(40)<=22050*2/5); // Each cue <=200ms.
        int peak=0;
        for(size_t i=44;i<wav.size();i+=2){int value=int16_t(wav[i]|(unsigned(wav[i+1])<<8));peak=std::max(peak,std::abs(value));}
        if(cue==lt::Cue::Click){assert(read(40)==792*2);assert(peak==1450);}
        assert(peak>0&&peak<=1638); // Never above 5% PCM amplitude.
        assert(wav[44]==0&&wav[45]==0&&wav[wav.size()-1]==0&&wav[wav.size()-2]==0);
    }
}
