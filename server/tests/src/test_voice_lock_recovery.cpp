// Exercises the real voice_input_service.cpp control queue and keyboard-hook
// entry. Only external effects are substituted: microphone, overlay, cue
// player, Doubao client, IPC, config reads and logging.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <msime/voice/audio_capture.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "tests/includes/test_framework.h"
#include "utils/common_utils.h"
#include "utils/network_proxy.h"
#include "voice-input/cue_player.h"
#include "voice-input/doubao_asr_client.h"
#include "voice-input/voice_input_gesture.h"
#include "voice-input/voice_input_service.h"
#include "voice-input/wave_overlay.h"

namespace
{
using std::chrono::milliseconds;

std::mutex g_effect_mutex;
std::condition_variable g_effect_cv;
std::atomic<bool> g_actions_visible{false};
std::atomic<int> g_actions_true_count{0};
std::atomic<int> g_hide_count{0};

void NotifyEffects()
{
    g_effect_cv.notify_all();
}

void ResetObservations()
{
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    g_actions_visible = false;
    g_actions_true_count = 0;
    g_hide_count = 0;
}

void PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template <typename Predicate> bool WaitFor(Predicate predicate, milliseconds timeout = milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        PumpMessages();
        std::unique_lock<std::mutex> lock(g_effect_mutex);
        if (g_effect_cv.wait_for(lock, milliseconds(5), predicate))
            return true;
    }
    PumpMessages();
    return predicate();
}

std::mutex g_capture_mutex;
std::condition_variable g_capture_cv;
bool g_capture_entered = false;
bool g_capture_release = false;
int g_capture_stop_count = 0;

void ResetCapture()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    g_capture_entered = false;
    g_capture_release = false;
}

bool WaitCaptureEntered()
{
    return WaitFor([] {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        return g_capture_entered;
    });
}

void AllowCaptureStart()
{
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        g_capture_release = true;
    }
    g_capture_cv.notify_all();
    NotifyEffects();
}

int CaptureStopCount()
{
    std::lock_guard<std::mutex> lock(g_capture_mutex);
    return g_capture_stop_count;
}

std::mutex g_config_mutex;
std::condition_variable g_config_cv;
VoiceInputConfig g_config;
bool g_block_next_config_read = false;
bool g_config_read_entered = false;
bool g_config_read_release = false;
std::atomic<int> g_config_read_count{0};

void SetConfig(const VoiceInputConfig &config)
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config = config;
}

void BlockNextConfigRead()
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_block_next_config_read = true;
    g_config_read_entered = false;
    g_config_read_release = false;
}

bool WaitConfigReadEntered()
{
    std::unique_lock<std::mutex> lock(g_config_mutex);
    return g_config_cv.wait_for(lock, milliseconds(5000), [] { return g_config_read_entered; });
}

void ReleaseConfigRead()
{
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config_read_release = true;
    g_config_cv.notify_all();
}

bool WaitConfigReadAfter(int previous)
{
    return WaitFor([previous] { return g_config_read_count.load() > previous; });
}

VoiceInputConfig TestConfig(bool enabled, bool hotkeys)
{
    VoiceInputConfig config;
    config.enabled = enabled;
    config.asr_provider = "openai";
    config.asr_tokens["openai"] = "test-token";
    config.hotkey_ralt = hotkeys;
    config.hotkey_ctrl_f9 = hotkeys;
    config.hotkey_ctrl_win = hotkeys;
    config.hotkey_rctrl_ralt = hotkeys;
    config.hotkey_hold_space_lock = hotkeys;
    config.start_sound = false;
    config.end_sound = false;
    config.mute_system_audio = false;
    config.polish_text = false;
    return config;
}

bool WaitRecording()
{
    return WaitFor([] { return VoiceInput::IsRecording(); });
}

bool WaitNormalStop(int stop_before, int hides_before)
{
    return WaitFor([stop_before, hides_before] {
        return CaptureStopCount() > stop_before && g_hide_count.load() >= hides_before + 2 && !g_actions_visible.load();
    });
}

LRESULT SendKey(UINT vk, bool down, bool system_key = false)
{
    KBDLLHOOKSTRUCT key{};
    key.vkCode = vk;
    const WPARAM message = down ? (system_key ? WM_SYSKEYDOWN : WM_KEYDOWN) : (system_key ? WM_SYSKEYUP : WM_KEYUP);
    return VoiceInput::Detail::KeyboardHookProc(HC_ACTION, message, reinterpret_cast<LPARAM>(&key));
}

