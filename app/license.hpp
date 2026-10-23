#pragma once
#include "util.hpp"
#include "entitlements.hpp"
#include "license_public.hpp"
#include "endpoint.hpp"
#include <atomic>
#include <ctime>
#include <ncrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
namespace lt {
inline std::wstring loadKey();
class HttpHandle {
    HINTERNET h = nullptr;

  public:
    explicit HttpHandle(HINTERNET v) : h(v) {
        if (!h)
            throw std::runtime_error("Could not open secure connection");
    }
    ~HttpHandle() {
        WinHttpCloseHandle(h);
    }
    operator HINTERNET() const {
        return h;
    }
    HttpHandle(const HttpHandle &) = delete;
};
class LicenseClient {
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE device = 0;
    std::wstring host;
    INTERNET_PORT port = 443;
    std::string hwid_, pub_, token_;
    std::atomic_int tier_{0};
    bool tpm_ = false;
    bool local_owner_ = false;
    long long expires_ = 0;
    void deviceKey() {
        for (const wchar_t *name : {MS_PLATFORM_CRYPTO_PROVIDER, MS_KEY_STORAGE_PROVIDER}) {
            if (NCryptOpenStorageProvider(&provider, name, 0))
                continue;
            SECURITY_STATUS e = NCryptOpenKey(provider, &device, L"LunarTweaks.Device.v1", 0, 0);
            if (e == NTE_BAD_KEYSET) {
                e = NCryptCreatePersistedKey(provider, &device, NCRYPT_ECDSA_P256_ALGORITHM,
                                             L"LunarTweaks.Device.v1", 0, 0);
                if (!e) {
                    DWORD policy = 0, usage = NCRYPT_ALLOW_SIGNING_FLAG;
                    e = NCryptSetProperty(device, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&policy,
                                          sizeof(policy), 0);
                    if (!e)
                        e = NCryptSetProperty(device, NCRYPT_KEY_USAGE_PROPERTY, (PBYTE)&usage,
                                              sizeof(usage), 0);
                    if (!e)
                        e = NCryptFinalizeKey(device, 0);
                }
            }
            if (!e) {
                tpm_ = std::wstring(name) == MS_PLATFORM_CRYPTO_PROVIDER;
                DWORD n = 0;
                e = NCryptExportKey(device, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &n, 0);
                std::vector<BYTE> blob(n);
                if (!e)
                    e = NCryptExportKey(device, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, blob.data(), n, &n,
                                        0);
                if (!e) {
                    pub_ = hex(blob);
                    return;
                }
            }
            if (device) {
                NCryptFreeObject(device);
                device = 0;
            }
            NCryptFreeObject(provider);
            provider = 0;
        }
        throw std::runtime_error("Could not create this PC's secure device identity");
    }
    std::string fingerprint() {
        auto machine =
            regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
        if (machine.empty())
            throw std::runtime_error("Machine identity unavailable");
        // SMBIOS type 1 UUID, excluding volatile BIOS and memory configuration.
        std::string uuid;
        constexpr DWORD rsmb = 0x52534d42;
        UINT n = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
        if (n >= 8 && n < 4 * 1024 * 1024) {
            std::vector<BYTE> b(n);
            if (GetSystemFirmwareTable(rsmb, 0, b.data(), n) == n) {
                size_t p = 8;
                while (p + 4 <= b.size()) {
                    BYTE type = b[p], len = b[p + 1];
                    if (len < 4 || p + len > b.size())
                        break;
                    if (type == 1 && len >= 24) {
                        uuid = hex(std::vector<BYTE>(b.begin() + p + 8, b.begin() + p + 24));
                        break;
                    }
                    size_t end = p + len;
                    while (end + 1 < b.size() && (b[end] != 0 || b[end + 1] != 0))
                        ++end;
                    p = end + 2;
                }
            }
        }
        return hex(sha("LunarTweaks/device/v1|" + utf8(machine) + "|" + uuid));
    }
    Object post(const std::wstring &path, const Object &o) {
        HttpHandle session(WinHttpOpen(L"LunarTweaks/1.0",
                                       local_owner_ ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                                    : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        HttpHandle conn(WinHttpConnect(session, host.c_str(), port, 0));
        HttpHandle req(WinHttpOpenRequest(conn, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          local_owner_ ? 0 : WINHTTP_FLAG_SECURE));
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        auto body = encode(o);
        if (!WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1L,
                                (void *)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            throw std::runtime_error(
                local_owner_
                    ? "Local owner service is unavailable. Start the app using START-LunarTweaks.cmd."
                    : "License server unavailable or HTTPS certificate invalid");
        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                 &status, &size, nullptr))
            throw std::runtime_error("Invalid HTTP response");
        std::string raw;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available))
                throw std::runtime_error("Connection interrupted");
            if (!available)
                break;
            if (raw.size() + available > 8192)
                throw std::runtime_error("Oversized licensing response");
            size_t at = raw.size();
            raw.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(req, raw.data() + at, available, &got))
                throw std::runtime_error("Connection interrupted");
            raw.resize(at + got);
            if (!got)
                break;
        }
        auto r = decode(raw);
        if (status != 200)
            throw std::runtime_error(r.count("error") ? r.at("error") : "License request rejected");
        return r;
    }
    Object proof(const std::string &op, const std::string &credential) {
        auto r = post(L"/v1/challenge", {});
        auto nonce = r.at("nonce");
        if (nonce.size() != 64)
            throw std::runtime_error("Invalid challenge");
        std::vector<BYTE> random(32);
        if (BCryptGenRandom(nullptr, random.data(), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            throw std::runtime_error("Could not create request identity");
        auto requestId = hex(random);
        auto hash = sha("LunarTweaks/v1\n" + op + "\n" + nonce + "\n" + hwid_ + "\n" +
                        hex(sha(credential)) + "\n" + requestId);
        DWORD n = 0;
        auto e = NCryptSignHash(device, nullptr, hash.data(), (DWORD)hash.size(), nullptr, 0, &n, 0);
        std::vector<BYTE> sig(n);
        if (!e)
            e = NCryptSignHash(device, nullptr, hash.data(), (DWORD)hash.size(), sig.data(), n, &n, 0);
        if (e || n != 64)
            throw std::runtime_error("Device proof failed");
        return {{"request_id", requestId}, {"nonce", nonce},
                {"hwid", hwid_},           {"pub", pub_},
                {"sig", hex(sig)},         {op == "activate" ? "key" : "token", credential}};
    }
    void verifyServer(const std::string &op, const Object &request, const Object &response) {
        auto token = op == "activate" ? response.at("token") : request.at("token");
        auto hash = sha("LunarTweaks/response/v1\n" + op + "\n" + request.at("nonce") + "\n" +
                        request.at("hwid") + "\n" + response.at("tier") + "\n" + token + "\n" +
                        response.at("expires") + "\n" + request.at("request_id"));
        auto signature = unhex(response.at("response_sig"));
        auto publicBlob = unhex(serverPublicHex);
        if (signature.size() != 64)
            throw std::runtime_error("License server proof is invalid");
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE publicKey = nullptr;
        NTSTATUS status =
            BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        if (!status)
            status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &publicKey,
                                         publicBlob.data(), (ULONG)publicBlob.size(), 0);
        if (!status)
            status = BCryptVerifySignature(publicKey, nullptr, hash.data(), (ULONG)hash.size(),
                                           signature.data(), (ULONG)signature.size(), 0);
        if (publicKey)
            BCryptDestroyKey(publicKey);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status)
            throw std::runtime_error("License server identity check failed");
    }
    static int tierValue(const std::string &s) {
        // Normalize only after verifying the server's signed response.
        // Older services may still call a paid entitlement Ultimate.
        return paidPlanValue(s);
    }

  public:
    LicenseClient() {
        wchar_t value[1024];
        GetPrivateProfileStringW(L"License", L"Server", L"", value, 1024,
                                 (exeDir() + L"\\LunarTweaks.ini").c_str());
        std::wstring url = value;
        if (url.empty()) {
            host = L"";
            return;
        }
        bool allowLocal = GetPrivateProfileIntW(L"License", L"AllowLocalOwnerServer", 0,
                                                (exeDir() + L"\\LunarTweaks.ini").c_str()) == 1;
        if (auto localPort = ownerLoopbackPort(url, allowLocal)) {
            local_owner_ = true;
            host = L"127.0.0.1";
            port = (INTERNET_PORT)localPort;
            return;
        }
        URL_COMPONENTS c{};
        c.dwStructSize = sizeof(c);
        wchar_t hn[512], pth[512];
        c.lpszHostName = hn;
        c.dwHostNameLength = 512;
        c.lpszUrlPath = pth;
        c.dwUrlPathLength = 512;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || c.nScheme != INTERNET_SCHEME_HTTPS ||
            c.dwUserNameLength || c.dwPasswordLength || c.dwExtraInfoLength || c.dwUrlPathLength > 1)
            throw std::runtime_error(
                "Server must be an HTTPS origin, for example https://license.example.com");
        host.assign(hn, c.dwHostNameLength);
        port = c.nPort;
    }
    ~LicenseClient() {
        if (device)
            NCryptFreeObject(device);
        if (provider)
            NCryptFreeObject(provider);
    }
    LicenseClient(const LicenseClient &) = delete;
    bool configured() const {
        return !host.empty();
    }
    bool localOwner() const {
        return local_owner_;
    }
    bool tpm() const {
        return tpm_;
    }
    int tier() const {
        return tier_;
    }
    const std::string &hwid() const {
        return hwid_;
    }
    void activate(std::string key) {
        if (!configured())
            throw std::runtime_error(
                "Owner setup needed: configure the HTTPS license server in LunarTweaks.ini");
        if (!device) {
            deviceKey();
            hwid_ = fingerprint();
        }
        auto request = proof("activate", key);
        auto r = post(L"/v1/activate", request);
        verifyServer("activate", request, r);
        tier_ = tierValue(r.at("tier"));
        token_ = r.at("token");
        expires_ = std::stoll(r.at("expires"));
        if (token_.size() != 64) {
            tier_ = 0;
            throw std::runtime_error("Invalid session");
        }
    }
    int authorize() {
        if (expires_ && std::time(nullptr) >= expires_ - 30) {
            auto k = loadKey();
            if (k.empty())
                throw std::runtime_error("Session expired; sign in again");
            activate(utf8(k));
        }
        if (token_.empty())
            throw std::runtime_error("Sign in first");
        try {
            auto request = proof("validate", token_);
            auto r = post(L"/v1/validate", request);
            verifyServer("validate", request, r);
            tier_ = tierValue(r.at("tier"));
            return tier_;
        } catch (...) {
            tier_ = 0;
            throw;
        }
    }
};
inline void saveKey(const std::wstring &key) {
    auto raw = utf8(key);
    DATA_BLOB in{(DWORD)raw.size(), (BYTE *)raw.data()}, out{};
    if (!CryptProtectData(&in, L"LunarTweaks license", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("Could not protect license key");
    try {
        durableWrite(dataDir() + L"\\license.dpapi", out.pbData, out.cbData);
    } catch (...) {
        LocalFree(out.pbData);
        throw;
    }
    LocalFree(out.pbData);
    SecureZeroMemory(raw.data(), raw.size());
}
inline std::wstring loadKey() {
    auto raw = readFile(dataDir() + L"\\license.dpapi");
    if (raw.empty())
        return {};
    DATA_BLOB in{(DWORD)raw.size(), (BYTE *)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string key((char *)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return wide(key);
}
} // namespace lt
