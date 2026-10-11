#include "../../engine/contracts/punctuation_hotkey.h"
#include <iostream>

int main()
{
    using namespace FanyImePunctuationHotkey;
    Binding binding;
    const auto check = [](bool ok, const char *message) {
        if (!ok)
            std::cerr << message << '\n';
        return ok;
    };
    if (!check(Parse(Default, binding) && binding.Matches(0xbe, Control), "default binding") ||
        !check(DefaultBinding.Matches(0xbe, Control), "missing key keeps default") ||
        !check(Parse("", binding) && !binding.Matches(0xbe, Control), "empty disables") ||
        !check(Parse("Ctrl+Shift+Period", binding) && binding.Matches(0xbe, Control | Shift) &&
                   !binding.Matches(0xbe, Control),
               "rebind releases old chord"))
        return 1;
    for (unsigned modifiers = 0; modifiers < 16; ++modifiers)
        if (!check(binding.Matches(0xbe, modifiers) == (modifiers == (Control | Shift)), "exact modifiers"))
            return 1;
    for (const auto value : {"Ctrl+Shift+KeyE", "Ctrl+Shift+KeyF", "Ctrl+F9", "Alt+F4", "Ctrl+Space",
                             "Ctrl+Shift+Space", "Alt+Space", "Ctrl+Alt+Period", "Win+Period", "Shift+KeyA", "Ctrl+",
                             "Ctrl+F13", "Ctrl+Digit10", "Ctrl+Keya", "Period", "true"})
        if (!check(!Parse(value, binding) && binding.key == 0, value))
            return 1;
    for (const auto value : {"Ctrl+KeyA", "Alt+Shift+Digit3", "Alt+F12", "Ctrl+BracketLeft", "Ctrl+Quote"})
        if (!check(Parse(value, binding) && binding.key != 0, value))
            return 1;
    if (!check(KeyCode("F1") == 0x70 && KeyCode("F9") == 0x78 && KeyCode("F10") == 0x79 && KeyCode("F12") == 0x7b &&
                   KeyCode("Digit0") == '0' && KeyCode("KeyZ") == 'Z',
               "VK mapping") ||
        !check(FromTomlValue(" \"Ctrl+Period\" \r").Matches(0xbe, Control), "basic TOML string") ||
        !check(FromTomlValue("'Alt+Comma'").Matches(0xbc, Alt), "literal TOML string"))
        return 1;
    for (const auto value : {"\"\"", "true", "190", "\"Ctrl+Alt+Period\"", "[]", "\"Ctrl+Period"})
        if (!check(FromTomlValue(value).key == 0, "explicit invalid/empty must not restore default"))
            return 1;
    return 0;
}