void KeyDown(UINT vk, bool system_key = false)
{
    SendKey(vk, true, system_key);
}

void KeyUp(UINT vk, bool system_key = false)
{
    SendKey(vk, false, system_key);
}

void PressSpace()
{
    KeyDown(VK_SPACE);
    KeyUp(VK_SPACE);
}

void PressCtrlF9()
{
    KeyDown(VK_LCONTROL);
    KeyDown(VK_F9);
    KeyUp(VK_F9);
    KeyUp(VK_LCONTROL);
}

enum class Gesture
{
    RAlt,
    CtrlWin,
    RCtrlRAlt
};

void BeginGesture(Gesture gesture)
{
    switch (gesture)
    {
    case Gesture::RAlt:
        KeyDown(VK_RMENU, true);
        break;
    case Gesture::CtrlWin:
        KeyDown(VK_LCONTROL);
        KeyDown(VK_LWIN);
        break;
    case Gesture::RCtrlRAlt:
        KeyDown(VK_RCONTROL);
        KeyDown(VK_RMENU, true);
        break;
    }
}

void EndGesture(Gesture gesture)
{
    switch (gesture)
    {
    case Gesture::RAlt:
        KeyUp(VK_RMENU, true);
        break;
    case Gesture::CtrlWin:
        KeyUp(VK_LWIN);
        KeyUp(VK_LCONTROL);
        break;
    case Gesture::RCtrlRAlt:
        KeyUp(VK_RMENU, true);
        KeyUp(VK_RCONTROL);
        break;
    }
}

void StartGesture(Gesture gesture)
{
    ResetCapture();
    BeginGesture(gesture);
    PumpMessages();
    REQUIRE(WaitCaptureEntered());
}

void ReleaseTestKeys()
{
    KeyUp(VK_RMENU, true);
    KeyUp(VK_LCONTROL);
    KeyUp(VK_RCONTROL);
    KeyUp(VK_LWIN);
    KeyUp(VK_RWIN);
    KeyUp(VK_F9);
}

struct ServiceFixture
{
    ServiceFixture()
    {
        SetConfig(TestConfig(true, false));
        ResetCapture();
        ResetObservations();
        if (!VoiceInput::Initialize())
            throw std::runtime_error("VoiceInput::Initialize failed");
        VoiceInput::SetImeActive(true);
        // No OS hook was installed because the initial config disabled every
        // hotkey. Enable them for the direct handler calls only.
        SetConfig(TestConfig(true, true));
    }

    ~ServiceFixture()
    {
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            g_block_next_config_read = false;
            g_config_read_release = true;
        }
        g_config_cv.notify_all();
        {
            std::lock_guard<std::mutex> lock(g_capture_mutex);
            g_capture_release = true;
        }
        g_capture_cv.notify_all();
        // The real handler clears the physical-key suppression state that
        // Shutdown would otherwise release with SendInput.
        ReleaseTestKeys();
        VoiceInput::CancelRecording();
        VoiceInput::Shutdown();
    }

    ServiceFixture(const ServiceFixture &) = delete;
    ServiceFixture &operator=(const ServiceFixture &) = delete;
};
} // namespace

