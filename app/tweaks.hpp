#pragma once
#include "util.hpp"
#include "adaptive_power.hpp"
#include <map>
#include <iphlpapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <array>
#include <cstring>
#include "reg_tweaks.hpp"
#include <winioctl.h>
#include <setupapi.h>
namespace lt {
enum class Kind { SpiBool, SpiUi, SpiPv, Mouse, Animation, Power, Guide, Maintenance, Ping, AdaptivePower, Registry, Bundle };
struct SystemInfo;
using Planner = reg::Plan (*)(const SystemInfo &);
struct Tweak {
    int id, category, tier;
    const wchar_t *name;
    const wchar_t *detail;
    Kind kind;
    UINT get = 0, set = 0;
    DWORD value = 0;
    const wchar_t *uri = L"";
    Planner plan = nullptr;
    const wchar_t *summary = L"";
    int impact = 1;          // 0 light, 1 medium, 2 high
    bool restart = false;    // takes full effect after a restart
    bool recommended = true; // part of the one-click "Apply recommended"
    bool legacy = false;     // kept only so older backups stay restorable
    bool hidden = false;     // part of a bundle; applied and restored through it
    const int *members = nullptr; // Kind::Bundle: the tweaks applied together
    int memberCount = 0;
};
const Tweak *findTweak(int id);
struct SystemInfo {
    std::wstring cpu, gpu, os, network, storage;
    std::wstring gpuInstance, netInterface;
    std::map<int, std::wstring> tailoring, headline;
    std::map<int, bool> admin;
    unsigned cores = 0, physicalCores = 0;
    unsigned long long ram = 0;
    DWORD build = 0;
    int gpuVendor = 0; // 1 NVIDIA, 2 AMD, 3 Intel
    int ssd = -1;      // -1 unknown, 0 HDD, 1 SSD
    bool laptop = false, windowsSupported = false, elevated = false, wifi = false;
};
inline bool processElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}
inline int vendorOf(const std::wstring &id) {
    if (id.find(L"VEN_10DE") != std::wstring::npos) return 1;
    if (id.find(L"VEN_1002") != std::wstring::npos) return 2;
    if (id.find(L"VEN_8086") != std::wstring::npos) return 3;
    return 0;
}
// PnP instance of the main (preferably discrete) display adapter, for device-specific tweaks.
inline std::wstring gpuInstancePath(int preferVendor) {
    static const GUID displayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO set = SetupDiGetClassDevsW(&displayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring best, fallback;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 512, nullptr)) continue;
        std::wstring s = id;
        if (s.rfind(L"PCI\\", 0) != 0) continue;
        if (fallback.empty()) fallback = s;
        if (preferVendor && vendorOf(s) == preferVendor && best.empty()) best = s;
    }
    SetupDiDestroyDeviceInfoList(set);
    return best.empty() ? fallback : best;
}
inline void storageScan(SystemInfo &s) {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return;
    std::wstring volume = L"\\\\.\\" + std::wstring(1, windows[0]) + L":";
    HANDLE h = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &seek, sizeof(seek), &got, nullptr) &&
        got >= sizeof(seek))
        s.ssd = seek.IncursSeekPenalty ? 0 : 1;
    q.PropertyId = StorageDeviceProperty;
    std::vector<BYTE> buffer(1024);
    bool nvme = false;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buffer.data(), DWORD(buffer.size()), &got, nullptr) &&
        got >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
        nvme = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(buffer.data())->BusType == BusTypeNvme;
    CloseHandle(h);
    s.storage = s.ssd == 1 ? (nvme ? L"NVMe SSD" : L"SSD") : s.ssd == 0 ? L"Hard drive (HDD)" : L"Unknown drive";
}
inline unsigned physicalCoreCount() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (!length) return 0;
    std::vector<BYTE> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data()), &length))
        return 0;
    unsigned count = 0;
    for (DWORD offset = 0; offset < length;) {
        auto item = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
        if (!item->Size) break;
        ++count;
        offset += item->Size;
    }
    return count;
}
inline std::wstring trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}
inline SystemInfo systemScan() {
    SystemInfo s;
    s.cpu = trim(regString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      L"ProcessorNameString"));
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    s.cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!s.cores) s.cores = si.dwNumberOfProcessors;
    s.physicalCores = physicalCoreCount();
    if (!s.physicalCores) s.physicalCores = s.cores;
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    GlobalMemoryStatusEx(&m);
    ULONGLONG installedKB = 0;
    s.ram = GetPhysicallyInstalledSystemMemory(&installedKB) ? installedKB / (1024ull * 1024)
                                                             : m.ullTotalPhys / (1024ull * 1024 * 1024);
    DISPLAY_DEVICEW d{};
    d.cb = sizeof(d);
    std::wstring primaryName, primaryId;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); ++i) {
        std::wstring id = d.DeviceID, name = d.DeviceString;
        if ((d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) && primaryName.empty()) { primaryName = name; primaryId = id; }
        int v = vendorOf(id);
        // Prefer a discrete NVIDIA/AMD adapter on hybrid laptops.
        if ((v == 1 || v == 2) && s.gpuVendor != 1 && s.gpuVendor != 2) { s.gpu = name; s.gpuVendor = v; }
        d.cb = sizeof(d);
    }
    if (s.gpu.empty()) { s.gpu = primaryName; s.gpuVendor = vendorOf(primaryId); }
    SYSTEM_POWER_STATUS p{};
    if (GetSystemPowerStatus(&p))
        s.laptop = p.BatteryFlag != 128 && p.BatteryFlag != 255;
    typedef LONG(WINAPI * VerFn)(OSVERSIONINFOW *);
    VerFn fn = nullptr;
    auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    static_assert(sizeof(fn) == sizeof(address));
    memcpy(&fn, &address, sizeof(fn));
    OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    if (fn && fn(&ver) == 0) {
        s.build = ver.dwBuildNumber;
        s.windowsSupported = ver.dwMajorVersion == 10 && ver.dwBuildNumber >= 19041;
        s.os = std::wstring(ver.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10") + L" · build " +
               std::to_wstring(s.build);
    }
    ULONG n = 16000;
    std::vector<BYTE> adapters(n);
    ULONG rc = GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
        (IP_ADAPTER_ADDRESSES *)adapters.data(), &n);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        adapters.resize(n);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, (IP_ADAPTER_ADDRESSES *)adapters.data(), &n);
    }
    s.network = L"No active adapter";
    if (rc == NO_ERROR)
        for (auto a = (IP_ADAPTER_ADDRESSES *)adapters.data(); a; a = a->Next)
            if (a->OperStatus == IfOperStatusUp && a->FirstGatewayAddress &&
                (a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211)) {
                s.wifi = a->IfType == IF_TYPE_IEEE80211;
                s.network = s.wifi ? L"Wi-Fi" : L"Ethernet";
                s.netInterface = wide(a->AdapterName);
                break;
            }
    storageScan(s);
    s.gpuInstance = gpuInstancePath(s.gpuVendor);
    s.elevated = processElevated();
    return s;
}
struct Snapshot {
    DWORD magic = 0x4c544231;
    DWORD version = 2;
    DWORD id = 0;
    DWORD size = 0;
    BYTE data[64]{};
    BYTE checksum[32]{};
};
inline std::wstring backupPath(const Tweak &t) {
    return dataDir() + L"\\backups\\" + std::to_wstring(t.id) + L".ltb";
}
inline bool hasBackup(const Tweak &t) {
    if (t.kind == Kind::Bundle) {
        for (int i = 0; i < t.memberCount; ++i)
            if (auto m = findTweak(t.members[i]); m && m->kind != Kind::Bundle && hasBackup(*m)) return true;
        return false;
    }
    if (t.kind == Kind::Registry) return reg::hasBackup(t.id);
    return t.kind==Kind::AdaptivePower ? adaptive::has(t.id) : std::filesystem::exists(backupPath(t));
}
inline Snapshot readState(const Tweak &t) {
    Snapshot s;
    s.id = t.id;
    s.size = sizeof(DWORD);
    BOOL ok = FALSE;
    switch (t.kind) {
    case Kind::Power: {
        GUID *g = nullptr;
        if (PowerGetActiveScheme(nullptr, &g) != ERROR_SUCCESS)
            throw std::runtime_error("Power profile is not accessible");
        s.size = sizeof(GUID);
        memcpy(s.data, g, s.size);
        LocalFree(g);
        return s;
    }
    case Kind::Mouse:
        s.size = 3 * sizeof(int);
        ok = SystemParametersInfoW(t.get, 0, s.data, 0);
        break;
    case Kind::Animation: {
        ANIMATIONINFO a{sizeof(a), 0};
        ok = SystemParametersInfoW(t.get, sizeof(a), &a, 0);
        memcpy(s.data, &a, sizeof(a));
        s.size = sizeof(a);
        break;
    }
    case Kind::SpiBool:
    case Kind::SpiUi:
    case Kind::SpiPv:
        ok = SystemParametersInfoW(t.get, 0, s.data, 0);
        break;
    default:
        throw std::runtime_error("This is a guided action");
    }
    if (!ok)
        throw std::runtime_error("This setting is unavailable on this Windows configuration");
    return s;
}
inline Snapshot desired(const Tweak &t) {
    Snapshot s;
    s.id = t.id;
    s.size = sizeof(DWORD);
    memcpy(s.data, &t.value, 4);
    if (t.kind == Kind::Mouse) {
        s.size = 12;
        memset(s.data, 0, 12);
    }
    if (t.kind == Kind::Animation) {
        ANIMATIONINFO a{sizeof(a), FALSE};
        s.size = sizeof(a);
        memcpy(s.data, &a, sizeof(a));
    }
    if (t.kind == Kind::Power) {
        s.size = sizeof(GUID);
        const GUID balanced = {
            0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
        const GUID high = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        memcpy(s.data, t.value ? &high : &balanced, sizeof(GUID));
    }
    return s;
}
inline bool same(const Snapshot &a, const Snapshot &b) {
    return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}
inline void seal(Snapshot &s) {
    auto h = sha(std::string(reinterpret_cast<const char *>(&s), offsetof(Snapshot, checksum)));
    memcpy(s.checksum, h.data(), 32);
}
inline Snapshot savedState(const Tweak &t) {
    auto raw = readFile(backupPath(t));
    if (raw.size() != sizeof(Snapshot))
        throw std::runtime_error("Backup is incomplete");
    Snapshot s;
    memcpy(&s, raw.data(), sizeof(s));
    auto h = sha(raw.substr(0, offsetof(Snapshot, checksum)));
    if (s.magic != 0x4c544231 || s.version != 2 || s.id != (DWORD)t.id || s.size != desired(t).size ||
        s.size > 64 || memcmp(s.checksum, h.data(), 32) != 0)
        throw std::runtime_error("Backup validation failed; original file has been preserved");
    return s;
}
inline void writeState(const Tweak &t, const Snapshot &s) {
    DWORD v = 0;
    memcpy(&v, s.data, 4);
    UINT flags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    BOOL ok = FALSE;
    switch (t.kind) {
    case Kind::Power:
        if (PowerSetActiveScheme(nullptr, (const GUID *)s.data) != ERROR_SUCCESS)
            throw std::runtime_error(
                "Power plan is unavailable or managed by your device administrator");
        return;
    case Kind::SpiUi:
        ok = SystemParametersInfoW(t.set, v, nullptr, flags);
        break;
    case Kind::SpiPv:
    case Kind::SpiBool:
        ok = SystemParametersInfoW(t.set, 0, (PVOID)(ULONG_PTR)v, flags);
        break;
    case Kind::Mouse:
        ok = SystemParametersInfoW(t.set, 0, (PVOID)s.data, flags);
        break;
    case Kind::Animation:
        ok = SystemParametersInfoW(t.set, sizeof(ANIMATIONINFO), (PVOID)s.data, flags);
        break;
    default:
        throw std::runtime_error("Unsupported automatic action");
    }
    if (!ok)
        throw std::runtime_error("Windows rejected the setting; your policy may control it");
}
inline reg::Plan planFor(const Tweak &t, const SystemInfo &pc) {
    if (!t.plan) throw std::runtime_error("This tweak has no plan");
    return t.plan(pc);
}
inline void apply(const Tweak &t, const SystemInfo &pc) {
    if (t.kind == Kind::Bundle) {
        // Apply every part this PC supports; each part keeps its own original value for Undo.
        int done = 0;
        std::string firstError;
        for (int i = 0; i < t.memberCount; ++i) {
            auto m = findTweak(t.members[i]);
            if (!m || m->kind == Kind::Bundle) continue;
            try { apply(*m, pc); ++done; }
            catch (const std::exception &e) { if (firstError.empty()) firstError = e.what(); }
        }
        if (!done) throw std::runtime_error(firstError.empty() ? "Nothing could be changed on this PC" : firstError);
        return;
    }
    if (t.kind == Kind::Registry) { reg::apply(t.id, planFor(t, pc)); return; }
    if(t.kind==Kind::AdaptivePower){adaptive::apply(t.id,t.value,t.name);return;}
    auto old = readState(t);
    auto target = desired(t);
    if (same(old, target))
        return;
    auto path = backupPath(t);
    // Snapshot must be durable before the first mutation. Never replace the original.
    if (!hasBackup(t)) {
        seal(old);
        durableWrite(path, &old, sizeof(old), true);
    } else {
        savedState(t); // Refuse mutation if an existing original backup is corrupt.
    }
    writeState(t, target);
    auto actual = readState(t);
    if (!same(actual, target))
        throw std::runtime_error(
            "Windows did not retain the requested value; the original backup is available");
}
inline void restore(const Tweak &t) {
    if (t.kind == Kind::Bundle) {
        std::string firstError;
        for (int i = t.memberCount; i-- > 0;) {
            auto m = findTweak(t.members[i]);
            if (!m || m->kind == Kind::Bundle || !hasBackup(*m)) continue;
            try { restore(*m); } catch (const std::exception &e) { if (firstError.empty()) firstError = e.what(); }
        }
        if (!firstError.empty()) throw std::runtime_error(firstError);
        return;
    }
    if (t.kind == Kind::Registry) { reg::restore(t.id); return; }
    if(t.kind==Kind::AdaptivePower){adaptive::restore(t.id);return;}
    auto s = savedState(t);
    auto expect = desired(t);
    // Refuse to overwrite a later change from another app/user.
    auto current = readState(t);
    if (!same(current, expect) && !same(current, s))
        throw std::runtime_error("This setting changed outside LunarTweaks. Review it before restoring");
    writeState(t, s);
    if (!same(readState(t), s))
        throw std::runtime_error("Restore verification failed");
    if (!DeleteFileW(backupPath(t).c_str()))
        throw std::runtime_error("Restored, but the backup file could not be removed");
}
inline bool automatic(const Tweak &t) {
    return t.kind != Kind::Guide && t.kind != Kind::Maintenance && t.kind != Kind::Ping;
}
inline void openGuide(const Tweak &t, HWND hwnd) {
    if (MessageBoxW(hwnd, t.detail, t.name, MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
        auto r = ShellExecuteW(hwnd, L"open", t.uri, nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            throw std::runtime_error("Windows could not open that settings page");
    }
}
inline std::wstring systemUtility(const wchar_t *name, const wchar_t *arguments, bool requireSuccess) {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring executable = std::wstring(system) + L"\\" + name;
    std::wstring cmd = L"\"" + executable + L"\" " + arguments;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        throw std::runtime_error("Could not start the Windows utility");
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(rd);
        CloseHandle(wr);
        throw std::runtime_error("Could not open the process input handle");
    }
    si.hStdInput = input;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    CloseHandle(input);
    if (!ok) {
        CloseHandle(rd);
        throw std::runtime_error("The Windows utility is unavailable");
    }
    std::string output;
    char buffer[1024];
    DWORD got;
    while (ReadFile(rd, buffer, sizeof(buffer), &got, nullptr) && got) {
        if (output.size() + got <= 32768)
            output.append(buffer, got);
    }
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    int n = MultiByteToWideChar(CP_OEMCP, 0, output.data(), (int)output.size(), nullptr, 0);
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_OEMCP, 0, output.data(), (int)output.size(), result.data(), n);
    if (requireSuccess && exitCode != 0)
        throw std::runtime_error(
            "Windows could not complete DNS maintenance; elevation or a policy change may be required");
    return result;
}
inline std::wstring pingTest() {
    return L"ICMP diagnostic to Cloudflare 1.1.1.1. This is not your game-server latency.\n\n" +
           systemUtility(L"ping.exe", L"-n 6 -w 1200 1.1.1.1", false);
}
inline void flushDns() {
    systemUtility(L"ipconfig.exe", L"/flushdns", true);
}
constexpr int categoryCount = 6;
const std::array<const wchar_t *, categoryCount> categories = {L"Windows Essentials", L"Input & Latency", L"CPU & Power",
                                                              L"GPU & Gaming", L"Memory & Storage", L"Network"};
const std::vector<Tweak> &catalog();
const Tweak *findTweak(int id);
constexpr int kMaxTweakId = 400;
} // namespace lt
