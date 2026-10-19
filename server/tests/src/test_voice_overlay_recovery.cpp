#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <iostream>
#include <algorithm>
#include <cstdint>
#include "voice-input/voice_overlay_layout.h"
#define private public
#include "voice-input/wave_overlay.h"
#undef private
#include "voice-input/voice_result_router.h"
#include "voice-input/wave_overlay.cpp"

namespace mvi_utils
{
HMONITOR GetForegroundMonitor()
{
    return MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
}
MonitorMetrics GetForegroundMonitorMetrics()
{
    return {{0, 0, 1280, 720}, {0, 0, 1280, 680}};
}
float GetForegroundMonitorScale()
{
    return 1.0f;
}
} // namespace mvi_utils

namespace
{
int failures = 0, checks = 0;
void Check(bool value, const char *name)
{
    ++checks;
    std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
    if (!value)
        ++failures;
}
void Pump(unsigned milliseconds = 0)
{
    const auto deadline = GetTickCount64() + milliseconds;
    do
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= deadline)
            break;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - GetTickCount64()), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    } while (true);
}
void Click(WaveOverlay &overlay, int x, int y)
{
    auto point = MAKELPARAM(static_cast<int>(x * overlay.scale_x_), static_cast<int>(y * overlay.scale_y_));
    SendMessageW(overlay.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(overlay.hwnd_, WM_LBUTTONUP, 0, point);
}
} // namespace

