#pragma once
// Pro licensing through your own key server (keys.lunartweaks.se, a Cloudflare Worker).
// Buyers pay with Stripe and get a LUNAR-XXXXX-XXXXX-XXXXX-XXXXX key. The app activates the key for
// this PC, then re-validates it. Every server answer is ECDSA P-256 signed over the app's random
// nonce and this PC's device id, and checked against the public key compiled in below, so a fake
// server or a replayed answer cannot unlock Pro.
#include "polar_license.hpp"
namespace lt {
// Public half of OWNER/private/keyserver-signing-key.json (the Worker's SIGNING_KEY secret).
inline constexpr const char *kKeyServerPublicX = "ba32d455cbb0d52d46d29bec8ba6e9f7e850782fba25e5efeba28ffc4dbe63e1";
inline constexpr const char *kKeyServerPublicY = "ab0c0f5c12dc6cd19d6a7b8cb2fd87591efaa2537041bb317e697a4224afbbcf";

class KeyServerLicense {
    std::wstring host_, base_;
    INTERNET_PORT port_ = INTERNET_DEFAULT_HTTPS_PORT;
    std::string key_, device_;
    long long lastOk_ = 0;
    ULONGLONG lastCheck_ = 0;
    std::atomic_int tier_{0};
    std::wstring keyFile() const { return dataDir() + L"\\keyserver.dpapi"; }
    static long long now() { return (long long)std::time(nullptr); }
    struct Reply { DWORD status = 0; std::string body; };
    Reply post(const std::wstring &path, const std::string &body) {
        HttpHandle session(WinHttpOpen(L"LunarTweaks/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        WinHttpSetTimeouts(session, 8000, 8000, 10000, 10000);
        HttpHandle conn(WinHttpConnect(session, host_.c_str(), port_, 0));
        HttpHandle req(WinHttpOpenRequest(conn, L"POST", (base_ + path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        if (!WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1L, (void *)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            throw PolarLicense::NetworkError();
        Reply r;
        DWORD size = sizeof(r.status);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &r.status, &size, nullptr))
            throw PolarLicense::NetworkError();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available)) throw PolarLicense::NetworkError();
            if (!available) break;
            if (r.body.size() + available > 65536) throw std::runtime_error("Key server response is too large");
            size_t at = r.body.size();
            r.body.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(req, r.body.data() + at, available, &got)) throw PolarLicense::NetworkError();
            r.body.resize(at + got);
            if (!got) break;
        }
        return r;
    }
    static bool verify(const std::string &message, const std::string &sigHex) {
        std::vector<BYTE> sig;
        try { sig = unhex(sigHex); } catch (...) { return false; }
        if (sig.size() != 64) return false;
        auto x = unhex(kKeyServerPublicX), y = unhex(kKeyServerPublicY);
        std::vector<BYTE> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
        auto header = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
        header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        header->cbKey = 32;
        memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), x.data(), 32);
        memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB) + 32, y.data(), 32);
        auto hash = sha(message);
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        if (!status) status = BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0);
        if (!status) status = BCryptVerifySignature(key, nullptr, hash.data(), (ULONG)hash.size(), sig.data(), (ULONG)sig.size(), 0);
        if (key) BCryptDestroyKey(key);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return status == 0;
    }
    // Sends a signed request; returns the verified status ("granted", "device_limit", ...).
    std::string ask(const char *purpose, std::string &message) {
        std::vector<BYTE> random(32);
        if (BCryptGenRandom(nullptr, random.data(), 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) throw std::runtime_error("Could not create a secure request");
        std::string nonce = hex(random);
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &n);
        auto r = post(std::wstring(L"/v1/") + wide(purpose), "{\"key\":" + jsonString(key_) + ",\"device\":" + jsonString(device_) +
                      ",\"label\":" + jsonString(utf8(name)) + ",\"nonce\":" + jsonString(nonce) + "}");
        if (r.status == 429) throw std::runtime_error("Too many attempts. Wait a minute and try again.");
        if (r.status >= 500) throw PolarLicense::NetworkError();
        Json j;
        try { j = Json::parse(r.body); } catch (...) { throw PolarLicense::NetworkError(); }
        std::string status = j.str("status"), sig = j.str("sig");
        auto ts = j.get("ts");
        if (status.empty() || !ts || ts->type != Json::Number)
            throw std::runtime_error("The key server sent an unexpected answer.");
        long long stamp = (long long)ts->n;
        std::string signedText = "LunarTweaks/v2\n" + std::string(purpose) + "\n" + nonce + "\n" + device_ + "\n" + status + "\n" + std::to_string(stamp);
        if (!verify(signedText, sig)) throw std::runtime_error("The key server's identity could not be verified. Pro was not unlocked.");
        message = j.str("message");
        return status;
    }
    void save() {
        std::string blob = key_ + "\n" + std::to_string(lastOk_);
        DATA_BLOB in{(DWORD)blob.size(), (BYTE *)blob.data()}, out{};
        if (!CryptProtectData(&in, L"LunarTweaks Pro", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
        try { durableWrite(keyFile(), out.pbData, out.cbData); } catch (...) {}
        LocalFree(out.pbData);
    }
    bool load() {
        auto raw = readFile(keyFile());
        if (raw.empty()) return false;
        DATA_BLOB in{(DWORD)raw.size(), (BYTE *)raw.data()}, out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        std::string s((char *)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        auto a = s.find('\n');
        if (a == std::string::npos) return false;
        key_ = s.substr(0, a);
        try { lastOk_ = std::stoll(s.substr(a + 1)); } catch (...) { lastOk_ = 0; }
        return !key_.empty();
    }
    void granted() { tier_ = 2; lastOk_ = now(); lastCheck_ = GetTickCount64(); save(); }
    void validateOnline() {
        std::string message;
        auto status = ask("validate", message);
        if (status == "not_activated") { // e.g. the owner reset this key's PCs: activate again.
            status = ask("activate", message);
        }
        if (status == "granted") { granted(); return; }
        tier_ = 0;
        throw std::runtime_error(message.empty() ? "This Pro key isn't valid on this PC." : message);
    }
  public:
    static constexpr long long kOfflineGrace = 7 * 24 * 3600;
    explicit KeyServerLicense(const std::wstring &url) {
        URL_COMPONENTS c{};
        c.dwStructSize = sizeof(c);
        wchar_t hostName[256]{}, path[512]{};
        c.lpszHostName = hostName; c.dwHostNameLength = 256;
        c.lpszUrlPath = path; c.dwUrlPathLength = 512;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c) || c.nScheme != INTERNET_SCHEME_HTTPS)
            throw std::runtime_error("KeyServer Url in LunarTweaks.ini must start with https://");
        host_.assign(hostName, c.dwHostNameLength);
        port_ = c.nPort;
        base_.assign(path, c.dwUrlPathLength);
        while (!base_.empty() && base_.back() == L'/') base_.pop_back();
        auto machine = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
        device_ = hex(sha("LunarTweaks/keyserver/device|" + utf8(machine))).substr(0, 40);
    }
    bool configured() const { return !host_.empty(); }
    bool remembered() { return load(); }
    int tier() const { return tier_; }
    void activate(std::string key) {
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return isspace(c); }), key.end());
        for (auto &ch : key) ch = (char)toupper((unsigned char)ch);
        if (key.empty()) throw std::runtime_error("Paste the Pro key from your receipt.");
        key_ = key;
        std::string message;
        auto status = ask("activate", message);
        if (status == "granted") { granted(); return; }
        tier_ = 0;
        throw std::runtime_error(message.empty() ? "That key couldn't be activated." : message);
    }
    void resume() {
        if (!load()) throw std::runtime_error("No saved key");
        try { validateOnline(); }
        catch (const PolarLicense::NetworkError &) {
            if (lastOk_ && now() - lastOk_ < kOfflineGrace) { tier_ = 2; return; }
            throw;
        }
    }
    int authorize() {
        if (key_.empty()) throw std::runtime_error("Activate Pro first.");
        if (tier_ == 2 && lastCheck_ && GetTickCount64() - lastCheck_ < 10 * 60 * 1000) return 2;
        try { validateOnline(); }
        catch (const PolarLicense::NetworkError &) {
            if (lastOk_ && now() - lastOk_ < kOfflineGrace) { tier_ = 2; lastCheck_ = GetTickCount64(); return 2; }
            throw;
        }
        return tier_;
    }
    void signOut() {
        if (!key_.empty()) {
            try { post(L"/v1/deactivate", "{\"key\":" + jsonString(key_) + ",\"device\":" + jsonString(device_) + "}"); } catch (...) {}
        }
        DeleteFileW(keyFile().c_str());
        SecureZeroMemory(key_.data(), key_.size());
        key_.clear(); lastOk_ = 0; lastCheck_ = 0; tier_ = 0;
    }
};
// ---- One front for the app: your key server, else Polar, else the old self-hosted server ----
class Licensing {
    std::unique_ptr<KeyServerLicense> keys_;
    std::unique_ptr<PolarLicense> polar_;
    std::unique_ptr<LicenseClient> legacy_;
    static std::wstring setting(const wchar_t *section, const wchar_t *name) {
        wchar_t value[512]{};
        GetPrivateProfileStringW(section, name, L"", value, 512, (exeDir() + L"\\LunarTweaks.ini").c_str());
        std::wstring v = value;
        v.erase(std::remove_if(v.begin(), v.end(), [](wchar_t c) { return iswspace(c); }), v.end());
        return v;
    }
  public:
    Licensing() {
        auto url = setting(L"KeyServer", L"Url");
        auto org = setting(L"Polar", L"OrganizationId");
        if (!url.empty()) keys_ = std::make_unique<KeyServerLicense>(url);
        else if (!org.empty())
            polar_ = std::make_unique<PolarLicense>(org, GetPrivateProfileIntW(L"Polar", L"Sandbox", 0, (exeDir() + L"\\LunarTweaks.ini").c_str()) == 1);
        else legacy_ = std::make_unique<LicenseClient>();
    }
    // True for the online key systems that remember the key and sign straight back in.
    bool polar() const { return keys_ || polar_; }
    bool configured() const { return keys_ ? keys_->configured() : polar_ ? polar_->configured() : legacy_->configured(); }
    int tier() const { return keys_ ? keys_->tier() : polar_ ? polar_->tier() : legacy_->tier(); }
    void activate(const std::string &key) { if (keys_) keys_->activate(key); else if (polar_) polar_->activate(key); else legacy_->activate(key); }
    int authorize() { return keys_ ? keys_->authorize() : polar_ ? polar_->authorize() : legacy_->authorize(); }
    bool remembered() { return keys_ ? keys_->remembered() : polar_ ? polar_->remembered() : !loadKey().empty(); }
    void resume() {
        if (keys_) keys_->resume();
        else if (polar_) polar_->resume();
        else { auto k = loadKey(); if (k.empty()) throw std::runtime_error("No saved key"); legacy_->activate(utf8(k)); }
    }
    void signOut() { if (keys_) keys_->signOut(); else if (polar_) polar_->signOut(); }
};
} // namespace lt
