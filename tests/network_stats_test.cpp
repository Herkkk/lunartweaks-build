#include "../app/network_stats.hpp"
#include <cassert>
#include <cmath>
int main() {
    lt::NetworkStats s;
    assert(s.summary() == L"No network probes completed.");
    s.record(true, 10);
    s.record(true, 30);
    s.record(false);
    s.record(true, 20);
    assert(s.sent == 4 && s.received == 3 && s.pairs == 1);
    assert(s.minimum == 10 && s.maximum == 30 && s.average() == 20);
    assert(s.loss() == 25 && s.jitter() == 20);
    s.record(true, 20);
    assert(s.pairs == 2 && s.jitter() == 10);
    lt::NetworkStats blocked;
    blocked.record(false);
    blocked.record(false);
    assert(blocked.summary().find(L"does not prove an outage") != std::wstring::npos);
    assert(blocked.detail().find(L"Minimum") == std::wstring::npos);
    lt::NetworkStats isolated;
    isolated.record(true, 0);
    isolated.record(false);
    isolated.record(true, 50);
    assert(isolated.average() == 25 && isolated.pairs == 0);
    assert(isolated.detail().find(L"Jitter unavailable") != std::wstring::npos);
}