int main()
{
    SetProcessDPIAware();
    WaveOverlay overlay;
    bool copy_ok = true;
    int copies = 0;
    std::wstring copied;
    Check(overlay.init(
              GetModuleHandleW(nullptr), [](WaveOverlay::Action) {},
              [&](HWND, std::uint64_t, const std::wstring &text) {
                  ++copies;
                  copied = text;
                  return copy_ok;
              }),
          "real overlay initializes");
    if (!overlay.is_available())
        return 1;
    overlay.set_session(1);
    std::wstring long_text(4097, L'x');
    long_text += L"\xD83D\xDE00";
    using namespace VoiceInput;
    int card_calls = 0;
    auto route = [&](bool available, RetainedResultPresentation result) {
        return RouteRetainedResult(
            1, long_text, available, overlay.is_available(),
            [&](std::uint64_t, const std::wstring &) {
                ++card_calls;
                return result;
            },
            [&](std::uint64_t session, const std::wstring &text) { return overlay.show_recovery(session, text); });
    };
    Check(route(false, RetainedResultPresentation::Unavailable) && card_calls == 0,
          "card unavailable routes to initialized overlay");
    Pump();
    Check(overlay.recovery_mode_ && IsWindowVisible(overlay.hwnd_), "fallback actually visible");
    Check(overlay.recovery_text_for_test() == long_text, "full long UTF16 result retained");
    Click(overlay, 150, 58);
    Check(copies == 1 && copied == long_text, "actual copy click receives complete text and surrogate pair");
    Check(std::wstring(overlay.recovery_copy_label()) == L"已复制", "success feedback shown before close");
    Pump(250);
    Check(IsWindowVisible(overlay.hwnd_), "success remains visible for reading");
    const UINT_PTR old_timer = overlay.recovery_timer_id_;
    Click(overlay, 150, 58);
    Check(overlay.recovery_timer_id_ != 0 && overlay.recovery_timer_id_ != old_timer,
          "repeated copy starts fresh timer");
    SendMessageW(overlay.hwnd_, WM_TIMER, old_timer, 0);
    Check(IsWindowVisible(overlay.hwnd_), "queued old timer cannot close refreshed result");
    Pump(1100);
    Check(!IsWindowVisible(overlay.hwnd_) && !overlay.recovery_active_for(1),
          "success automatically closes and clears model after one second");

    Check(route(true, RetainedResultPresentation::DeliveryFailed), "card post failure routes to overlay");
    Pump();
    copy_ok = false;
    Click(overlay, 150, 58);
    Check(std::wstring(overlay.recovery_copy_label()) == L"重试复制" && overlay.recovery_timer_id_ == 0,
          "failed copy offers retry without timer");
    Pump(1100);
    Check(IsWindowVisible(overlay.hwnd_) && overlay.recovery_text_for_test() == long_text,
          "failure preserves full text beyond close delay");
    copy_ok = true;
    Click(overlay, 150, 58);
    Check(overlay.recovery_timer_id_ != 0 && copied == long_text, "retry succeeds with same full text");
    const auto timer_a = overlay.recovery_timer_id_;
    const auto generation_a = overlay.recovery_generation_for_test();
    Check(overlay.show_recovery(1, L"new generation"), "same session can publish next generation");
    Pump();
    overlay.post_recovery_dismiss_for_test(1, generation_a);
    SendMessageW(overlay.hwnd_, WM_TIMER, timer_a, 0);
    Pump();
    Check(IsWindowVisible(overlay.hwnd_) && overlay.recovery_text_for_test() == L"new generation",
          "old dismiss and timer preserve same-session new generation");
    overlay.finish_recognition(1);
    overlay.hide();
    overlay.show();
    Pump();
    Check(IsWindowVisible(overlay.hwnd_) && overlay.recovery_mode_,
          "ordinary finish hide show preserve current recovery");
    Check(overlay.dismiss_recovery(1), "public dismiss accepts active recovery");
    Pump();
    Check(!IsWindowVisible(overlay.hwnd_) && !overlay.recovery_active_for(1),
          "public dismiss really hides already-cleared model");
    Check(!route(true, RetainedResultPresentation::Rejected), "model rejection is final");
    Pump();
    Check(!IsWindowVisible(overlay.hwnd_), "rejected card does not reopen fallback");

    overlay.show_recovery(1, L"displayed generation");
    Pump();
    overlay.show_recovery(1, L"pending replacement");
    overlay.dismiss_recovery(1);
    Pump();
    Check(!IsWindowVisible(overlay.hwnd_),
          "authoritative dismiss hides older visible generation while replacement pending");

    overlay.show_recovery(1, L"old session");
    Pump();
    overlay.finish_recognition(1);
    overlay.hide();
    overlay.set_session(2);
    overlay.show_recovery(2, L"new session");
    Pump();
    Check(IsWindowVisible(overlay.hwnd_) && overlay.recovery_text_for_test() == L"new session",
          "old queued finish hide cannot hide new session");
    overlay.dismiss_recovery(2);
    Pump();
    overlay.set_session(3);
    overlay.show();
    Pump();
    Check(IsWindowVisible(overlay.hwnd_) && !overlay.recovery_mode_, "ordinary recording show remains functional");
    overlay.hide();
    overlay.set_compact_status(WaveOverlay::CompactStatus::Recognizing);
    overlay.set_actions_visible(true);
    overlay.set_transcript(L"latest recognition state");
    overlay.show();
    Pump();
    Check(overlay.compact_status_.load() == WaveOverlay::CompactStatus::Recognizing &&
              overlay.actions_visible_.load() && overlay.transcript_ == L"latest recognition state",
          "queued hide preserves subsequently published recognition state");

    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool nonblocking = false;
    overlay.set_finish_recognition_ui_test_hook([&] {
        SetEvent(entered);
        nonblocking = WaitForSingleObject(returned, 1000) == WAIT_OBJECT_0;
    });
    std::thread control([&] {
        if (WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0)
        {
            overlay.show();
            overlay.hide();
            SetEvent(returned);
        }
    });
    overlay.finish_recognition(3);
    Pump();
    control.join();
    Check(nonblocking, "control show hide return while UI completion is deliberately blocked");
    CloseHandle(entered);
    CloseHandle(returned);
    Pump();
    Check(!IsWindowVisible(overlay.hwnd_), "ordinary hide remains functional");
    overlay.shutdown();
    Check(!overlay.is_available(), "shutdown withdraws publication");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
