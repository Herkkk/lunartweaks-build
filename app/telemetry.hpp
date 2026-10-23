#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace lt {
// Windows kernel time includes idle time. Only deltas between valid samples are useful.
struct CpuCounter {
    bool primed = false;
    uint64_t idle = 0, kernel = 0, user = 0;
    std::optional<float> update(uint64_t i, uint64_t k, uint64_t u) {
        std::optional<float> result;
        if (primed && i >= idle && k >= kernel && u >= user) {
            double total = double(k - kernel) + double(u - user);
            double resting = double(i - idle);
            if (total > 0 && resting <= total)
                result = float(100.0 * (total - resting) / total);
        }
        idle = i;
        kernel = k;
        user = u;
        primed = true;
        return result;
    }
};
struct ActivitySample {
    float cpu = -1, memory = -1;
    double seconds = 0;
};
struct ActivityHistory {
    std::array<ActivitySample, 61> samples{};
    size_t count = 0;
    void clear() {
        count = 0;
    }
    void push(ActivitySample s) {
        if (count && s.seconds <= samples[count - 1].seconds)
            return;
        if (count == samples.size()) {
            std::move(samples.begin() + 1, samples.end(), samples.begin());
            --count;
        }
        samples[count++] = s;
    }
};
struct PlotPoint {
    float value = 0;
    double seconds = 0;
    bool connected = false;
};
struct ActivityPlot {
    std::array<PlotPoint, 61> points{};
    size_t count = 0;
    bool hasSegment = false;
};
// Keep valid history on both sides of missing readings, without bridging gaps.
inline ActivityPlot plotHistory(const ActivityHistory &history, bool memory, double now) {
    ActivityPlot plot;
    bool previousValid = false;
    double previousTime = 0;
    for (size_t i = 0; i < history.count; ++i) {
        const auto &sample = history.samples[i];
        float value = memory ? sample.memory : sample.cpu;
        if (!std::isfinite(value) || value < 0 || value > 100 ||
            !std::isfinite(sample.seconds) || sample.seconds < now - 60 || sample.seconds > now) {
            previousValid = false;
            continue;
        }
        bool connected = previousValid && sample.seconds > previousTime &&
                         sample.seconds - previousTime <= 2.5;
        plot.points[plot.count++] = {value, sample.seconds, connected};
        plot.hasSegment = plot.hasSegment || connected;
        previousValid = true;
        previousTime = sample.seconds;
    }
    return plot;
}
inline float easeOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
} // namespace lt
