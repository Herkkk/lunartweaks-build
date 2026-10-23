#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t buffer[32768];
    DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    if (!length || length == 32768) return 1;
    auto root = std::filesystem::path(buffer).parent_path();
    auto python = root / L"OWNER" / L"runtime" / L"python.exe";
    auto script = root / L"OWNER" / L"source" / L"server" / L"owner_launcher.py";
    auto client = root / L"CLIENT" / L"LunarTweaks.exe";
    if (!std::filesystem::is_regular_file(python) || !std::filesystem::is_regular_file(script) ||
        !std::filesystem::is_regular_file(client)) {
        MessageBoxW(nullptr,
            L"Extract ALL files from the full LunarTweaks ZIP first.\n\n"
            L"Keep OPEN-LUNARTWEAKS.exe beside the CLIENT and OWNER folders.\n"
            L"The launcher needs the portable runtime inside OWNER.",
            L"LunarTweaks — incomplete extraction", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    std::wstring command = L"\"" + python.wstring() + L"\" \"" + script.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) {
        std::wstring error = L"Windows could not start the private owner service.\n\nError code: " +
                             std::to_wstring(GetLastError()) +
                             L"\n\nExtract the complete package to a normal folder and try again.";
        MessageBoxW(nullptr, error.c_str(), L"LunarTweaks startup", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
