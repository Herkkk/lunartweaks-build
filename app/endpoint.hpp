#pragma once
#include <string_view>
namespace lt {
// The private owner launcher is the only supported plaintext endpoint.
// No DNS names, user info, external addresses, query strings or API paths.
inline unsigned ownerLoopbackPort(std::wstring_view origin, bool explicitlyEnabled) {
    constexpr std::wstring_view prefix = L"http://127.0.0.1:";
    if (!explicitlyEnabled || !origin.starts_with(prefix))
        return 0;
    auto tail = origin.substr(prefix.size());
    if (tail.ends_with(L"/"))
        tail.remove_suffix(1);
    if (tail.empty() || tail.size() > 5)
        return 0;
    unsigned port = 0;
    for (wchar_t c : tail) {
        if (c < L'0' || c > L'9')
            return 0;
        port = port * 10 + static_cast<unsigned>(c - L'0');
    }
    return port >= 1 && port <= 65535 ? port : 0;
}
} // namespace lt
