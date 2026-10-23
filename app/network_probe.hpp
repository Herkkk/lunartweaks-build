#pragma once
#include "util.hpp"
#include "network_stats.hpp"
#include <iphlpapi.h>
#include <icmpapi.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
namespace lt {
inline NetworkStats networkBaseline(std::atomic_bool &skip,
                                    const std::function<void(unsigned)> &progress) {
    struct EchoHandle {
        HANDLE value = IcmpCreateFile();
        ~EchoHandle() {
            if (value != INVALID_HANDLE_VALUE)
                IcmpCloseHandle(value);
        }
    } echo;
    if (echo.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("IPv4 connectivity measurement is unavailable on this PC.");
    char payload[] = "LunarTweaks connectivity baseline";
    std::vector<BYTE> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8);
    NetworkStats stats;
    auto started = std::chrono::steady_clock::now();
    for (unsigned sample = 0; sample < 30 && !skip.load(); ++sample) {
        // 1.1.1.1 has the same byte order in host and network representation.
        DWORD count = IcmpSendEcho(echo.value, 0x01010101, payload, sizeof(payload), nullptr,
                                   reply.data(), static_cast<DWORD>(reply.size()), 800);
        auto response = reinterpret_cast<const ICMP_ECHO_REPLY *>(reply.data());
        bool success = count && response->Status == IP_SUCCESS && response->Address == 0x01010101;
        stats.record(success, success ? response->RoundTripTime : 0);
        progress(stats.sent);
        auto next = started + std::chrono::seconds(sample + 1);
        while (!skip.load() && std::chrono::steady_clock::now() < next)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return stats;
}
} // namespace lt
