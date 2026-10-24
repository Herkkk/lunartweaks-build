#pragma once
// Pro licensing through Polar (polar.sh) license keys.
// Buyers receive a key automatically from Polar. The app activates it once per PC through Polar's
// public customer-portal API (no secret token in the app), binds the activation to this PC with a
// "device" condition, and re-validates it online. Short offline periods keep Pro working.
#include "license.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
namespace lt {
// ---- Minimal JSON reader (objects, arrays, strings, numbers, true/false/null) ----------
struct Json {
    enum Type { Null, Bool, Number, String, Array, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json *get(const std::string &k) const {
        for (auto &f : fields) if (f.first == k) return &f.second;
        return nullptr;
    }
    std::string str(const std::string &k) const { auto v = get(k); return v && v->type == String ? v->s : std::string(); }
    bool isNull(const std::string &k) const { auto v = get(k); return !v || v->type == Null; }
    static Json parse(const std::string &text) {
        size_t p = 0;
        Json j = value(text, p, 0);
        ws(text, p);
        if (p != text.size()) throw std::runtime_error("Unexpected license response");
        return j;
    }
  private:
    static void ws(const std::string &t, size_t &p) { while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p; }
    static void fail() { throw std::runtime_error("Unexpected license response"); }
    static std::string string(const std::string &t, size_t &p) {
        if (p >= t.size() || t[p] != '"') fail();
        ++p;
        std::string out;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) fail();
            char e = t[p++];
            switch (e) {
            case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'u': {
                if (p + 4 > t.size()) fail();
                unsigned cp = std::stoul(t.substr(p, 4), nullptr, 16); p += 4;
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                break;
            }
            default: out += e;
            }
        }
        if (p >= t.size()) fail();
        ++p;
        return out;
    }
    static Json value(const std::string &t, size_t &p, int depth) {
        if (depth > 32) fail();
        ws(t, p);
        if (p >= t.size()) fail();
        Json j;
        char c = t[p];
        if (c == '{') {
            j.type = Obj; ++p; ws(t, p);
            if (p < t.size() && t[p] == '}') { ++p; return j; }
            for (;;) {
                ws(t, p);
                auto k = string(t, p);
                ws(t, p);
                if (p >= t.size() || t[p++] != ':') fail();
                j.fields.emplace_back(k, value(t, p, depth + 1));
                ws(t, p);
                if (p >= t.size()) fail();
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == '}') { ++p; return j; }
                fail();
            }
        }
        if (c == '[') {
            j.type = Array; ++p; ws(t, p);
            if (p < t.size() && t[p] == ']') { ++p; return j; }
            for (;;) {
                j.items.push_back(value(t, p, depth + 1));
                ws(t, p);
                if (p >= t.size()) fail();
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == ']') { ++p; return j; }
                fail();
            }
        }
        if (c == '"') { j.type = String; j.s = string(t, p); return j; }
        if (t.compare(p, 4, "true") == 0) { j.type = Bool; j.b = true; p += 4; return j; }
        if (t.compare(p, 5, "false") == 0) { j.type = Bool; p += 5; return j; }
        if (t.compare(p, 4, "null") == 0) { p += 4; return j; }
        size_t start = p;
        while (p < t.size() && (isdigit((unsigned char)t[p]) || t[p] == '-' || t[p] == '+' || t[p] == '.' || t[p] == 'e' || t[p] == 'E')) ++p;
        if (p == start) fail();
        j.type = Number; j.n = std::stod(t.substr(start, p - start));
        return j;
    }
};
inline std::string jsonString(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 32) { char buf[16]; snprintf(buf, sizeof buf, "\\u%04x", unsigned(c)); o += buf; }
        else o += char(c);
    }
    return o + "\"";
}
// ---- Polar client ------------------------------------------------------------------------
class PolarLicense {
    std::wstring org_, host_;
    std::string key_, activation_, device_;
    long long lastOk_ = 0;            // unix time of last successful online validation
    ULONGLONG lastCheck_ = 0;         // tick of last validation attempt that succeeded
    std::atomic_int tier_{0};
    std::wstring keyFile() const { return dataDir() + L"\\polar.dpapi"; }
    static long long now() { return (long long)std::time(nullptr); }
    struct Reply { DWORD status = 0; std::string body; };
    Reply post(const wchar_t *path, const std::string &body) {
        HttpHandle session(WinHttpOpen(L"LunarTweaks/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        WinHttpSetTimeouts(session, 8000, 8000, 10000, 10000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
        HttpHandle conn(WinHttpConnect(session, host_.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        HttpHandle req(WinHttpOpenRequest(conn, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!WinHttpSendRequest(req, L"Content-Type: application/json\r\nAccept: application/json\r\n", (DWORD)-1L,
                                (void *)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            throw NetworkError();
        Reply r;
        DWORD size = sizeof(r.status);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &r.status, &size, nullptr))
            throw NetworkError();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available)) throw NetworkError();
            if (!available) break;
            if (r.body.size() + available > 262144) throw std::runtime_error("License response is too large");
            size_t at = r.body.size();
            r.body.resize(at + available);
            DWORD got = 0;
            if (!WinHttpReadData(req, r.body.data() + at, available, &got)) throw NetworkError();
            r.body.resize(at + got);
            if (!got) break;
        }
        return r;
    }
    std::string base() const {
        return "\"key\":" + jsonString(key_) + ",\"organization_id\":" + jsonString(utf8(org_)) +
               ",\"conditions\":{\"device\":" + jsonString(device_) + "}";
    }
    static std::string detail(const std::string &body) {
        try { auto j = Json::parse(body); auto d = j.get("detail"); if (d && d->type == Json::String) return d->s; } catch (...) {}
        return {};
    }
    void save() {
        std::string blob = key_ + "\n" + activation_ + "\n" + std::to_string(lastOk_);
        DATA_BLOB in{(DWORD)blob.size(), (BYTE *)blob.data()}, out{};
        if (!CryptProtectData(&in, L"LunarTweaks Pro", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
        try { durableWrite(keyFile(), out.pbData, out.cbData); } catch (...) {}
        LocalFree(out.pbData);
        SecureZeroMemory(blob.data(), blob.size());
    }
    bool load() {
        auto raw = readFile(keyFile());
        if (raw.empty()) return false;
        DATA_BLOB in{(DWORD)raw.size(), (BYTE *)raw.data()}, out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
        std::string s((char *)out.pbData, out.cbData);
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        auto a = s.find('\n'), b = a == std::string::npos ? a : s.find('\n', a + 1);
        if (b == std::string::npos) return false;
        key_ = s.substr(0, a);
        activation_ = s.substr(a + 1, b - a - 1);
        try { lastOk_ = std::stoll(s.substr(b + 1)); } catch (...) { lastOk_ = 0; }
        return !key_.empty();
    }
    // Returns true when Polar confirms the key is granted for this PC.
    void validateOnline() {
        std::string body = "{" + base() + (activation_.empty() ? "" : ",\"activation_id\":" + jsonString(activation_)) + "}";
        auto r = post(L"/v1/customer-portal/license-keys/validate", body);
        if (r.status == 404) {
            tier_ = 0;
            throw std::runtime_error("This Pro key isn't valid on this PC. Check the key from your Polar receipt, or activate it again.");
        }
        if (r.status != 200) throw std::runtime_error("Polar couldn't check the key right now (HTTP " + std::to_string(r.status) + "). " + detail(r.body));
        auto j = Json::parse(r.body);
        auto status = j.str("status");
        if (status != "granted") {
            tier_ = 0;
            throw std::runtime_error(status == "revoked" ? "This Pro key has been revoked." : status == "disabled" ? "This Pro key is disabled." : "This Pro key is not active.");
        }
        if (!j.isNull("expires_at")) {
            std::tm tm{};
            auto e = j.str("expires_at");
            auto num = [&](size_t at, size_t n) { int v = 0; for (size_t i = at; i < at + n && i < e.size(); ++i) { if (!isdigit((unsigned char)e[i])) return -1; v = v * 10 + (e[i] - '0'); } return v; };
            if (e.size() >= 19 && num(0, 4) > 0 && num(5, 2) >= 1 && num(8, 2) >= 1 && num(11, 2) >= 0 && num(14, 2) >= 0 && num(17, 2) >= 0) {
                tm.tm_year = num(0, 4) - 1900; tm.tm_mon = num(5, 2) - 1; tm.tm_mday = num(8, 2);
                tm.tm_hour = num(11, 2); tm.tm_min = num(14, 2); tm.tm_sec = num(17, 2);
                if ((long long)_mkgmtime(&tm) < now()) { tier_ = 0; throw std::runtime_error("This Pro key has expired. Renew it on lunartweaks.se."); }
            }
        }
        tier_ = 2;
        lastOk_ = now();
        lastCheck_ = GetTickCount64();
        save();
    }
  public:
    struct NetworkError : std::runtime_error {
        NetworkError() : std::runtime_error("Can't reach Polar to check your key. Check your internet connection and try again.") {}
    };
    static constexpr long long kOfflineGrace = 7 * 24 * 3600; // Pro keeps working offline for 7 days.
    PolarLicense(std::wstring organization, bool sandbox) : org_(std::move(organization)) {
        host_ = sandbox ? L"sandbox-api.polar.sh" : L"api.polar.sh";
        auto machine = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid");
        device_ = hex(sha("LunarTweaks/polar/device|" + utf8(machine))).substr(0, 40);
    }
    bool configured() const { return !org_.empty(); }
    bool remembered() { return load(); }
    int tier() const { return tier_; }
    std::string maskedKey() const { return key_.size() > 6 ? "••••" + key_.substr(key_.size() - 6) : key_; }
    void activate(std::string key) {
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return isspace(c); }), key.end());
        if (key.empty()) throw std::runtime_error("Paste the Pro key from your Polar receipt.");
        key_ = key;
        activation_.clear();
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(name, &n);
        std::string label = utf8(name);
        if (label.empty()) label = "Windows PC";
        auto r = post(L"/v1/customer-portal/license-keys/activate",
                      "{" + base() + ",\"label\":" + jsonString(label) + ",\"meta\":{\"app\":\"LunarTweaks 2.0\"}}");
        if (r.status == 200) {
            activation_ = Json::parse(r.body).str("id");
        } else if (r.status == 404) {
            throw std::runtime_error("That key wasn't found. Copy it exactly from your Polar receipt email.");
        } else if (r.status == 403) {
            // Either the key has no device limit (validate directly) or every activation is used.
            try { validateOnline(); return; } catch (const NetworkError &) { throw; } catch (...) {}
            throw std::runtime_error("This key is already active on its maximum number of PCs. Remove an old PC in your Polar customer portal (link in your receipt), then try again.");
        } else if (r.status == 422) {
            throw std::runtime_error("That doesn't look like a LunarTweaks Pro key.");
        } else {
            throw std::runtime_error("Polar rejected the activation (HTTP " + std::to_string(r.status) + "). " + detail(r.body));
        }
        validateOnline();
    }
    // Startup: silently restore a remembered key.
    void resume() {
        if (!load()) throw std::runtime_error("No saved key");
        try { validateOnline(); }
        catch (const NetworkError &) {
            if (lastOk_ && now() - lastOk_ < kOfflineGrace) { tier_ = 2; return; }
            throw;
        }
    }
    int authorize() {
        if (key_.empty()) throw std::runtime_error("Activate Pro first.");
        if (tier_ == 2 && lastCheck_ && GetTickCount64() - lastCheck_ < 10 * 60 * 1000) return 2;
        try { validateOnline(); }
        catch (const NetworkError &) {
            if (lastOk_ && now() - lastOk_ < kOfflineGrace) { tier_ = 2; lastCheck_ = GetTickCount64(); return 2; }
            throw;
        }
        return tier_;
    }
    void signOut() {
        if (!key_.empty() && !activation_.empty()) {
            try { post(L"/v1/customer-portal/license-keys/deactivate", "{\"key\":" + jsonString(key_) + ",\"organization_id\":" + jsonString(utf8(org_)) + ",\"activation_id\":" + jsonString(activation_) + "}"); } catch (...) {}
        }
        DeleteFileW(keyFile().c_str());
        SecureZeroMemory(key_.data(), key_.size());
        key_.clear(); activation_.clear(); lastOk_ = 0; lastCheck_ = 0; tier_ = 0;
    }
};
} // namespace lt
