#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lt {
// Pixel positions, independent of wheel-event frequency and paint timing.
struct SmoothScroll {
    float position = 0, target = 0, limit = 0, viewportSize = 384;
    uint64_t lastTick = 0;
    bool moving() const { return std::abs(target - position) > .05f; }
    void reset() { position = target = 0; lastTick = 0; }
    void configure(float content, float viewport) {
        viewportSize=viewport;
        limit = std::max(0.0f, content - viewport);
        position = std::clamp(position, 0.0f, limit);
        target = std::clamp(target, 0.0f, limit);
    }
    void settle() { position = target; lastTick = 0; }
    bool advance(uint64_t now) {
        if (!moving()) { settle(); return false; }
        float dt = lastTick && now >= lastTick ? float(now - lastTick) / 1000 : 0;
        lastTick = now;
        dt = std::min(dt, .05f);
        position += (target - position) * (1 - std::exp(-dt / .080f));
        if (!moving()) settle();
        return true;
    }
    bool to(float value, uint64_t now, bool immediate = false) {
        advance(now); // Retarget from the displayed position, never an old screenshot.
        float next = std::clamp(value, 0.0f, limit);
        bool changed = next != target || (immediate && position != next);
        target = next;
        lastTick = now;
        if (immediate) settle();
        return changed;
    }
    float wheelTarget(int delta, unsigned lines, float minimumStep = 0) const {
        if(!lines)return target;
        float distance = lines == UINT32_MAX ? viewportSize : std::max(minimumStep, std::min(lines, 20u) * 24.0f);
        return target - float(delta) * distance / 120;
    }

};
} // namespace lt
