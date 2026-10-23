// Executes the production registry transaction code against an in-memory registry fake.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
using DWORD = uint32_t; using BYTE = unsigned char; using LSTATUS = long;
struct HKEY__ { int hive; std::wstring path; }; using HKEY = HKEY__ *;
HKEY__ machineHive{0, L""}, userHive{1, L""};
#define HKEY_LOCAL_MACHINE (&machineHive)
#define HKEY_CURRENT_USER (&userHive)
constexpr DWORD REG_SZ = 1, REG_EXPAND_SZ = 2, REG_DWORD = 4, KEY_QUERY_VALUE = 1, KEY_SET_VALUE = 2, KEY_WOW64_64KEY = 0x100;
constexpr LSTATUS ERROR_SUCCESS = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5;
struct Stored { DWORD type; std::vector<BYTE> data; };
std::map<std::tuple<int, std::wstring>, std::map<std::wstring, Stored>> keys;
bool denyMachine = false; int failAfter = -1, setCount = 0;
std::filesystem::path testDir;
LSTATUS RegOpenKeyExW(HKEY h, const wchar_t *p, DWORD, DWORD, HKEY *out) {
    if (!keys.count({h->hive, p})) return ERROR_FILE_NOT_FOUND;
    *out = new HKEY__{h->hive, p}; return ERROR_SUCCESS;
}
LSTATUS RegCreateKeyExW(HKEY h, const wchar_t *p, DWORD, void *, DWORD, DWORD, void *, HKEY *out, void *) {
    if (denyMachine && h->hive == 0) return ERROR_ACCESS_DENIED;
    keys[{h->hive, p}]; *out = new HKEY__{h->hive, p}; return ERROR_SUCCESS;
}
LSTATUS RegQueryValueExW(HKEY k, const wchar_t *n, void *, DWORD *type, BYTE *data, DWORD *size) {
    auto &m = keys[{k->hive, k->path}]; auto it = m.find(n);
    if (it == m.end()) return ERROR_FILE_NOT_FOUND;
    *type = it->second.type;
    if (data) { if (*size < it->second.data.size()) return 234; memcpy(data, it->second.data.data(), it->second.data.size()); }
    *size = DWORD(it->second.data.size()); return ERROR_SUCCESS;
}
LSTATUS RegSetValueExW(HKEY k, const wchar_t *n, DWORD, DWORD type, const BYTE *data, DWORD size) {
    if (++setCount == failAfter) return 87;
    keys[{k->hive, k->path}][n] = {type, std::vector<BYTE>(data, data + size)}; return ERROR_SUCCESS;
}
LSTATUS RegDeleteValueW(HKEY k, const wchar_t *n) {
    if (denyMachine && k->hive == 0) return ERROR_ACCESS_DENIED;
    return keys[{k->hive, k->path}].erase(n) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}
void RegCloseKey(HKEY k) { delete k; }
bool DeleteFileW(const wchar_t *p) { return std::filesystem::remove(std::filesystem::path(p)); }
namespace lt {
std::wstring dataDir() { return testDir.wstring(); }
std::array<BYTE, 32> sha(const std::string &s) { std::array<BYTE, 32> r{}; uint32_t h = 2166136261u; for (unsigned char c : s) h = (h ^ c) * 16777619u; for (int i = 0; i < 32; i++) r[i] = BYTE((h >> (i % 4 * 8)) ^ i); return r; }
void durableWrite(const std::wstring &p, const void *d, DWORD n, bool exclusive = false) { if (exclusive && std::filesystem::exists(p)) throw std::runtime_error("exists"); std::ofstream f(std::filesystem::path(p), std::ios::binary); f.write((const char *)d, n); }
std::string readFile(const std::wstring &p) { std::ifstream f(std::filesystem::path(p), std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
}
#define LUNAR_REG_TEST
#include "../app/reg_tweaks.hpp"
template <class F> void throws(F f) { bool c = false; try { f(); } catch (const std::exception &) { c = true; } assert(c); }
void reset() { keys.clear(); denyMachine = false; failAfter = -1; setCount = 0; std::filesystem::remove_all(testDir); std::filesystem::create_directories(testDir / "backups"); }
int main() {
    using namespace lt::reg;
    testDir = std::filesystem::temp_directory_path() / ("lunar-reg-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    // Values that existed come back exactly; values that did not exist are deleted again.
    reset();
    set(Machine, L"A", L"Existing", REG_DWORD, encode(dword(Machine, L"", L"", 7)));
    set(User, L"B", L"Text", REG_SZ, encode(string(User, L"", L"", L"400")));
    Plan p{{dword(Machine, L"A", L"Existing", 1), dword(Machine, L"A", L"New", 2), string(User, L"B", L"Text", L"0")}, L"", L""};
    assert(!allMatch(p)); apply(201, p); assert(allMatch(p) && hasBackup(201));
    apply(201, p); // idempotent
    restore(201); assert(!hasBackup(201));
    assert(display(read(Machine, L"A", L"Existing")) == L"7"); assert(!read(Machine, L"A", L"New").exists);
    assert(asText(read(User, L"B", L"Text")) == L"400");
    // A failed write rolls back everything written in this run and leaves no recovery file.
    reset(); set(Machine, L"A", L"X", REG_DWORD, encode(dword(Machine, L"", L"", 5))); setCount = 0; failAfter = 2;
    Plan q{{dword(Machine, L"A", L"X", 1), dword(Machine, L"A", L"Y", 1)}, L"", L""};
    throws([&] { apply(202, q); }); assert(display(read(Machine, L"A", L"X")) == L"5" && !read(Machine, L"A", L"Y").exists && !hasBackup(202));
    // Missing admin rights: clear failure, nothing changed, no recovery file.
    reset(); denyMachine = true; throws([&] { apply(203, q); }); assert(!hasBackup(203) && !read(Machine, L"A", L"X").exists);
    // Re-applying after a later edit keeps the FIRST originals.
    reset(); set(Machine, L"A", L"X", REG_DWORD, encode(dword(Machine, L"", L"", 9)));
    Plan one{{dword(Machine, L"A", L"X", 1)}, L"", L""}; apply(204, one);
    set(Machine, L"A", L"X", REG_DWORD, encode(dword(Machine, L"", L"", 3))); apply(204, one); restore(204);
    assert(display(read(Machine, L"A", L"X")) == L"9");
    // Tampered recovery files are refused and kept.
    reset(); apply(205, one); { auto raw = lt::readFile(backupFile(205)); raw[10] ^= 1; lt::durableWrite(backupFile(205), raw.data(), DWORD(raw.size())); }
    throws([] { restore(205); }); assert(hasBackup(205));
    std::filesystem::remove_all(testDir);
    std::cout << "PASS: registry apply/restore, missing values, rollback, admin denial, first-original retention, tamper detection.\n";
}