// Start is still initializing when Space requests the lock and the hold
// shortcut is released. The start must commit, and the next hold must choose
// Stop rather than leaving a lock with no recording.
TEST_CASE(space_during_startup_locks_and_next_gesture_stops)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt);
    PumpMessages();

    AllowCaptureStart();
    REQUIRE(WaitRecording());
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));
    REQUIRE(g_actions_visible.load());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    // Start + one Lock + Stop's batch-recognition state.
    REQUIRE_EQ(g_actions_true_count.load(), 3);

    // A fresh hold starts and its release stops: no stale lock remains.
    ResetObservations();
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int second_stop = CaptureStopCount();
    const int second_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(second_stop, second_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A Start that fails before touching the microphone must not leave a queued
// Lock that makes the next hold choose Stop.
TEST_CASE(failed_start_with_queued_lock_does_not_lock_next_gesture)
{
    ServiceFixture fixture;
    SetConfig(TestConfig(false, true));
    BlockNextConfigRead();

    // The public Start (not a hook call) takes the config gate.
    VoiceInput::StartRecording();
    REQUIRE(WaitConfigReadEntered());

    ResetCapture();
    BeginGesture(Gesture::RAlt); // queues a second, disabled Start
    PumpMessages();
    PressSpace(); // pending ticket
    PumpMessages();
    const int before_release = g_config_read_count.load();
    ReleaseConfigRead();
    // The queued Start reads the disabled config first; the Lock follows it.
    REQUIRE(WaitConfigReadAfter(before_release));

    // A Start queued after the Lock reaches the same read only after the Lock
    // has run, so this wait crosses the queue execution point in order.
    const int before_sentinel = g_config_read_count.load();
    VoiceInput::StartRecording();
    REQUIRE(WaitConfigReadAfter(before_sentinel));
    EndGesture(Gesture::RAlt);
    SetConfig(TestConfig(true, true));

    for (int i = 0; i < 3; ++i)
    {
        StartGesture(Gesture::RAlt);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int stop_before = CaptureStopCount();
        const int hides_before = g_hide_count.load();
        EndGesture(Gesture::RAlt);
        REQUIRE(WaitNormalStop(stop_before, hides_before));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// Cancelling a starting capture must invalidate its own queued Lock before a
// new Start exists. The stale Lock arriving after that Start must not lock the
// new recording, and the next hold must start rather than post Stop.
TEST_CASE(external_cancel_with_queued_lock_does_not_lock_next_gesture)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();                  // pending ticket; Lock message not dispatched
    VoiceInput::CancelRecording(); // carries that ticket
    VoiceInput::StartRecording();  // new Start before the old Lock is dispatched
    PumpMessages();                // dispatch the old Lock behind the new Start
    const int stop_before = CaptureStopCount();
    AllowCaptureStart(); // abort first Start, cancel, then start again
    REQUIRE(WaitRecording());

    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt); // the stale Lock must not keep this locked
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());

    for (int i = 0; i < 2; ++i)
    {
        StartGesture(Gesture::RAlt);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int loop_stop = CaptureStopCount();
        const int loop_hides = g_hide_count.load();
        EndGesture(Gesture::RAlt);
        REQUIRE(WaitNormalStop(loop_stop, loop_hides));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// Without Space, releasing the hold shortcut still aborts a starting capture
// and stops a committed recording.
TEST_CASE(plain_hold_release_aborts_startup_and_stops_recording)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    const int abort_stop = CaptureStopCount();
    EndGesture(Gesture::RAlt);
    // Deliver Stop while capture.start() is still gated, so this is the abort
    // path rather than a race with a just-committed recording.
    PumpMessages();
    AllowCaptureStart();
    REQUIRE(WaitFor([abort_stop] { return CaptureStopCount() > abort_stop; }));
    REQUIRE(!VoiceInput::IsRecording());

    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
}

// Ctrl+F9 toggles through the same hook and control queue.
TEST_CASE(ctrl_f9_toggle_starts_and_stops)
{
    ServiceFixture fixture;
    ResetCapture();
    PressCtrlF9();
    PumpMessages();
    REQUIRE(WaitCaptureEntered());
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    PressCtrlF9();
    PumpMessages();
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
}

// All three supported hold shortcuts share the hook's gesture lifecycle.
TEST_CASE(hold_shortcuts_share_gesture_lifecycle)
{
    ServiceFixture fixture;
    for (const Gesture gesture : {Gesture::RAlt, Gesture::CtrlWin, Gesture::RCtrlRAlt})
    {
        StartGesture(gesture);
        AllowCaptureStart();
        REQUIRE(WaitRecording());
        const int stop_before = CaptureStopCount();
        const int hides_before = g_hide_count.load();
        EndGesture(gesture);
        REQUIRE(WaitNormalStop(stop_before, hides_before));
        REQUIRE(!VoiceInput::IsRecording());
    }
}

// A Lock still waiting in the message queue is a real request: the next hold
// must end that recording instead of starting another one.
TEST_CASE(pending_lock_makes_next_hold_stop)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    PressSpace();              // pending; do not dispatch the Lock yet
    EndGesture(Gesture::RAlt); // pending must not be cancelled on release
    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();

    BeginGesture(Gesture::RAlt); // pending -> Stop, not Start
    PumpMessages();              // Lock then Stop
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    EndGesture(Gesture::RAlt);

    // A fresh hold starts and stops normally.
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A Stop created for an older request must not clear a newer pending Space
// request when it is finally executed before that request's Lock.
TEST_CASE(stale_stop_does_not_clear_next_space_request)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt); // first Start is gated in capture.start()

    PressSpace();                    // pending T1
    PumpMessages();                  // Lock T1 queued behind the blocked Start
    VoiceInput::StopRecording();     // Stop T1, still queued behind the Start
    VoiceInput::SetImeActive(false); // resets all lock state; queues Cancel 0
    VoiceInput::SetImeActive(true);
    ResetObservations();

    BeginGesture(Gesture::RAlt); // state 0 -> Start
    PressSpace();                // pending T2
    PumpMessages();              // dispatch Start and Lock T2
    AllowCaptureStart();         // abort first Start, then Cancel, Start, Lock

    REQUIRE(WaitRecording());
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));
    REQUIRE(g_actions_visible.load());

    const int stop_before = CaptureStopCount();
    const int hides_before = g_hide_count.load();
    EndGesture(Gesture::RAlt); // applied lock: release must not stop

    // The lock is still applied, so the next hold ends the recording instead
    // of starting another one; its stop completion is the evidence.
    BeginGesture(Gesture::RAlt);
    PumpMessages();
    REQUIRE(WaitNormalStop(stop_before, hides_before));
    REQUIRE(!VoiceInput::IsRecording());
    EndGesture(Gesture::RAlt);

    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// Switching IMEs while a Space-locked start is still initializing must abort
