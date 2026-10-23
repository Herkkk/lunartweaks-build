#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace lt {
enum class Cue { Click, Complete, Error };
// Soft, musical interface cues synthesised in memory (no files, no loops).
//  Click    — a 28 ms rounded "tick": pitched sine blip with a fast exponential decay.
//  Complete — a two-note glass chime (E6 → B6) with gentle bell partials.
//  Error    — two soft, low, descending notes (A4 → F4). Never harsh.
// All cues start and end on silence and stay well below full scale.
constexpr unsigned kCueRate = 44100;
inline std::vector<unsigned char> cueWave(Cue cue) {
    constexpr double tau = 6.283185307179586;
    const double seconds = cue == Cue::Click ? .028 : cue == Cue::Complete ? .52 : .36;
    const unsigned count = unsigned(seconds * kCueRate);
    std::vector<unsigned char> out(44 + count * 2);
    auto word = [&](unsigned at, unsigned value, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) out[at + i] = static_cast<unsigned char>(value >> (8 * i)); };
    auto tag = [&](unsigned at, const char *s) { for (int i = 0; i < 4; ++i) out[at + i] = s[i]; };
    tag(0, "RIFF"); word(4, unsigned(out.size() - 8), 4); tag(8, "WAVE"); tag(12, "fmt "); word(16, 16, 4);
    word(20, 1, 2); word(22, 1, 2); word(24, kCueRate, 4); word(28, kCueRate * 2, 4); word(32, 2, 2); word(34, 16, 2);
    tag(36, "data"); word(40, count * 2, 4);
    // A note with a smooth raised-cosine attack and exponential decay, plus soft bell partials.
    auto note = [&](double t, double start, double freq, double decay, double attack) {
        double x = t - start;
        if (x < 0) return 0.0;
        double env = (x < attack ? .5 - .5 * std::cos(3.141592653589793 * x / attack) : 1.0) * std::exp(-x * decay);
        return env * (std::sin(tau * freq * x) + .22 * std::sin(tau * freq * 2.0 * x) * std::exp(-x * decay * 1.6) +
                      .08 * std::sin(tau * freq * 3.01 * x) * std::exp(-x * decay * 2.4));
    };
    std::vector<double> samples(count);
    double peak = 1e-9;
    for (unsigned i = 0; i < count; ++i) {
        double t = double(i) / kCueRate, s = 0;
        if (cue == Cue::Click) {
            // Pitch glides 1500 → 1100 Hz for a rounded, non-mechanical tick.
            double phase = tau * (1500 * t - 0.5 * 400 / .028 * t * t);
            double env = (t < .0015 ? .5 - .5 * std::cos(3.141592653589793 * t / .0015) : 1.0) * std::exp(-t * 180);
            s = env * std::sin(phase);
        } else if (cue == Cue::Complete) {
            s = .62 * note(t, 0.0, 1318.5, 9.0, .004) + .55 * note(t, .085, 1975.5, 7.5, .004);
        } else {
            s = .65 * note(t, 0.0, 440.0, 11.0, .008) + .6 * note(t, .11, 349.2, 10.0, .008);
        }
        samples[i] = s;
        peak = std::max(peak, std::abs(s));
    }
    // Loudness: clicks are very quiet, the chime a little louder, errors in between.
    const double level = cue == Cue::Click ? 2600 : cue == Cue::Complete ? 4200 : 3400;
    for (unsigned i = 0; i < count; ++i) {
        double fade = i + 1 >= count ? 0.0 : std::min(1.0, double(count - 1 - i) / (kCueRate * .012));
        double head = i == 0 ? 0.0 : 1.0;
        auto value = static_cast<int16_t>(std::lround(samples[i] / peak * level * fade * head));
        word(44 + i * 2, static_cast<uint16_t>(value), 2);
    }
    return out;
}
} // namespace lt
