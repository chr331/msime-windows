#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure delivery-result policy for the Windows SendInput adapters. Keeping the
// classification here makes the zero/partial/full outcomes testable without
// injecting real user input.
namespace VoiceInput
{
// One SendInput call delivers only when every requested event was inserted.
inline bool AllInputEventsInjected(std::uint32_t requested, std::uint32_t injected)
{
    return requested != 0 && injected == requested;
}

// Send one Unicode character as a key-down/key-up pair. `send` returns the
// number of events SendInput accepted for that character. A full count (2)
// advances to the next character. An accepted key-down only (1) is repaired by
// one KEYUP for the same UTF-16 unit through `cleanup`; zero accepted events
// needs no cleanup. Either short count stops delivery immediately and is never
// counted as fully delivered. The cleanup callback returns the number of
// events SendInput accepted for the KEYUP (1 on success); its result is
// diagnostic only and never turns a partial character into a delivered one.
template <typename SendUnicodeCharacter, typename CleanupUnicodeCharacter>
std::size_t SendUnicodeTextWith(const std::wstring &text, SendUnicodeCharacter send, CleanupUnicodeCharacter cleanup)
{
    std::size_t delivered = 0;
    for (wchar_t ch : text)
    {
        const std::uint32_t injected = send(ch);
        if (AllInputEventsInjected(2, injected))
        {
            ++delivered;
            continue;
        }
        if (injected == 1)
        {
            cleanup(ch);
        }
        break;
    }
    return delivered;
}

// Backward-compatible entry point for callers that inject a character pair
// atomically and therefore have no partial key-down to release.
template <typename SendUnicodeCharacter>
std::size_t SendUnicodeTextWith(const std::wstring &text, SendUnicodeCharacter send)
{
    return SendUnicodeTextWith(text, send, [](wchar_t) -> std::uint32_t { return 0; });
}

// Ctrl+V is sent as four events: Ctrl down, V down, V up, Ctrl up. Only all
// four count as a delivered paste. A partial prefix still identifies the keys
// that may need key-up cleanup; cleanup must never be reported as success.
struct CtrlVInjectionOutcome
{
    bool delivered = false;
    bool ctrl_key_down = false;
    bool v_key_down = false;
};

inline CtrlVInjectionOutcome ClassifyCtrlVInjection(std::uint32_t injected)
{
    CtrlVInjectionOutcome outcome;
    outcome.delivered = injected == 4;
    outcome.ctrl_key_down = injected >= 1 && injected < 4;
    outcome.v_key_down = injected >= 2 && injected < 3;
    return outcome;
}

// The result card is the user's Copy surface. Even when a TSF prefix reached
// the field before a later packet failed, the card must retain the complete
// original model text; callers must never replay the prefix automatically.
inline std::wstring TextForCardAfterTsfDelivery(const std::wstring &original, std::size_t delivered)
{
    (void)delivered;
    return original;
}
} // namespace VoiceInput
