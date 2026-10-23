#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstddef>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace lt {
inline std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
inline std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
inline std::string hex(const std::vector<BYTE> &v) {
    static const char *h = "0123456789abcdef";
    std::string s;
    for (auto b : v) {
        s += h[b >> 4];
        s += h[b & 15];
    }
    return s;
}
inline std::vector<BYTE> unhex(const std::string &s) {
    if (s.size() % 2)
        throw std::runtime_error("Invalid hex");
    std::vector<BYTE> v;
    for (size_t i = 0; i < s.size(); i += 2) {
        auto d = [](char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            throw std::runtime_error("Invalid hex");
        };
        v.push_back((BYTE)((d(s[i]) << 4) | d(s[i + 1])));
    }
    return v;
}
inline std::vector<BYTE> sha(const std::string &s) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<BYTE> out(32);
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        throw std::runtime_error("Crypto provider unavailable");
    NTSTATUS e = BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0);
    if (!e)
        e = BCryptHashData(h, (PUCHAR)s.data(), (ULONG)s.size(), 0);
    if (!e)
        e = BCryptFinishHash(h, out.data(), 32, 0);
    if (h)
        BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    if (e)
        throw std::runtime_error("Hash failed");
    return out;
}
inline std::wstring exeDir() {
    wchar_t p[32768];
    DWORD n = GetModuleFileNameW(nullptr, p, 32768);
    if (!n || n == 32768)
        throw std::runtime_error("Executable path unavailable");
    return std::filesystem::path(p).parent_path().wstring();
}
inline std::wstring dataDir() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        throw std::runtime_error("Profile folder unavailable");
    std::filesystem::path path = std::filesystem::path(p) / L"LunarTweaks";
    CoTaskMemFree(p);
    std::filesystem::create_directories(path / L"backups");
    return path.wstring();
}
inline std::string readFile(const std::wstring &path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
inline void durableWrite(const std::wstring &path, const void *data, DWORD size,
                         bool exclusive = false) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           exclusive ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create backup or settings file");
    DWORD n = 0;
    bool ok = WriteFile(h, data, size, &n, nullptr) && n == size && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok)
        throw std::runtime_error("Could not safely save data");
}
inline std::wstring regString(HKEY root, const wchar_t *path, const wchar_t *name) {
    HKEY h;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return {};
    DWORD n = 0, type = 0;
    LSTATUS e = RegQueryValueExW(h, name, nullptr, &type, nullptr, &n);
    std::vector<wchar_t> v(n / 2 + 2);
    if (e == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ))
        e = RegQueryValueExW(h, name, nullptr, &type, (BYTE *)v.data(), &n);
    else
        e = ERROR_INVALID_DATA;
    RegCloseKey(h);
    return e == ERROR_SUCCESS ? std::wstring(v.data()) : L"";
}
// Deliberately small JSON codec for the protocol's flat string/integer objects.
inline std::string quote(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 32)
            throw std::runtime_error("Invalid protocol string");
        else
            o += char(c);
    }
    return o + '"';
}
using Object = std::map<std::string, std::string>;
inline std::string encode(const Object &o) {
    std::string s = "{";
    for (auto &[k, v] : o) {
        if (s.size() > 1)
            s += ',';
        s += quote(k) + ":" + quote(v);
    }
    return s + "}";
}
inline Object decode(const std::string &s) {
    size_t p = 0;
    auto ws = [&]() {
        while (p < s.size() && isspace((unsigned char)s[p]))
            ++p;
    };
    auto fail = []() { throw std::runtime_error("Invalid licensing response"); };
    auto str = [&]() {
        std::string v;
        if (p >= s.size() || s[p++] != '"')
            fail();
        bool closed = false;
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') {
                closed = true;
                break;
            }
            if ((unsigned char)c < 32)
                fail();
            if (c == '\\') {
                if (p >= s.size())
                    fail();
                char e = s[p++];
                if (e == '"' || e == '\\' || e == '/')
                    v += e;
                else if (e == 'n')
                    v += '\n';
                else if (e == 'r')
                    v += '\r';
                else if (e == 't')
                    v += '\t';
                else
                    fail();
            } else
                v += c;
        }
        if (!closed)
            fail();
        return v;
    };
    Object o;
    ws();
    if (p >= s.size() || s[p++] != '{')
        fail();
    ws();
    if (p < s.size() && s[p] == '}') {
        ++p;
        ws();
        if (p != s.size())
            fail();
        return o;
    }
    while (p < s.size()) {
        ws();
        auto k = str();
        ws();
        if (p >= s.size() || s[p++] != ':')
            fail();
        ws();
        std::string v;
        if (p < s.size() && s[p] == '"')
            v = str();
        else {
            size_t start = p;
            while (p < s.size() && isdigit((unsigned char)s[p]))
                ++p;
            if (p == start)
                fail();
            v = s.substr(start, p - start);
        }
        if (o.count(k))
            fail();
        o[k] = v;
        ws();
        if (p >= s.size())
            fail();
        char c = s[p++];
        if (c == '}') {
            ws();
            if (p != s.size())
                fail();
            return o;
        }
        if (c != ',')
            fail();
    }
    fail();
    return {};
}
} // namespace lt
