#include "../app/ui_audio.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
int main(int argc, char **argv) {
    const char *names[] = {"click", "complete", "error"};
    int n = 0;
    for (auto cue : {lt::Cue::Click, lt::Cue::Complete, lt::Cue::Error}) {
        auto wav = lt::cueWave(cue);
        auto read = [&](int at) { return unsigned(wav[at]) | (unsigned(wav[at + 1]) << 8) | (unsigned(wav[at + 2]) << 16) | (unsigned(wav[at + 3]) << 24); };
        assert(!std::memcmp(wav.data(), "RIFF", 4) && !std::memcmp(wav.data() + 8, "WAVE", 4));
        assert(read(4) == wav.size() - 8 && read(40) == wav.size() - 44 && read(24) == lt::kCueRate);
        assert(read(40) > 0 && read(40) <= lt::kCueRate * 2 * 6 / 10); // Every cue is shorter than 600 ms.
        int peak = 0;
        for (size_t i = 44; i < wav.size(); i += 2) { int v = int16_t(wav[i] | (unsigned(wav[i + 1]) << 8)); peak = std::max(peak, std::abs(v)); }
        assert(peak > 0 && peak <= 4300); // Never above ~13% of full scale.
        assert(wav[44] == 0 && wav[45] == 0 && wav[wav.size() - 1] == 0 && wav[wav.size() - 2] == 0);
        if (argc > 1) { char path[256]; snprintf(path, sizeof path, "%s/%s.wav", argv[1], names[n]); FILE *f = fopen(path, "wb"); fwrite(wav.data(), 1, wav.size(), f); fclose(f); }
        ++n;
    }
    std::puts("PASS: cue headers, durations, levels and silent edges.");
}