// that start instead of committing the recording after deactivation.
TEST_CASE(ime_deactivation_cancels_pending_space_locked_start)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt);
    PumpMessages();
    const int stop_before = CaptureStopCount();
    VoiceInput::SetImeActive(false);
    AllowCaptureStart();
    REQUIRE(WaitFor([stop_before] { return CaptureStopCount() > stop_before; }));
    REQUIRE(!VoiceInput::IsRecording());
}

// Disabling voice input while a Space-locked start is still initializing must
// abort that start, and the next hold must work after re-enabling it.
TEST_CASE(refresh_disabled_aborts_starting_space_locked_capture)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);

    PressSpace();
    PumpMessages();
    EndGesture(Gesture::RAlt); // pending lock: release posts no Stop

    SetConfig(TestConfig(false, true));
    const int stop_before = CaptureStopCount();
    VoiceInput::RefreshKeyboardHook();
    AllowCaptureStart();
    REQUIRE(WaitFor([stop_before] { return CaptureStopCount() > stop_before; }));
    REQUIRE(!VoiceInput::IsRecording());

    SetConfig(TestConfig(true, true));
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());
    const int next_stop = CaptureStopCount();
    const int next_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(next_stop, next_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

// A lock from the previous hold must be gone before Ctrl+F9 restarts within
// the same physical hold, so releasing the key stops the new recording.
TEST_CASE(ctrl_f9_restart_with_held_shortcut_stops_on_release)
{
    ServiceFixture fixture;
    StartGesture(Gesture::RAlt);
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    PressSpace();
    PumpMessages();
    REQUIRE(WaitFor([] { return g_actions_true_count.load() >= 2; }));

    const int first_stop = CaptureStopCount();
    const int first_hides = g_hide_count.load();
    PressCtrlF9();
    REQUIRE(WaitNormalStop(first_stop, first_hides));
    REQUIRE(!VoiceInput::IsRecording());

    ResetCapture();
    PressCtrlF9();
    REQUIRE(WaitCaptureEntered());
    AllowCaptureStart();
    REQUIRE(WaitRecording());

    const int second_stop = CaptureStopCount();
    const int second_hides = g_hide_count.load();
    EndGesture(Gesture::RAlt);
    REQUIRE(WaitNormalStop(second_stop, second_hides));
    REQUIRE(!VoiceInput::IsRecording());
}

namespace test
{
std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char *name, std::function<void()> fn)
{
    registry().push_back({name, std::move(fn)});
}
} // namespace test

int main()
{
    int failures = 0;
    for (const auto &test_case : test::registry())
    {
        try
        {
            test_case.fn();
            std::cout << "[PASS] " << test_case.name << '\n';
        }
        catch (const std::exception &error)
        {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": " << error.what() << '\n';
        }
        catch (...)
        {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name << ": unknown exception\n";
        }
    }
    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << test::registry().size() << " test(s) passed\n";
    return 0;
}

// ---------------------------------------------------------------------------
// External-effect substitutes. The service translation unit is the real one.
// ---------------------------------------------------------------------------

