#pragma once
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
namespace lt {
// A missed echo breaks adjacency: jitter never bridges a lost sample.
struct NetworkStats {
    unsigned sent = 0, received = 0, pairs = 0;
    double total = 0, minimum = 0, maximum = 0, variation = 0, previous = 0;
    bool adjacent = false;
    void record(bool success, double milliseconds = 0) {
        ++sent;
        if (!success) {
            adjacent = false;
            return;
        }
        if (received == 0)
            minimum = maximum = milliseconds;
        minimum = std::min(minimum, milliseconds);
        maximum = std::max(maximum, milliseconds);
        if (adjacent) {
            variation += std::abs(milliseconds - previous);
            ++pairs;
        }
        previous = milliseconds;
        adjacent = true;
        total += milliseconds;
        ++received;
    }
    double average() const {
        return received ? total / received : 0;
    }
    double loss() const {
        return sent ? 100.0 * (sent - received) / sent : 0;
    }
    double jitter() const {
        return pairs ? variation / pairs : 0;
    }
    std::wstring summary() const {
        if (!sent)
            return L"No network probes completed.";
        if (!received)
            return std::to_wstring(sent) +
                   L" probes: no echo replies. ICMP may be blocked; this does not prove an outage.";
        std::wostringstream out;
        out << std::fixed << std::setprecision(1) << received << L"/" << sent << L" replies | avg "
            << average() << L" ms | no reply " << loss() << L"%";
        return out.str();
    }
    std::wstring detail() const {
        std::wostringstream out;
        out << L"Target: Cloudflare 1.1.1.1 (IPv4 ICMP)\nThis is not your game-server latency.\n\n"
            << summary();
        if (received) {
            out << std::fixed << std::setprecision(1) << L"\nMinimum: " << minimum << L" ms\nMaximum: "
                << maximum << L" ms\n";
            if (pairs)
                out << L"Mean absolute variation between consecutive replies: " << jitter() << L" ms";
            else
                out << L"Jitter unavailable: no consecutive successful replies.";
        }
        out << L"\n\nA missing reply can mean filtering, rate limiting or packet loss. Compare repeated "
               L"measurements under similar conditions.";
        return out.str();
    }
};
} // namespace lt
