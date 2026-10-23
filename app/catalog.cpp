// LunarTweaks 2.0 catalog.
// Free: 19 standard Windows tweaks. Pro: 27 performance tweaks whose values are computed from
// the scanned hardware (cores, RAM, storage type, GPU vendor/device, active adapter, power source,
// Windows build). Every automatic change is captured first and can be undone one by one.
#include "tweaks.hpp"
namespace lt {
namespace {
using reg::Machine;
using reg::Plan;
using reg::Unavailable;
using reg::User;
using reg::dword;
using reg::string;

const wchar_t *kMultimedia = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
const wchar_t *kGames = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
const wchar_t *kServices = L"SYSTEM\\CurrentControlSet\\Services\\";
const wchar_t *kContent = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";

std::wstring threadsText(const SystemInfo &pc) {
    return std::to_wstring(pc.physicalCores) + L" cores / " + std::to_wstring(pc.cores) + L" threads";
}
std::wstring cpuName(const SystemInfo &pc) { return pc.cpu.empty() ? L"your CPU" : pc.cpu; }
std::wstring gpuName(const SystemInfo &pc) { return pc.gpu.empty() ? L"your GPU" : pc.gpu; }
bool serviceExists(const wchar_t *name) {
    return reg::read(Machine, std::wstring(kServices) + name, L"Start").exists;
}
Plan service(const wchar_t *name, DWORD start) {
    if (!serviceExists(name)) throw Unavailable(std::wstring(name) + L" is not installed on this PC.");
    return {{dword(Machine, std::wstring(kServices) + name, L"Start", start)}, L"", L""};
}

// ---------------- Free · Windows essentials ----------------
Plan instantMenus(const SystemInfo &) {
    return {{string(User, L"Control Panel\\Desktop", L"MenuShowDelay", L"0")}, L"Menus open instantly", L""};
}
Plan taskbarAnimations(const SystemInfo &) {
    return {{dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAnimations", 0)}, L"", L""};
}
Plan transparencyOff(const SystemInfo &) {
    return {{dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", 0)},
            L"Solid taskbar and Start", L""};
}
Plan startupDelay(const SystemInfo &) {
    const wchar_t *k = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize";
    return {{dword(User, k, L"StartupDelayInMSec", 0), dword(User, k, L"WaitForIdleState", 0)},
            L"Startup apps launch without the built-in delay", L""};
}
Plan backgroundApps(const SystemInfo &pc) {
    Plan p{{dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", 1)},
           L"Store apps stop running in the background", L""};
    if (pc.build >= 22000)
        p.writes.push_back(dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BackgroundAppGlobalToggle", 0));
    return p;
}
Plan suggestionsOff(const SystemInfo &) {
    Plan p{{}, L"No ads, tips or silent app installs", L""};
    for (auto name : {L"SubscribedContent-338388Enabled", L"SubscribedContent-338389Enabled",
                      L"SubscribedContent-353694Enabled", L"SubscribedContent-353696Enabled",
                      L"SystemPaneSuggestionsEnabled", L"SilentInstalledAppsEnabled", L"SoftLandingEnabled"})
        p.writes.push_back(dword(User, kContent, name, 0));
    return p;
}
Plan advertisingId(const SystemInfo &) {
    return {{dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0)},
            L"Apps can no longer profile you for ads", L""};
}
Plan webSearch(const SystemInfo &) {
    return {{dword(User, L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"DisableSearchBoxSuggestions", 1),
             dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", 0)},
            L"Start search stays local and fast", L""};
}
Plan diagnosticData(const SystemInfo &) {
    return {{dword(Machine, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 0),
             dword(Machine, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"DoNotShowFeedbackNotifications", 1)},
            L"Lowest diagnostic data level your edition allows", L""};
}
Plan telemetryService(const SystemInfo &) {
    Plan p{{}, L"Telemetry services stop starting with Windows", L""};
    for (auto name : {L"DiagTrack", L"dmwappushservice"})
        if (serviceExists(name)) p.writes.push_back(dword(Machine, std::wstring(kServices) + name, L"Start", 4));
    if (p.writes.empty()) throw Unavailable(L"The telemetry services are not present on this edition of Windows.");
    return p;
}
Plan gameMode(const SystemInfo &) {
    return {{dword(User, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1),
             dword(User, L"Software\\Microsoft\\GameBar", L"AllowAutoGameMode", 1)},
            L"Windows prioritises the game you are playing", L""};
}
Plan recordingOff(const SystemInfo &) {
    return {{dword(User, L"System\\GameConfigStore", L"GameDVR_Enabled", 0),
             dword(User, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", 0)},
            L"No hidden background capture while you play", L""};
}
Plan fastStartupOff(const SystemInfo &) {
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", 0)},
            L"Every boot starts a fresh kernel and drivers", L""};
}

// ---------------- Pro · performance, matched to this PC ----------------
Plan foregroundBoost(const SystemInfo &pc) {
    // 0x26: short, variable quanta with a 3:1 foreground boost. Best fit for games and
    // interactive apps on every modern desktop/laptop CPU.
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation", 0x26)},
            L"Foreground game gets 3x CPU time slices on " + std::to_wstring(pc.cores) + L" threads",
            L"Short, variable scheduler quanta with a 3:1 foreground boost (0x26). Your " + cpuName(pc) + L" (" +
                threadsText(pc) + L") switches to the active game faster."};
}
Plan timerResolution(const SystemInfo &pc) {
    if (pc.build < 22000)
        throw Unavailable(L"Windows 10 build " + std::to_wstring(pc.build) +
                          L" does not support the global timer request switch. It is a Windows 11 kernel option.");
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel", L"GlobalTimerResolutionRequests", 1)},
            L"High-resolution timer honoured system-wide on build " + std::to_wstring(pc.build),
            L"Windows 11 ignores a game's timer-resolution request when its window is minimised or covered. This restores "
            L"system-wide behaviour so frame pacing stays tight."};
}
Plan powerThrottling(const SystemInfo &pc) {
    if (pc.laptop)
        throw Unavailable(L"This PC runs on battery. Power throttling protects battery life, so LunarTweaks keeps it on for laptops.");
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", 1)},
            L"No background throttling on your " + cpuName(pc),
            L"Desktop detected. Windows will no longer push apps onto efficiency scheduling when they lose focus."};
}
Plan systemResponsiveness(const SystemInfo &pc) {
    return {{dword(Machine, kMultimedia, L"SystemResponsiveness", 10)},
            L"Games get 90% of multimedia CPU scheduling",
            L"Windows reserves 20% of CPU for background tasks during multimedia playback by default. 10% is the lowest value "
            L"Windows accepts on your " + std::to_wstring(pc.cores) + L"-thread CPU."};
}
Plan hags(const SystemInfo &pc) {
    if (pc.build < 19041) throw Unavailable(L"Hardware GPU scheduling needs Windows 10 2004 or newer.");
    if (pc.gpuVendor == 0)
        throw Unavailable(L"Your graphics adapter (" + gpuName(pc) + L") does not report hardware scheduling support.");
    if (pc.gpuVendor == 3 && gpuName(pc).find(L"Arc") == std::wstring::npos)
        throw Unavailable(L"Integrated Intel graphics do not support hardware-accelerated GPU scheduling.");
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 2)},
            L"Hardware GPU scheduling on your " + gpuName(pc),
            L"Lets the " + gpuName(pc) + L" manage its own memory and work queue, lowering CPU overhead and latency. Required for DLSS Frame Generation."};
}
Plan gpuMsi(const SystemInfo &pc) {
    if (pc.gpuInstance.empty()) throw Unavailable(L"No PCI Express graphics adapter was found.");
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Enum\\" + pc.gpuInstance +
                                L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties",
                   L"MSISupported", 1)},
            L"Message-signalled interrupts for your " + gpuName(pc),
            L"Matched to the exact PCI device of your " + gpuName(pc) +
                L". MSI interrupts avoid shared legacy IRQ lines and reduce DPC latency."};
}
Plan gameTask(const SystemInfo &pc) {
    return {{dword(Machine, kGames, L"GPU Priority", 8), dword(Machine, kGames, L"Priority", 6),
             string(Machine, kGames, L"Scheduling Category", L"High"), string(Machine, kGames, L"SFIO Priority", L"High")},
            L"Top GPU, CPU and I/O priority for games",
            L"The multimedia scheduler task Windows uses for games gets the highest GPU priority (8), CPU priority 6 and "
            L"high I/O priority on your " + gpuName(pc) + L"."};
}
Plan fullscreenExclusive(const SystemInfo &) {
    const wchar_t *k = L"System\\GameConfigStore";
    return {{dword(User, k, L"GameDVR_FSEBehaviorMode", 2), dword(User, k, L"GameDVR_HonorUserFSEBehaviorMode", 1),
             dword(User, k, L"GameDVR_FSEBehavior", 2), dword(User, k, L"GameDVR_DXGIHonorFSEWindowsCompatible", 1)},
            L"True exclusive fullscreen for DirectX games",
            L"Fullscreen games take direct control of the display instead of being composited by Windows."};
}
Plan windowedOptimizations(const SystemInfo &pc) {
    if (pc.build < 22000) throw Unavailable(L"Optimizations for windowed games is a Windows 11 feature.");
    const wchar_t *k = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
    std::wstring current = reg::asText(reg::read(User, k, L"DirectXUserGlobalSettings"));
    // Keep every other preference (e.g. VRR) the user already has.
    std::wstring merged;
    size_t start = 0;
    while (start < current.size()) {
        size_t end = current.find(L';', start);
        std::wstring item = current.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!item.empty() && item.rfind(L"SwapEffectUpgradeEnable=", 0) != 0) merged += item + L";";
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    merged += L"SwapEffectUpgradeEnable=1;";
    return {{string(User, k, L"DirectXUserGlobalSettings", merged)},
            L"Flip-model presentation for borderless games",
            L"Borderless and windowed DirectX 10/11 games use the low-latency flip model, unlocking VRR and Auto HDR."};
}
Plan kernelInRam(const SystemInfo &pc) {
    if (pc.ram < 16)
        throw Unavailable(std::to_wstring(pc.ram) + L" GB of RAM detected. Keeping the kernel resident needs at least 16 GB.");
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"DisablePagingExecutive", 1)},
            L"Kernel and drivers stay in your " + std::to_wstring(pc.ram) + L" GB of RAM",
            std::to_wstring(pc.ram) + L" GB RAM detected: enough headroom to keep kernel-mode code out of the page file, "
            L"avoiding disk reads when the game needs a driver path."};
}
Plan svchostSplit(const SystemInfo &pc) {
    if (pc.ram < 8) throw Unavailable(L"Needs at least 8 GB of RAM.");
    DWORD kb = DWORD(std::min<unsigned long long>(pc.ram * 1024ull * 1024ull, 0xFFFFFFFFull));
    return {{dword(Machine, L"SYSTEM\\CurrentControlSet\\Control", L"SvcHostSplitThresholdInKB", kb)},
            L"Fewer service processes, sized to " + std::to_wstring(pc.ram) + L" GB",
            L"Threshold set to exactly your installed memory (" + std::to_wstring(kb) +
                L" KB), so Windows groups services into shared hosts instead of ~80 separate processes."};
}
Plan sysmain(const SystemInfo &pc) {
    if (pc.ssd == 0) throw Unavailable(L"Your Windows drive is a hard disk. SysMain speeds up HDDs, so it stays on.");
    if (pc.ssd < 0) throw Unavailable(L"The Windows drive type could not be identified, so SysMain is left alone.");
    auto p = service(L"SysMain", 4);
    p.headline = L"SysMain off — your " + pc.storage + L" is already fast";
    p.note = L"Your Windows drive is an " + pc.storage + L". Prefetching into RAM adds background disk and CPU work with no benefit on solid-state storage.";
    return p;
}
Plan searchIndexer(const SystemInfo &pc) {
    auto p = service(L"WSearch", 4);
    p.headline = L"No background indexing on your " + pc.storage;
    p.note = L"Stops the indexer from scanning files while you play. File search in Explorer still works, just without the index.";
    return p;
}
Plan ntfs(const SystemInfo &pc) {
    const wchar_t *k = L"SYSTEM\\CurrentControlSet\\Control\\FileSystem";
    return {{dword(Machine, k, L"NtfsDisableLastAccessUpdate", 0x80000001), dword(Machine, k, L"NtfsDisable8dot3NameCreation", 1)},
            L"Less NTFS write overhead on your " + pc.storage,
            L"Stops last-access timestamp writes and legacy 8.3 short names on every file operation."};
}
Plan hibernation(const SystemInfo &pc) {
    if (pc.laptop) throw Unavailable(L"Laptops rely on hibernation when the battery runs low, so it stays on.");
    const wchar_t *k = L"SYSTEM\\CurrentControlSet\\Control\\Power";
    return {{dword(Machine, k, L"HibernateEnabled", 0), dword(Machine, k, L"HibernateEnabledDefault", 0)},
            L"Reclaim about " + std::to_wstring(std::max(1ull, pc.ram * 4 / 10)) + L" GB of disk space",
            L"Desktop detected. The hibernation file (sized from your " + std::to_wstring(pc.ram) +
                L" GB RAM) is removed after the next restart."};
}
Plan serviceTrim(const SystemInfo &) {
    Plan p{{}, L"", L"Only services that are installed on this PC are included:\n"};
    int n = 0;
    for (auto name : {L"MapsBroker", L"RetailDemo", L"Fax", L"WpcMonSvc", L"WerSvc", L"TrkWks", L"wisvc"})
        if (serviceExists(name)) {
            p.writes.push_back(dword(Machine, std::wstring(kServices) + name, L"Start", 4));
            p.note += std::wstring(L"• ") + name + L"\n";
            ++n;
        }
    if (!n) throw Unavailable(L"None of the optional services are installed.");
    p.headline = std::to_wstring(n) + L" unneeded background services switched off";
    return p;
}
Plan fastShutdown(const SystemInfo &) {
    return {{string(User, L"Control Panel\\Desktop", L"WaitToKillAppTimeout", L"2000"),
             string(User, L"Control Panel\\Desktop", L"HungAppTimeout", L"2000"),
             string(User, L"Control Panel\\Desktop", L"AutoEndTasks", L"1"),
             string(Machine, L"SYSTEM\\CurrentControlSet\\Control", L"WaitToKillServiceTimeout", L"2000")},
            L"Frozen apps close in 2 s instead of 5–20 s", L""};
}
Plan nagle(const SystemInfo &pc) {
    if (pc.netInterface.empty()) throw Unavailable(L"No connected Ethernet or Wi-Fi adapter was found.");
    std::wstring k = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + pc.netInterface;
    return {{dword(Machine, k, L"TcpAckFrequency", 1), dword(Machine, k, L"TCPNoDelay", 1), dword(Machine, k, L"TcpDelAckTicks", 0)},
            L"Instant TCP packets on your " + pc.network + L" adapter",
            L"Applied to your active " + pc.network + L" adapter " + pc.netInterface +
                L". Small game packets are sent and acknowledged immediately instead of being batched."};
}
Plan networkThrottling(const SystemInfo &) {
    return {{dword(Machine, kMultimedia, L"NetworkThrottlingIndex", 0xFFFFFFFF)},
            L"No packet throttling while media plays",
            L"Removes the 10-packets-per-millisecond limit Windows applies to network traffic during multimedia playback."};
}
Plan deliveryOptimization(const SystemInfo &) {
    return {{dword(Machine, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization", L"DODownloadMode", 0)},
            L"Your upload stays free for games", L""};
}
Plan dnsNegativeCache(const SystemInfo &) {
    const wchar_t *k = L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters";
    return {{dword(Machine, k, L"MaxNegativeCacheTtl", 0), dword(Machine, k, L"NegativeCacheTime", 0)},
            L"Failed DNS lookups retry immediately", L""};
}

Tweak spi(int id, int cat, int tier, const wchar_t *name, const wchar_t *summary, const wchar_t *detail, Kind kind, UINT get,
          UINT set, DWORD value, int impact = 0, bool legacy = false) {
    Tweak t{id, cat, tier, name, detail, kind, get, set, value};
    t.summary = summary;
    t.impact = impact;
    t.legacy = legacy;
    return t;
}
Tweak make(int id, int cat, int tier, const wchar_t *name, const wchar_t *summary, const wchar_t *detail, Planner p,
           int impact, bool restart = false, bool recommended = true) {
    Tweak t{id, cat, tier, name, detail, Kind::Registry};
    t.plan = p;
    t.summary = summary;
    t.impact = impact;
    t.restart = restart;
    t.recommended = recommended;
    return t;
}
Tweak power(int id, int cat, const wchar_t *name, const wchar_t *summary, const wchar_t *detail, DWORD mode, int impact,
            bool recommended, bool legacy = false, int tier = 2) {
    Tweak t{id, cat, tier, name, detail, Kind::AdaptivePower, 0, 0, mode};
    t.summary = summary;
    t.impact = impact;
    t.recommended = recommended;
    t.legacy = legacy;
    return t;
}
Tweak part(int id, const wchar_t *name, Kind kind, UINT get, UINT set, DWORD value) {
    Tweak t = spi(id, 0, 0, name, L"", L"", kind, get, set, value, 0, false);
    t.hidden = true;
    return t;
}
Tweak legacySpi(int id, const wchar_t *name, Kind kind, UINT get, UINT set, DWORD value) {
    return spi(id, 0, 0, name, L"Saved by an earlier version", L"Kept so you can restore a value saved by LunarTweaks 1.x.",
               kind, get, set, value, 0, true);
}
} // namespace

const std::vector<Tweak> &catalog() {
    static const std::vector<Tweak> all = [] {
        std::vector<Tweak> v;
        // ===== FREE · 19 =====
        // Windows Essentials
        v.push_back(make(201, 0, 0, L"Instant menus", L"Remove the 400 ms delay before menus open",
                         L"Windows waits 400 ms before showing cascading menus. Setting the delay to 0 makes every right-click and Start submenu open instantly.",
                         instantMenus, 0));
        {
            Tweak t{213, 0, 0, L"Animations off",
                    L"Turns off every Windows animation in one step: windows opening, closing, minimising and maximising, "
                    L"menu and tooltip fades, drop-down and list animations, and taskbar animations. Windows, menus and the "
                    L"taskbar react instantly. The taskbar part finishes after you sign out or restart Explorer.",
                    Kind::Bundle};
            static const int members[] = {8, 9, 1, 2, 3, 4, 5, 6, 7, 214};
            t.members = members;
            t.memberCount = int(sizeof(members) / sizeof(members[0]));
            t.summary = L"Windows, menus and the taskbar react instantly";
            t.impact = 1;
            v.push_back(t);
        }
        v.push_back(make(202, 0, 0, L"Transparency effects off", L"Solid taskbar, Start and Settings",
                         L"Acrylic blur is re-rendered by the desktop compositor every frame. Solid surfaces free a little GPU time, which matters most on integrated graphics.",
                         transparencyOff, 0));
        v.push_back(make(203, 0, 0, L"Faster startup apps", L"Skip the delay before startup apps launch",
                         L"Windows deliberately delays startup apps after you sign in. This removes the delay so your desktop is ready sooner.",
                         startupDelay, 1));
        v.push_back(make(204, 0, 0, L"Background apps off", L"Stop Store apps running when closed",
                         L"Store apps (Weather, News, Mail…) keep running in the background to refresh content. Turning this off frees RAM and CPU wake-ups.",
                         backgroundApps, 1));
        v.push_back(make(205, 0, 0, L"Ads, tips & suggestions off", L"No promoted apps or silent installs",
                         L"Turns off Start suggestions, lock-screen tips, \"get even more out of Windows\" prompts and silently installed promoted apps.",
                         suggestionsOff, 0));
        v.push_back(make(206, 0, 0, L"Advertising ID off", L"Stop cross-app ad tracking",
                         L"Disables the per-user advertising identifier apps use to personalise ads.", advertisingId, 0));
        v.push_back(make(207, 0, 0, L"Web results in Start off", L"Local-only, faster Start search",
                         L"Start search no longer sends every keystroke to Bing, so local results appear faster.", webSearch, 0, false));
        v.push_back(make(208, 0, 0, L"Minimal diagnostic data", L"Lowest telemetry level for your edition",
                         L"Sets the diagnostic data policy to the lowest level your Windows edition supports and hides feedback prompts.",
                         diagnosticData, 0));
        v.push_back(make(209, 0, 0, L"Telemetry services off", L"Connected User Experiences stops at boot",
                         L"Disables the DiagTrack and WAP push services that collect and upload usage data in the background.",
                         telemetryService, 1, true));
        // Input & Latency
        v.push_back(spi(21, 1, 0, L"Pointer acceleration off", L"1:1 mouse movement on the desktop",
                        L"Disables \"Enhance pointer precision\" so the cursor moves exactly as far as your hand does. Aim and muscle memory become consistent.",
                        Kind::Mouse, SPI_GETMOUSE, SPI_SETMOUSE, 0, 1));
        v.push_back(spi(26, 1, 0, L"Fast keyboard repeat", L"Highest key repeat rate",
                        L"Sets the Windows key repeat speed to its maximum (31) for faster held-key input.", Kind::SpiUi,
                        SPI_GETKEYBOARDSPEED, SPI_SETKEYBOARDSPEED, 31, 0));
        v.push_back(spi(25, 1, 0, L"Shortest repeat delay", L"Held keys repeat sooner",
                        L"Uses the shortest supported delay before a held key starts repeating.", Kind::SpiUi,
                        SPI_GETKEYBOARDDELAY, SPI_SETKEYBOARDDELAY, 0, 0));
        // CPU & Power
        {
            Tweak t = spi(41, 2, 0, L"High performance power plan", L"Switch to Windows' high performance plan",
                          L"Activates the built-in High performance plan. Faster clock ramp-up, more heat and power use. Plug laptops in first. Pro replaces this with a plan built for your exact CPU.",
                          Kind::Power, 0, 0, 1, 1);
            v.push_back(t);
        }
        // GPU & Gaming
        v.push_back(make(210, 3, 0, L"Game Mode on", L"Windows prioritises the active game",
                         L"Makes sure Game Mode is enabled so Windows Update and background work are held back while a game runs.",
                         gameMode, 1));
        v.push_back(make(211, 3, 0, L"Background recording off", L"No hidden Game Bar capture",
                         L"Stops Xbox Game Bar from continuously recording gameplay in the background, freeing the GPU video encoder and disk.",
                         recordingOff, 1));
        // Memory & Storage
        v.push_back(make(212, 4, 0, L"Fast startup off", L"Clean kernel on every boot",
                         L"Fast startup restores a hibernated kernel, keeping driver issues and memory leaks across boots. Off = a truly fresh start each time.",
                         fastStartupOff, 0, true, false));
        // Network
        v.push_back(spi(61, 5, 0, L"Ping test", L"30-second connectivity check to 1.1.1.1",
                        L"Measures ICMP response times to Cloudflare 1.1.1.1. A connectivity check, not your game-server ping. No setting is changed.",
                        Kind::Ping, 0, 0, 0, 0));
        v.push_back(spi(62, 5, 0, L"Flush DNS cache", L"Clear stale name lookups",
                        L"Clears the Windows DNS resolver cache so addresses are looked up fresh. Fixes stale DNS problems. The cache rebuilds naturally.",
                        Kind::Maintenance, 0, 0, 0, 0));

        // ===== PRO · 27 performance tweaks =====
        // Input & Latency
        v.push_back(make(301, 1, 2, L"Foreground priority boost", L"Your game gets the biggest CPU time slices",
                         L"Tunes the Windows scheduler so the window you are using gets three times longer CPU quanta than background work.",
                         foregroundBoost, 2));
        v.push_back(make(302, 1, 2, L"Global timer resolution", L"Tight frame pacing, even when tabbed out", L"", timerResolution, 2, true));
        v.push_back(make(310, 1, 2, L"Game scheduling priority", L"Highest GPU, CPU and I/O priority for games", L"", gameTask, 2));
        // CPU & Power
        v.push_back(power(104, 2, L"Lunar Performance plan", L"A power plan built for your exact CPU",
                          L"Creates a copy of your current plan tuned for your PC in one step: minimum clocks, full boost, energy preference, unparked cores, PCIe and USB power saving off on AC. Battery values are untouched.",
                          3, 2, true));
        v.push_back(power(305, 2, L"Unpark CPU cores", L"Every core stays ready for work",
                          L"Core parking puts cores to sleep and wakes them on demand, adding latency when a game suddenly needs them. Desktops keep 100% of cores unparked; laptops keep 50%.",
                          10, 2, false));
        v.push_back(power(108, 2, L"CPU energy preference", L"Clocks ramp up instantly",
                          L"Sets the processor energy performance preference to maximum performance on desktops, and a less aggressive value on laptops.",
                          7, 1, false));
        v.push_back(power(109, 2, L"Aggressive processor boost", L"Full turbo whenever there is headroom",
                          L"Desktops get aggressive boost, laptops normal boost. Firmware temperature and power limits always stay in force.",
                          8, 1, false));
        v.push_back(power(110, 2, L"PCIe link power saving off", L"No wake-up delay on GPU and NVMe links",
                          L"Disables Active State Power Management on PCI Express links on desktops (moderate on laptops), removing link wake-up latency.",
                          9, 1, false));
        v.push_back(power(322, 2, L"USB selective suspend off", L"Mouse, keyboard and headset never doze",
                          L"Stops Windows from suspending idle USB devices on AC power, preventing wake-up stutter on input devices and audio.",
                          11, 1, false));
        v.push_back(make(306, 2, 2, L"Power throttling off", L"Background apps keep full CPU speed",
                         L"", powerThrottling, 1, true));
        v.push_back(make(307, 2, 2, L"Multimedia CPU reservation", L"Give games 90% of scheduled CPU", L"", systemResponsiveness, 1));
        // GPU & Gaming
        v.push_back(make(308, 3, 2, L"Hardware GPU scheduling", L"Your GPU manages its own queue", L"", hags, 2, true));
        v.push_back(make(309, 3, 2, L"GPU interrupt mode (MSI)", L"Lower DPC latency for your graphics card", L"", gpuMsi, 2, true));
        v.push_back(make(311, 3, 2, L"Exclusive fullscreen", L"Bypass desktop composition in fullscreen", L"", fullscreenExclusive, 1, false, false));
        v.push_back(make(312, 3, 2, L"Windowed game optimizations", L"Flip model for borderless games", L"", windowedOptimizations, 1));
        // Memory & Storage
        v.push_back(make(314, 4, 2, L"Kernel stays in RAM", L"No paging of drivers and kernel code", L"", kernelInRam, 1, true));
        v.push_back(make(315, 4, 2, L"Service host grouping", L"Fewer svchost processes, sized to your RAM", L"", svchostSplit, 1, true));
        v.push_back(make(316, 4, 2, L"SysMain off on SSD", L"No prefetch work on solid-state drives", L"", sysmain, 1, true));
        v.push_back(make(317, 4, 2, L"Search indexing off", L"No background file scanning", L"", searchIndexer, 1, true, false));
        v.push_back(make(318, 4, 2, L"NTFS overhead off", L"Fewer disk writes per file access", L"", ntfs, 1, true));
        v.push_back(make(319, 4, 2, L"Remove hibernation file", L"Reclaim gigabytes on your system drive", L"", hibernation, 0, true, false));
        v.push_back(make(320, 4, 2, L"Background service trim", L"Disable services you never use", L"", serviceTrim, 1, true));
        v.push_back(make(321, 4, 2, L"Fast shutdown & hang recovery", L"Frozen apps close in 2 seconds", L"Shortens how long Windows waits for hung apps and services before closing them, so shutdowns and frozen programs resolve quickly.", fastShutdown, 0));
        // Network
        v.push_back(make(323, 5, 2, L"Nagle off on your adapter", L"Send game packets instantly", L"", nagle, 2, true));
        v.push_back(make(324, 5, 2, L"Network throttling off", L"Full packet rate during media playback", L"", networkThrottling, 1, true));
        v.push_back(make(325, 5, 2, L"Peer-to-peer updates off", L"Stop uploading Windows updates to others",
                         L"Delivery Optimization can upload update files to other PCs, using your upload bandwidth during games. This keeps downloads from Microsoft only.",
                         deliveryOptimization, 1));
        v.push_back(make(326, 5, 2, L"DNS failure cache off", L"Retry failed lookups immediately",
                         L"Windows remembers failed DNS lookups for minutes. This makes a dropped lookup retry immediately, helping game launchers reconnect.",
                         dnsNegativeCache, 0, true));

        // ===== Legacy (hidden): keeps 1.x backups restorable =====
        // ===== Parts of "Animations off" (applied, detected and restored through the bundle) =====
        v.push_back(part(8, L"Window open and close animations", Kind::SpiBool, SPI_GETCLIENTAREAANIMATION, SPI_SETCLIENTAREAANIMATION, 0));
        v.push_back(part(9, L"Minimise and maximise animation", Kind::Animation, SPI_GETANIMATION, SPI_SETANIMATION, 0));
        v.push_back(part(1, L"Menu animations", Kind::SpiBool, SPI_GETMENUANIMATION, SPI_SETMENUANIMATION, 0));
        v.push_back(part(2, L"Menu fade", Kind::SpiBool, SPI_GETMENUFADE, SPI_SETMENUFADE, 0));
        v.push_back(part(3, L"Tooltip animations", Kind::SpiBool, SPI_GETTOOLTIPANIMATION, SPI_SETTOOLTIPANIMATION, 0));
        v.push_back(part(4, L"Tooltip fade", Kind::SpiBool, SPI_GETTOOLTIPFADE, SPI_SETTOOLTIPFADE, 0));
        v.push_back(part(5, L"Drop-down animation", Kind::SpiBool, SPI_GETCOMBOBOXANIMATION, SPI_SETCOMBOBOXANIMATION, 0));
        v.push_back(part(6, L"Smooth list scrolling", Kind::SpiBool, SPI_GETLISTBOXSMOOTHSCROLLING, SPI_SETLISTBOXSMOOTHSCROLLING, 0));
        v.push_back(part(7, L"Selection fade", Kind::SpiBool, SPI_GETSELECTIONFADE, SPI_SETSELECTIONFADE, 0));
        {
            Tweak t = make(214, 0, 0, L"Taskbar animations", L"", L"", taskbarAnimations, 0);
            t.hidden = true;
            v.push_back(t);
        }
        v.push_back(legacySpi(10, L"Drag window outlines", Kind::SpiUi, SPI_GETDRAGFULLWINDOWS, SPI_SETDRAGFULLWINDOWS, 0));
        v.push_back(legacySpi(22, L"Neutral pointer speed", Kind::SpiPv, SPI_GETMOUSESPEED, SPI_SETMOUSESPEED, 10));
        v.push_back(legacySpi(23, L"Pointer trails", Kind::SpiUi, SPI_GETMOUSETRAILS, SPI_SETMOUSETRAILS, 0));
        v.push_back(legacySpi(24, L"Pointer snapping", Kind::SpiUi, SPI_GETSNAPTODEFBUTTON, SPI_SETSNAPTODEFBUTTON, 0));
        v.push_back(legacySpi(27, L"Vertical scrolling lines", Kind::SpiUi, SPI_GETWHEELSCROLLLINES, SPI_SETWHEELSCROLLLINES, 3));
        v.push_back(legacySpi(28, L"Horizontal scrolling", Kind::SpiUi, SPI_GETWHEELSCROLLCHARS, SPI_SETWHEELSCROLLCHARS, 3));
        v.push_back(legacySpi(29, L"Cursor shadow", Kind::SpiBool, SPI_GETCURSORSHADOW, SPI_SETCURSORSHADOW, 0));
        v.push_back(legacySpi(30, L"Pointer sonar", Kind::SpiBool, SPI_GETMOUSESONAR, SPI_SETMOUSESONAR, 0));
        v.push_back(power(101, 2, L"Adaptive everyday power plan", L"", L"", 0, 0, false, true));
        v.push_back(power(102, 2, L"Responsive workload power plan", L"", L"", 1, 0, false, true));
        v.push_back(power(103, 2, L"Sustained-load thermal profile", L"", L"", 2, 0, false, true));
        v.push_back(power(105, 2, L"Active cooling on AC", L"", L"", 4, 0, false, true));
        v.push_back(power(106, 2, L"Full processor headroom on AC", L"", L"", 5, 0, false, true));
        v.push_back(power(107, 2, L"Dynamic processor idle floor", L"", L"", 6, 0, false, true));
        return v;
    }();
    return all;
}
const Tweak *findTweak(int id) {
    for (auto &t : catalog())
        if (t.id == id) return &t;
    return nullptr;
}
} // namespace lt