VoiceInputConfig GetConfiguredVoiceInput()
{
    std::unique_lock<std::mutex> lock(g_config_mutex);
    const VoiceInputConfig snapshot = g_config;
    // Count only after the snapshot is fixed: a waiter may then change the
    // config without changing what this read returns.
    ++g_config_read_count;
    NotifyEffects();
    if (g_block_next_config_read)
    {
        // Only the control thread's one read is gated; test-thread hook calls
        // must keep working while it is parked.
        g_block_next_config_read = false;
        g_config_read_entered = true;
        g_config_cv.notify_all();
        g_config_cv.wait(lock, [] { return g_config_read_release; });
    }
    return snapshot;
}

namespace CommonUtils
{
std::wstring get_ime_data_path_w()
{
    return {};
}
} // namespace CommonUtils

std::wstring string_to_wstring(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

namespace NetworkProxy
{
void ApplyToCurl(CURL *)
{
}
} // namespace NetworkProxy

namespace DiagnosticLog
{
bool IsEnabled()
{
    return false;
}

void Write(const std::wstring &)
{
}
} // namespace DiagnosticLog

namespace VoiceInput
{
void MuteOtherSystemAudio()
{
}

void RestoreOtherSystemAudio()
{
}
} // namespace VoiceInput

PipeClientActivation GetActivePipeClient()
{
    return {};
}

bool SendToTsfWorkerThreadClientViaNamedpipe(uint64_t, uint64_t, UINT, const std::wstring &)
{
    return false;
}

bool SendVoiceCompositionToTsfWorker(uint64_t, uint64_t, UINT, const std::wstring &)
{
    return false;
}

namespace metasequoia::voice
{
struct AudioCapture::Impl
{
};

AudioCapture::AudioCapture() = default;
AudioCapture::~AudioCapture() = default;

bool AudioCapture::start(AudioCallback)
{
    std::unique_lock<std::mutex> lock(g_capture_mutex);
    g_capture_entered = true;
    g_capture_cv.notify_all();
    NotifyEffects();
    g_capture_cv.wait(lock, [] { return g_capture_release; });
    return true;
}

void AudioCapture::stop()
{
    {
        std::lock_guard<std::mutex> lock(g_capture_mutex);
        ++g_capture_stop_count;
    }
    NotifyEffects();
}

bool AudioCapture::callback_failed() const
{
    return false;
}
} // namespace metasequoia::voice

WaveOverlay::WaveOverlay() = default;
WaveOverlay::~WaveOverlay() = default;

bool WaveOverlay::init(HINSTANCE, std::function<void(Action)>)
{
    return true;
}

void WaveOverlay::shutdown()
{
}

void WaveOverlay::show()
{
    NotifyEffects();
}

void WaveOverlay::hide()
{
    ++g_hide_count;
    NotifyEffects();
}

void WaveOverlay::set_listening(bool)
{
}

void WaveOverlay::set_input_level(float)
{
}

void WaveOverlay::set_transcript(const std::wstring &)
{
}

void WaveOverlay::set_show_transcript(bool)
{
}

void WaveOverlay::set_compact_status(CompactStatus)
{
}

void WaveOverlay::set_actions_visible(bool visible)
{
    g_actions_visible = visible;
    if (visible)
        ++g_actions_true_count;
    NotifyEffects();
}

CuePlayer::CuePlayer() = default;
CuePlayer::~CuePlayer() = default;

bool CuePlayer::init(const std::wstring &, const std::wstring &)
{
    return true;
}

void CuePlayer::shutdown()
{
}

void CuePlayer::play_start()
{
}

void CuePlayer::play_end()
{
}

DoubaoAsrClient::DoubaoAsrClient(std::string, bool, std::string, std::string, std::string, bool, bool, bool,
                                 std::string, TranscriptCallback)
{
}

DoubaoAsrClient::~DoubaoAsrClient() = default;

bool DoubaoAsrClient::Start()
{
    return false;
}

void DoubaoAsrClient::PushFloatSamples(const float *, std::size_t)
{
}

std::string DoubaoAsrClient::Finish()
{
    return {};
}

std::string DoubaoAsrClient::LastError() const
{
    return {};
}

void DoubaoAsrClient::Cancel()
{
}

std::string DoubaoAsrClient::TestCredentials(const std::string &, bool, const std::string &, const std::string &,
                                             const std::string &)
{
    return {};
}
