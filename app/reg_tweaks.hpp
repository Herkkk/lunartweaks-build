#pragma once
// Reversible registry tweaks.
// Every value a tweak touches is captured (including "did not exist") in a sealed local
// recovery file BEFORE the first write. Restore puts back the exact original type and data,
// or deletes values that did not exist. Partial failures roll back the current run.
#ifndef LUNAR_REG_TEST
#include "util.hpp"
#endif
#include <cstring>
#include <string>
#include <vector>
namespace lt::reg {
enum Root : DWORD { Machine = 0, User = 1 };
struct Write {
    Root root;
    std::wstring path, name;
    DWORD type = REG_DWORD;
    DWORD number = 0;
    std::wstring text;
};
inline Write dword(Root r, std::wstring path, std::wstring name, DWORD v) {
    return {r, std::move(path), std::move(name), REG_DWORD, v, L""};
}
inline Write string(Root r, std::wstring path, std::wstring name, std::wstring v) {
    return {r, std::move(path), std::move(name), REG_SZ, 0, std::move(v)};
}
struct Plan {
    std::vector<Write> writes;
    std::wstring headline; // Short, PC-specific line ("Unpark all 16 threads").
    std::wstring note;     // Longer explanation of how the values were matched.
};
// Thrown by planners when a tweak does not fit this PC. The message is shown to the user.
struct Unavailable : std::runtime_error {
    std::wstring reason;
    explicit Unavailable(std::wstring why) : std::runtime_error("Not available on this PC"), reason(std::move(why)) {}
};
inline HKEY hive(Root r) { return r == Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }
struct Value {
    bool exists = false;
    DWORD type = 0;
    std::vector<BYTE> data;
};
inline Value read(Root root, const std::wstring &path, const std::wstring &name) {
    Value v;
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive(root), path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return v;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size <= 65536) {
        v.data.resize(size);
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type, size ? v.data.data() : nullptr, &size) ==
            ERROR_SUCCESS) {
            v.exists = true;
            v.type = type;
            v.data.resize(size);
        } else
            v.data.clear();
    }
    RegCloseKey(key);
    return v;
}
inline std::vector<BYTE> encode(const Write &w) {
    std::vector<BYTE> out;
    if (w.type == REG_DWORD) {
        out.resize(4);
        memcpy(out.data(), &w.number, 4);
    } else {
        const BYTE *p = reinterpret_cast<const BYTE *>(w.text.c_str());
        out.assign(p, p + (w.text.size() + 1) * sizeof(wchar_t));
    }
    return out;
}
inline std::wstring asText(const Value &v) {
    if (!v.exists || (v.type != REG_SZ && v.type != REG_EXPAND_SZ)) return {};
    std::wstring s(v.data.size() / sizeof(wchar_t), L'\0');
    if (!s.empty()) memcpy(s.data(), v.data.data(), s.size() * sizeof(wchar_t));
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}
inline bool matches(const Write &w) {
    auto v = read(w.root, w.path, w.name);
    if (!v.exists) return false;
    if (w.type == REG_DWORD) {
        DWORD n = 0;
        if (v.type != REG_DWORD || v.data.size() != 4) return false;
        memcpy(&n, v.data.data(), 4);
        return n == w.number;
    }
    return (v.type == REG_SZ || v.type == REG_EXPAND_SZ) && asText(v) == w.text;
}
inline bool allMatch(const Plan &p) {
    for (auto &w : p.writes)
        if (!matches(w)) return false;
    return !p.writes.empty();
}
inline std::wstring display(const Value &v) {
    if (!v.exists) return L"not set";
    if (v.type == REG_DWORD && v.data.size() == 4) {
        DWORD n;
        memcpy(&n, v.data.data(), 4);
        return std::to_wstring(n);
    }
    if (v.type == REG_SZ || v.type == REG_EXPAND_SZ) return L"\"" + asText(v) + L"\"";
    return L"(binary)";
}
inline std::wstring display(const Write &w) {
    return w.type == REG_DWORD ? std::to_wstring(w.number) : L"\"" + w.text + L"\"";
}
inline void set(Root root, const std::wstring &path, const std::wstring &name, DWORD type,
                const std::vector<BYTE> &data) {
    HKEY key = nullptr;
    LSTATUS rc = RegCreateKeyExW(hive(root), path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY,
                                 nullptr, &key, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, name.c_str(), 0, type, data.empty() ? nullptr : data.data(), DWORD(data.size()));
        RegCloseKey(key);
    }
    if (rc == ERROR_ACCESS_DENIED)
        throw std::runtime_error("Windows needs administrator rights for this tweak. Click \"Run as admin\" in the top bar, then apply again.");
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Windows rejected a registry change (a policy may manage this setting).");
}
inline void erase(Root root, const std::wstring &path, const std::wstring &name) {
    HKEY key = nullptr;
    LSTATUS rc = RegOpenKeyExW(hive(root), path.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return;
    if (rc == ERROR_SUCCESS) {
        rc = RegDeleteValueW(key, name.c_str());
        RegCloseKey(key);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    if (rc == ERROR_ACCESS_DENIED)
        throw std::runtime_error("Windows needs administrator rights to restore this value. Click \"Run as admin\" in the top bar.");
    if (rc != ERROR_SUCCESS) throw std::runtime_error("Windows rejected the registry restore.");
}
// ---- Sealed recovery file -------------------------------------------------------------
struct Saved {
    Root root;
    std::wstring path, name;
    Value original;
};
inline std::wstring backupFile(int id) { return dataDir() + L"\\backups\\reg-" + std::to_wstring(id) + L".ltr"; }
inline bool hasBackup(int id) { return std::filesystem::exists(std::filesystem::path(backupFile(id))); }
inline void put(std::string &o, DWORD v) { o.append(reinterpret_cast<const char *>(&v), 4); }
inline void putText(std::string &o, const std::wstring &s) {
    put(o, DWORD(s.size()));
    o.append(reinterpret_cast<const char *>(s.data()), s.size() * sizeof(wchar_t));
}
inline std::string serialize(int id, const std::vector<Saved> &items) {
    std::string o = "LTR1";
    put(o, DWORD(id));
    put(o, DWORD(items.size()));
    for (auto &i : items) {
        put(o, i.root);
        putText(o, i.path);
        putText(o, i.name);
        put(o, i.original.exists);
        put(o, i.original.type);
        put(o, DWORD(i.original.data.size()));
        o.append(reinterpret_cast<const char *>(i.original.data.data()), i.original.data.size());
    }
    auto h = sha(o);
    o.append(reinterpret_cast<const char *>(h.data()), 32);
    return o;
}
inline std::vector<Saved> loadBackup(int id) {
    auto raw = readFile(backupFile(id));
    auto fail = [] { throw std::runtime_error("Recovery file validation failed; it has been kept unchanged."); };
    if (raw.size() < 44 || raw.compare(0, 4, "LTR1") != 0) fail();
    auto h = sha(raw.substr(0, raw.size() - 32));
    if (memcmp(h.data(), raw.data() + raw.size() - 32, 32) != 0) fail();
    size_t p = 4, end = raw.size() - 32;
    auto get = [&]() {
        if (p + 4 > end) fail();
        DWORD v;
        memcpy(&v, raw.data() + p, 4);
        p += 4;
        return v;
    };
    auto getText = [&]() {
        DWORD n = get();
        if (n > 4096 || p + size_t(n) * sizeof(wchar_t) > end) fail();
        std::wstring s(n, L'\0');
        if (n) memcpy(s.data(), raw.data() + p, n * sizeof(wchar_t));
        p += n * sizeof(wchar_t);
        return s;
    };
    if (get() != DWORD(id)) fail();
    DWORD count = get();
    if (count == 0 || count > 64) fail();
    std::vector<Saved> items;
    for (DWORD i = 0; i < count; ++i) {
        Saved s;
        DWORD root = get();
        if (root > 1) fail();
        s.root = Root(root);
        s.path = getText();
        s.name = getText();
        s.original.exists = get() != 0;
        s.original.type = get();
        DWORD n = get();
        if (n > 65536 || p + n > end) fail();
        s.original.data.assign(raw.begin() + p, raw.begin() + p + n);
        p += n;
        items.push_back(std::move(s));
    }
    if (p != end) fail();
    return items;
}
inline void putBack(const Saved &s) {
    if (s.original.exists) set(s.root, s.path, s.name, s.original.type, s.original.data);
    else erase(s.root, s.path, s.name);
}
// ---- Transactions -----------------------------------------------------------------------
inline void apply(int id, const Plan &plan) {
    if (plan.writes.empty()) throw std::runtime_error("Nothing to change on this PC.");
    if (allMatch(plan)) return;
    std::vector<Saved> originals;
    for (auto &w : plan.writes) originals.push_back({w.root, w.path, w.name, read(w.root, w.path, w.name)});
    bool created = false;
    if (hasBackup(id)) {
        // Keep the very first originals. Add any value the saved file does not cover yet.
        auto saved = loadBackup(id);
        bool extended = false;
        for (auto &o : originals) {
            bool known = false;
            for (auto &s : saved)
                if (s.root == o.root && s.path == o.path && s.name == o.name) known = true;
            if (!known) {
                saved.push_back(o);
                extended = true;
            }
        }
        if (extended) {
            auto blob = serialize(id, saved);
            durableWrite(backupFile(id), blob.data(), DWORD(blob.size()));
        }
    } else {
        auto blob = serialize(id, originals);
        durableWrite(backupFile(id), blob.data(), DWORD(blob.size()), true);
        created = true;
    }
    size_t done = 0;
    try {
        for (auto &w : plan.writes) {
            set(w.root, w.path, w.name, w.type, encode(w));
            ++done;
        }
        if (!allMatch(plan)) throw std::runtime_error("Windows did not keep the new values; changes were rolled back.");
    } catch (...) {
        for (size_t i = done; i-- > 0;) {
            try { putBack(originals[i]); } catch (...) {}
        }
        if (created) DeleteFileW(backupFile(id).c_str());
        throw;
    }
}
inline void restore(int id) {
    auto saved = loadBackup(id);
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) putBack(*it);
    if (!DeleteFileW(backupFile(id).c_str()))
        throw std::runtime_error("Restored, but the recovery file could not be removed.");
}
inline std::wstring rootName(Root r) { return r == Machine ? L"HKLM" : L"HKCU"; }
inline bool needsAdmin(const Plan &p) {
    for (auto &w : p.writes)
        if (w.root == Machine) return true;
    return false;
}
inline std::wstring describe(const Plan &p) {
    std::wstring s;
    for (auto &w : p.writes) {
        auto now = read(w.root, w.path, w.name);
        s += w.name + L":  " + display(now) + L"  →  " + display(w) + L"\n";
    }
    return s;
}
} // namespace lt::reg
