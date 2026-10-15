#pragma once
#include <string_view>

// Config-only Windows binding contract; no IPC layout or engine runtime dependency.
namespace FanyImePunctuationHotkey
{
constexpr unsigned Shift = 1, Control = 2, Alt = 4, Win = 8;
constexpr std::string_view Default = "Ctrl+Period";
struct Binding
{
    unsigned key = 0;
    unsigned modifiers = 0;
    constexpr bool Matches(unsigned code, unsigned pressed, bool rightAlt = false) const
    {
        return key != 0 && key == code && modifiers == pressed && (!(modifiers & Alt) || !rightAlt);
    }
};
constexpr Binding DefaultBinding{0xbe, Control};

inline unsigned KeyCode(std::string_view code)
{
    if (code.size() == 4 && code.substr(0, 3) == "Key" && code[3] >= 'A' && code[3] <= 'Z')
        return static_cast<unsigned>(code[3]);
    if (code.size() == 6 && code.substr(0, 5) == "Digit" && code[5] >= '0' && code[5] <= '9')
        return static_cast<unsigned>(code[5]);
    if (code.size() == 2 && code[0] == 'F' && code[1] >= '1' && code[1] <= '9')
        return 0x70 + static_cast<unsigned>(code[1] - '1');
    if (code == "F10" || code == "F11" || code == "F12")
        return 0x79 + static_cast<unsigned>(code[2] - '0');
    constexpr struct
    {
        std::string_view name;
        unsigned key;
    } punctuation[] = {{"Semicolon", 0xba}, {"Equal", 0xbb},        {"Comma", 0xbc},     {"Minus", 0xbd},
                       {"Period", 0xbe},    {"Slash", 0xbf},        {"Backquote", 0xc0}, {"BracketLeft", 0xdb},
                       {"Backslash", 0xdc}, {"BracketRight", 0xdd}, {"Quote", 0xde}};
    for (const auto &entry : punctuation)
        if (entry.name == code)
            return entry.key;
    return 0;
}

// Empty explicitly disables. Only canonical Ctrl/Alt[+Shift]+code is accepted;
// Alt means left Alt; right Alt is used by voice input / AltGr.
// Ctrl+Alt (including AltGr), Win and product-owned chords are not bindable here.
inline bool Parse(std::string_view value, Binding &result)
{
    result = {};
    if (value.empty())
        return true;
    unsigned modifiers = 0;
    if (value.substr(0, 5) == "Ctrl+")
        modifiers = Control;
    else if (value.substr(0, 4) == "Alt+")
        modifiers = Alt;
    else
        return false;
    value.remove_prefix(modifiers == Control ? 5 : 4);
    if (value.substr(0, 6) == "Shift+")
    {
        modifiers |= Shift;
        value.remove_prefix(6);
    }
    const unsigned key = KeyCode(value);
    if (key == 0 || (modifiers == (Control | Shift) && (key == 'E' || key == 'F')) ||
        ((modifiers & Control) && key == 0x78) || ((modifiers & Alt) && key == 0x73))
        return false; // Ctrl+Shift+E/F, voice Ctrl[+Shift]+F9, and Alt[+Shift]+F4.
    result = {key, modifiers};
    return true;
}

// The DLL's lightweight TOML reader must distinguish explicit empty/invalid
// values from a missing key. Both TOML string quoting styles are supported.
inline Binding FromTomlValue(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos)
        return {};
    value = value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
    Binding result;
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        Parse(value.substr(1, value.size() - 2), result);
    return result;
}
} // namespace FanyImePunctuationHotkey
