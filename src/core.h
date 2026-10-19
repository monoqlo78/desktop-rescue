#pragma once
#include <string>
#include <string_view>

namespace rescue {
inline bool IsRecoveryKey(unsigned key, unsigned flags, bool keyDown, unsigned modifiers = 0) {
    return keyDown && !modifiers && !(flags & 0x32) && (key == 0x1B || key == 0x24);
}

inline unsigned ModifierBit(unsigned key) {
    switch (key) {
    case 0x10: case 0xA0: return 1;
    case 0xA1: return 2;
    case 0x11: case 0xA2: return 4;
    case 0xA3: return 8;
    case 0x12: case 0xA4: return 16;
    case 0xA5: return 32;
    case 0x5B: return 64;
    case 0x5C: return 128;
    default: return 0;
    }
}

inline bool IsSecureDesktop(std::wstring_view name) {
    return name == L"Winlogon" || name == L"Disconnect";
}

struct InputDesktop {
    bool accessible = false;
    std::wstring name;
    unsigned long error = 0;
};

struct RecoveryResult {
    bool ok = false;
    std::string code;
    std::wstring before;
    unsigned long error = 0;
};

// Ops is injectable so failure/race/verification tests never switch real desktops.
template<class Ops>
RecoveryResult Recover(Ops& ops, const std::wstring& baseline,
                       const std::wstring& expected = {}) {
    auto input = ops.Input();
    if (!input.accessible)
        return {false, "input_inaccessible", {}, input.error};
    if (IsSecureDesktop(input.name))
        return {false, "secure_desktop", input.name, 0};
    if (!expected.empty() && input.name != expected)
        return {false, "input_changed", input.name, 0};
    if (input.name == baseline)
        return {true, "already_on_baseline", input.name, 0};
    const auto before = input.name;
    if (!ops.Switch())
        return {false, "switch_failed", before, ops.Error()};
    for (int i = 0; i < 20; ++i) {
        input = ops.Input();
        if (input.accessible && input.name == baseline)
            return {true, "recovered_verified", before, 0};
        ops.Wait();
    }
    return {false, "verification_failed", before, input.error};
}

inline std::string JsonEscape(std::string_view value) {
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                out += "\\u00";
                out += hex[ch >> 4];
                out += hex[ch & 15];
            } else out += static_cast<char>(ch);
        }
    }
    return out;
}
}
