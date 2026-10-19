#include "wave_overlay.h"
#include "mvi_utils.h"
#include "voice_clipboard.h"
#ifndef MSIME_VOICE_OVERLAY_TEST_HOOKS
#include "config/ime_config.h"
#endif

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <windowsx.h>

namespace
{
constexpr wchar_t kClassName[] = L"MviWaveOverlayWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 16;
constexpr UINT kTranscriptChangedMessage = WM_APP + 186;
constexpr UINT kRecoveryChangedMessage = WM_APP + 187;
constexpr UINT kRecoveryClearMessage = WM_APP + 188;
constexpr UINT kFinishRecognitionMessage = WM_APP + 189;
constexpr UINT kAnchorChangedMessage = WM_APP + 192;
constexpr UINT kRecoveryDismissMessage = WM_APP + 193;
constexpr UINT kShowMessage = WM_APP + 194;
constexpr UINT kHideMessage = WM_APP + 195;
constexpr UINT kRecoveryAuthoritativeDismissMessage = WM_APP + 196;
constexpr UINT_PTR kRecoveryCloseTimerIdBase = 2;
constexpr UINT kRecoveryCloseDelayMs = 1000;
constexpr int kCompactWidth = 78;
constexpr int kCompactHeight = 32;
constexpr int kProcessingWidth = 112;
constexpr int kProcessingHeight = 40;
constexpr int kActionWidth = 142;
constexpr int kActionHeight = 40;
constexpr int kRecoveryWidth = 264;
constexpr int kRecoveryHeight = 86;
constexpr float kRecoveryCloseCenterX = 28.0f;
constexpr float kRecoveryCloseCenterY = 58.0f;
constexpr float kRecoveryCloseRadius = 16.0f;
constexpr float kRecoveryCopyLeft = 58.0f;
constexpr float kRecoveryCopyTop = 42.0f;
constexpr float kRecoveryCopyRight = 252.0f;
constexpr float kRecoveryCopyBottom = 74.0f;
constexpr float kActionCenterInset = 18.0f;
constexpr float kActionRadius = 12.0f;
constexpr int kTranscriptWidth = kVoiceOverlayPreferredWidthDip;
constexpr int kTranscriptHeight = 112;
constexpr float kTranscriptHorizontalPadding = 18.0f;
constexpr float kTranscriptTextTop = 37.0f;
constexpr float kTranscriptTextBottom = 6.0f;
constexpr UINT32 kMaxTranscriptLines = 3;
constexpr int kBarCount = 12;
constexpr float kDotRadius = 1.15f;
constexpr float kMaxHalfHeight = 14.0f;

template <typename T> void safe_release(T **obj)
{
    if (*obj)
    {
        (*obj)->Release();
        *obj = nullptr;
    }
}

#ifdef MSIME_VOICE_OVERLAY_TEST_HOOKS
std::function<void()> g_finish_recognition_ui_test_hook;
#endif
} // namespace

WaveOverlay::WaveOverlay()
{
    // Deterministic-but-irregular profile per bar:
    // each bar has different amplitude/frequency/phase so movement looks natural.
    for (int i = 0; i < kBarCount; ++i)
    {
        const float fi = static_cast<float>(i);
        amplitudes_[i] = 0.55f + 0.45f * std::fabs(std::sin(0.73f * fi + 0.19f));
        phases_[i] = 0.41f * fi + 0.37f * std::sin(0.29f * fi + 1.11f);
        freqs_[i] = 4.2f + std::fmod(1.7f * fi, 3.6f);
    }
}

WaveOverlay::~WaveOverlay()
{
    shutdown();
}

bool WaveOverlay::init(HINSTANCE instance, std::function<void(Action)> action_handler, ClipboardWriter clipboard_writer)
{
    if (hwnd_)
    {
        return true;
    }

    instance_ = instance;
    action_handler_ = std::move(action_handler);
    clipboard_writer_ = std::move(clipboard_writer);

    const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    if (FAILED(hr))
    {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&write_factory_))) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-cn",
                                                &text_format_)) ||
        FAILED(write_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn",
                                                &processing_text_format_)))
    {
        shutdown();
        return false;
    }
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    text_format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 23.0f, 18.0f);
    processing_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    processing_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WaveOverlay::wnd_proc;
    wc.hInstance = instance_;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;

    const ATOM atom = RegisterClassW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        shutdown();
        return false;
    }

    hwnd_ = CreateWindowExW(                                                 //
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, //
        kClassName,                                                          //
        L"",                                                                 //
        WS_POPUP,                                                            //
        120,                                                                 //
        120,                                                                 //
        kCompactWidth,                                                       //
        kCompactHeight,                                                      //
        nullptr,                                                             //
        nullptr,                                                             //
        instance_,                                                           //
        this);

    if (!hwnd_)
    {
        shutdown();
        return false;
    }

    post_hwnd_.store(hwnd_, std::memory_order_release);

    // 设置窗口透明
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    MARGINS mar = {-1};
    DwmExtendFrameIntoClientArea(hwnd_, &mar);

    return true;
}

void WaveOverlay::shutdown()
{
    post_hwnd_.store(nullptr, std::memory_order_release);
    session_fence_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        recovery_active_ = false;
        recovery_session_ = 0;
        ++recovery_generation_;
        recovery_text_.clear();
    }
    recovery_mode_ = false;
    recovery_feedback_ = RecoveryFeedback::None;
    recovery_timer_id_ = 0;
    recovery_timer_session_ = 0;
    recovery_timer_generation_ = 0;
    recovery_shown_session_ = 0;
    recovery_shown_generation_ = 0;
    pressed_recovery_session_ = 0;
    pressed_recovery_generation_ = 0;
    if (hwnd_)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    release_render_target();
    release_text_layout();
    safe_release(&processing_text_format_);
    safe_release(&text_format_);
    safe_release(&write_factory_);
    safe_release(&factory_);
}

void WaveOverlay::show_window()
{
    // UI thread only. Background callers post kShowMessage/kRecoveryChangedMessage instead of
    // entering this path, so SetWindowPos/ShowWindow never synchronously waits on the UI thread.
    if (!hwnd_)
        return;

#ifndef MSIME_VOICE_OVERLAY_TEST_HOOKS
    ReloadImeConfigIfChanged();
    const bool light_theme = ResolveConfiguredTheme(GetConfiguredThemeVoice()) == "light";
#else
    const bool light_theme = false;
#endif
    if (light_theme != light_theme_)
    {
        light_theme_ = light_theme;
        release_render_target();
    }
    // 每次显示时根据当前显示器重新定位
    update_window_bounds();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

void WaveOverlay::show()
{
    const HWND hwnd = post_hwnd_.load(std::memory_order_acquire);
    if (!hwnd)
        return;
    const std::uint64_t session = session_fence_.load(std::memory_order_acquire);
    PostMessageW(hwnd, kShowMessage, 0, static_cast<LPARAM>(session));
}

void WaveOverlay::hide()
{
    const HWND hwnd = post_hwnd_.load(std::memory_order_acquire);
    if (!hwnd)
        return;
    const std::uint64_t session = session_fence_.load(std::memory_order_acquire);
    PostMessageW(hwnd, kHideMessage, 0, static_cast<LPARAM>(session));
}

bool WaveOverlay::show_recovery(std::uint64_t session, const std::wstring &text)
{
    const HWND hwnd = post_hwnd_.load(std::memory_order_acquire);
    if (!hwnd || session == 0 || text.empty())
        return false;

    // Unified generation gate: a delayed result for an older recording cannot revive a retained
    // surface after a newer session has become current. This is a monotonic atomic update, not a
    // wait on the UI thread; window application itself remains a posted UI-thread message.
    std::uint64_t fence = session_fence_.load(std::memory_order_acquire);
    while (session > fence &&
           !session_fence_.compare_exchange_weak(fence, session, std::memory_order_acq_rel, std::memory_order_acquire))
    {
    }
    if (session < fence || !post_hwnd_.load(std::memory_order_acquire))
        return false;

    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> recovery_lock(recovery_mutex_);
        recovery_active_ = true;
        recovery_session_ = session;
        recovery_text_ = text;
        generation = ++recovery_generation_;
    }
    if (PostMessageW(hwnd, kRecoveryChangedMessage, 0, 0))
        return true;

    std::lock_guard<std::mutex> lock(recovery_mutex_);
    if (recovery_generation_ == generation)
    {
        recovery_active_ = false;
        recovery_session_ = 0;
        recovery_text_.clear();
    }
    return false;
}

bool WaveOverlay::dismiss_recovery(std::uint64_t session)
{
    const HWND hwnd = post_hwnd_.load(std::memory_order_acquire);
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (!recovery_active_ || recovery_session_ != session)
            return false;
        generation = recovery_generation_;
        recovery_active_ = false;
        recovery_session_ = 0;
        ++recovery_generation_;
        recovery_text_.clear();
    }
    // The model is already gone. This authoritative close must also hide an older generation
    // that is still on screen while a newer generation was published but not applied yet.
    if (hwnd)
        PostMessageW(hwnd, kRecoveryAuthoritativeDismissMessage, static_cast<WPARAM>(session),
                     static_cast<LPARAM>(generation));
    return true;
}

void WaveOverlay::set_session(std::uint64_t session)
{
    // Nonblocking monotonic fence: this may be called while service state locks are held. UI
    // messages reconcile against it, so no lock is ever held across a Win32 call.
    std::uint64_t fence = session_fence_.load(std::memory_order_acquire);
    while (session > fence &&
           !session_fence_.compare_exchange_weak(fence, session, std::memory_order_acq_rel, std::memory_order_acquire))
    {
    }
}

void WaveOverlay::clear_recovery()
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        recovery_active_ = false;
        recovery_session_ = 0;
        generation = ++recovery_generation_;
        recovery_text_.clear();
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kRecoveryClearMessage, 0, static_cast<LPARAM>(generation));
}

bool WaveOverlay::recovery_active_for(std::uint64_t session) const
{
    std::lock_guard<std::mutex> lock(recovery_mutex_);
    return recovery_active_ && recovery_session_ == session;
}

void WaveOverlay::show_ordinary_ui(std::uint64_t session)
{
    if (session == 0 || session_fence_.load(std::memory_order_acquire) != session)
        return;

    bool keep_recovery = false;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (recovery_active_)
        {
            if (recovery_session_ >= session)
            {
                keep_recovery = true;
            }
            else
            {
                recovery_active_ = false;
                recovery_session_ = 0;
                ++recovery_generation_;
                recovery_text_.clear();
            }
        }
    }
    if (keep_recovery)
        return;
    if (recovery_mode_)
    {
        if (recovery_shown_session_ >= session)
            return;
        clear_recovery_ui(0);
    }
    show_window();
}

void WaveOverlay::hide_ordinary_ui(std::uint64_t session)
{
    if (session == 0 || session_fence_.load(std::memory_order_acquire) != session)
        return;

    bool keep_recovery = false;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (recovery_active_)
        {
            if (recovery_session_ >= session)
            {
                keep_recovery = true;
            }
            else
            {
                recovery_active_ = false;
                recovery_session_ = 0;
                ++recovery_generation_;
                recovery_text_.clear();
            }
        }
    }
    if (keep_recovery)
        return;
    if (recovery_mode_ && recovery_shown_session_ >= session)
        return;
    if (recovery_mode_)
        clear_recovery_ui(0);

    // Ordinary hide is visual only. StopRecording posts the next Recognizing/Cancel/text state
    // immediately afterwards, and this message may still be queued behind it; clearing those
    // fields here would erase the state that belongs to the same session.
    if (hwnd_)
        ShowWindow(hwnd_, SW_HIDE);
}

bool WaveOverlay::clear_recovery_model(std::uint64_t session, std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(recovery_mutex_);
    if (!recovery_active_ || recovery_session_ != session || (generation != 0 && recovery_generation_ != generation))
        return false;
    recovery_active_ = false;
    recovery_session_ = 0;
    ++recovery_generation_;
    recovery_text_.clear();
    return true;
}

void WaveOverlay::finish_recognition(std::uint64_t session)
{
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kFinishRecognitionMessage, 0, static_cast<LPARAM>(session));
}

#ifdef MSIME_VOICE_OVERLAY_TEST_HOOKS
std::wstring WaveOverlay::recovery_text_for_test() const
{
    std::lock_guard<std::mutex> lock(recovery_mutex_);
    return recovery_text_;
}

std::uint64_t WaveOverlay::recovery_generation_for_test() const
{
    std::lock_guard<std::mutex> lock(recovery_mutex_);
    return recovery_generation_;
}

void WaveOverlay::post_recovery_dismiss_for_test(std::uint64_t session, std::uint64_t generation)
{
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kRecoveryDismissMessage, static_cast<WPARAM>(session), static_cast<LPARAM>(generation));
}

void WaveOverlay::set_finish_recognition_ui_test_hook(std::function<void()> hook)
{
    g_finish_recognition_ui_test_hook = std::move(hook);
}
#endif

void WaveOverlay::set_anchor(const VoiceOverlayAnchor &anchor)
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        anchor_ = anchor;
    }
    if (HWND hwnd = post_hwnd_.load(std::memory_order_acquire))
        PostMessageW(hwnd, kAnchorChangedMessage, 0, 0);
}

void WaveOverlay::set_listening(bool listening)
{
    listening_.store(listening);
}

void WaveOverlay::set_input_level(float level)
{
    const float clamped = std::max(0.0f, std::min(1.0f, level));
    input_level_.store(clamped);
}

void WaveOverlay::set_transcript(const std::wstring &text)
{
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        if (transcript_ == text)
            return;
        transcript_ = text;
    }
    if (hwnd_ && show_transcript_.load())
        PostMessageW(hwnd_, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_show_transcript(bool show)
{
    const bool previous = show_transcript_.exchange(show);
    if (previous == show)
        return;
    if (!show)
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        transcript_.clear();
    }
    if (hwnd_)
        PostMessageW(hwnd_, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_compact_status(CompactStatus status)
{
    const CompactStatus previous = compact_status_.exchange(status);
    if (previous == status)
        return;
    if (hwnd_)
        PostMessageW(hwnd_, kTranscriptChangedMessage, 0, 0);
}

void WaveOverlay::set_actions_visible(bool visible)
{
    const bool previous = actions_visible_.exchange(visible);
    if (previous == visible)
        return;
    if (hwnd_)
        PostMessageW(hwnd_, kTranscriptChangedMessage, 0, 0);
}

LRESULT CALLBACK WaveOverlay::wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WaveOverlay *self = reinterpret_cast<WaveOverlay *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
        self = reinterpret_cast<WaveOverlay *>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (!self)
    {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return self->handle_message(hwnd, message, wParam, lParam);
}

LRESULT WaveOverlay::handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kTimerId)
        {
            update_wave_levels();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (recovery_timer_id_ != 0 && wParam == recovery_timer_id_)
        {
            on_recovery_timer();
        }
        return 0;
    case WM_SIZE:
        if (render_target_)
        {
            render_target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        update_dpi_scale(resolve_anchor());
        return 0;
    case WM_DPICHANGED:
        // A saved anchor is refreshed only for system layout changes. Ordinary foreground changes
        // during recognition never reach this path.
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
        // Re-resolve the saved monitor after a display/DPI layout change, then keep growing from
        // the same work-area center/bottom anchor. Foreground focus is deliberately not consulted.
        refresh_anchor_for_display_change();
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        // Taskbar position/autohide changes resize rcWork without necessarily changing the monitor
        // or DPI. Refresh the saved anchor for that layout change only.
        if (wParam == SPI_SETWORKAREA)
        {
            refresh_anchor_for_display_change();
            update_window_bounds();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        Action action{};
        if (hit_test_action(static_cast<float>(point.x) / scale_x_, static_cast<float>(point.y) / scale_y_, action))
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        Action action{};
        if (hit_test_action(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                            static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, action))
        {
            pressed_action_ = action;
            action_pressed_ = true;
            pressed_recovery_session_ = recovery_mode_ ? recovery_shown_session_ : 0;
            pressed_recovery_generation_ = recovery_mode_ ? recovery_shown_generation_ : 0;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!action_pressed_)
            break;
        ReleaseCapture();
        action_pressed_ = false;
        Action action{};
        if (hit_test_action(static_cast<float>(GET_X_LPARAM(lParam)) / scale_x_,
                            static_cast<float>(GET_Y_LPARAM(lParam)) / scale_y_, action) &&
            action == pressed_action_)
        {
            if (recovery_mode_)
            {
                // Bind the click to the result generation that was on screen when the button
                // went down. A newer recovery result must not be copied or closed by this click.
                if (pressed_recovery_session_ == 0 || pressed_recovery_session_ != recovery_shown_session_ ||
                    pressed_recovery_generation_ != recovery_shown_generation_)
                    return 0;
                if (action == Action::Copy)
                {
                    if (action_handler_)
                        action_handler_(Action::Copy);
                    on_recovery_copy(pressed_recovery_session_, pressed_recovery_generation_);
                }
                else if (action == Action::Close)
                {
                    if (action_handler_)
                        action_handler_(Action::Close);
                    clear_recovery_model(pressed_recovery_session_, pressed_recovery_generation_);
                    dismiss_recovery_ui(pressed_recovery_session_, pressed_recovery_generation_);
                }
            }
            else if (action_handler_)
            {
                action_handler_(action);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        action_pressed_ = false;
        return 0;
    case WM_RBUTTONUP:
        // 右键一律当作取消。这个窗口是 WS_POPUP + WS_EX_NOACTIVATE，右键落在它
        // 上面本来就会被吃掉，前台应用的右键菜单也弹不出来；把它定义成取消，
        // 可以保证在任何状态下（按钮被隐藏时同样有效）都有一个鼠标退出通道。
        if (recovery_mode_)
        {
            if (action_handler_)
                action_handler_(Action::Close);
            if (recovery_shown_session_ != 0 && recovery_shown_generation_ != 0)
            {
                clear_recovery_model(recovery_shown_session_, recovery_shown_generation_);
                dismiss_recovery_ui(recovery_shown_session_, recovery_shown_generation_);
            }
        }
        else if (action_handler_)
        {
            action_handler_(Action::Cancel);
        }
        return 0;
    case kAnchorChangedMessage:
        update_window_bounds();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kTranscriptChangedMessage:
        if (!actions_visible_.load())
            action_pressed_ = false;
        if (!recovery_mode_)
        {
            release_text_layout();
            update_window_bounds();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case kShowMessage:
        show_ordinary_ui(static_cast<std::uint64_t>(lParam));
        return 0;
    case kHideMessage:
        hide_ordinary_ui(static_cast<std::uint64_t>(lParam));
        return 0;
    case kRecoveryChangedMessage:
        apply_recovery_to_window();
        return 0;
    case kRecoveryClearMessage:
        clear_recovery_ui(static_cast<std::uint64_t>(lParam));
        return 0;
    case kRecoveryDismissMessage:
        dismiss_recovery_ui(static_cast<std::uint64_t>(wParam), static_cast<std::uint64_t>(lParam));
        return 0;
    case kRecoveryAuthoritativeDismissMessage:
        dismiss_recovery_ui_authoritative(static_cast<std::uint64_t>(wParam), static_cast<std::uint64_t>(lParam));
        return 0;
    case kFinishRecognitionMessage:
        finish_recognition_ui(static_cast<std::uint64_t>(lParam));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        draw();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        cancel_recovery_close();
        post_hwnd_.store(nullptr, std::memory_order_release);
        release_render_target();
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void WaveOverlay::apply_recovery_to_window()
{
    if (!hwnd_)
        return;

    bool active = false;
    std::uint64_t session = 0;
    std::uint64_t generation = 0;
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        active = recovery_active_ && !recovery_text_.empty();
        session = recovery_session_;
        generation = recovery_generation_;
        text = recovery_text_;
    }
    // A queued notification is harmless after a newer session or generation has taken over. Do not
    // clear whatever is already visible here: an authoritative dismiss owns that transition.
    if (!active || session == 0 || text.empty() || session != session_fence_.load(std::memory_order_acquire))
        return;

    cancel_recovery_close();
    if (action_pressed_)
    {
        // A click that began on the previous result cannot finish on this one.
        ReleaseCapture();
    }
    recovery_mode_ = true;
    recovery_feedback_ = RecoveryFeedback::None;
    action_pressed_ = false;
    pressed_recovery_session_ = 0;
    pressed_recovery_generation_ = 0;
    recovery_shown_session_ = session;
    recovery_shown_generation_ = generation;
    listening_.store(false);
    input_level_.store(0.0f);
    compact_status_.store(CompactStatus::None);
    actions_visible_.store(false);
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        transcript_.clear();
    }
    release_text_layout();
    update_window_bounds();
    show_window();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WaveOverlay::clear_recovery_ui(std::uint64_t generation)
{
    if (generation != 0)
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (recovery_generation_ != generation)
            return;
    }
    cancel_recovery_close();
    if (!recovery_mode_)
        return;

    recovery_mode_ = false;
    recovery_feedback_ = RecoveryFeedback::None;
    action_pressed_ = false;
    pressed_recovery_session_ = 0;
    pressed_recovery_generation_ = 0;
    recovery_shown_session_ = 0;
    recovery_shown_generation_ = 0;
    release_text_layout();
    update_window_bounds();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (hwnd_)
        ShowWindow(hwnd_, SW_HIDE);
}

bool WaveOverlay::dismiss_recovery_ui(std::uint64_t session, std::uint64_t generation)
{
    // Visual-only dismissal, bound to the exact value that was visible. Any newer result that has
    // already been published but not applied is left untouched and will be applied by its own message.
    if (session == 0 || generation == 0 || !recovery_mode_ || recovery_shown_session_ != session ||
        recovery_shown_generation_ != generation)
        return false;
    clear_recovery_ui(0);
    return true;
}

bool WaveOverlay::dismiss_recovery_ui_authoritative(std::uint64_t session, std::uint64_t cleared_generation)
{
    // Public dismiss clears the model for the current session, while the message can arrive after
    // a newer generation was published without being pumped. Hide every older screen generation of
    // the same session, but never a newer result that appeared after the model was cleared.
    if (session == 0 || cleared_generation == 0 || !recovery_mode_ || recovery_shown_session_ != session ||
        recovery_shown_generation_ > cleared_generation)
        return false;
    clear_recovery_ui(0);
    return true;
}

void WaveOverlay::finish_recognition_ui(std::uint64_t session)
{
#ifdef MSIME_VOICE_OVERLAY_TEST_HOOKS
    if (g_finish_recognition_ui_test_hook)
    {
        auto hook = std::move(g_finish_recognition_ui_test_hook);
        g_finish_recognition_ui_test_hook = {};
        hook();
    }
#endif
    // A delayed completion for session A must not hide session B or a retained result for A/B. The
    // atomic fence is read on the UI thread; no service lock is involved and no Win32 call is made
    // from a background thread.
    hide_ordinary_ui(session);
}

void WaveOverlay::on_recovery_copy(std::uint64_t session, std::uint64_t generation)
{
    if (!recovery_mode_ || session == 0 || session != recovery_shown_session_ ||
        generation != recovery_shown_generation_)
        return;

    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (!recovery_active_ || recovery_session_ != session || recovery_generation_ != generation)
            return;
        text = recovery_text_;
    }
    if (text.empty())
        return;

    const bool copied = clipboard_writer_ ? clipboard_writer_(hwnd_, session, text)
                                          : VoiceInput::CopyUnicodeTextToClipboard(hwnd_, text);
    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (!recovery_active_ || recovery_session_ != session || recovery_generation_ != generation)
            return;
    }
    if (copied)
    {
        recovery_feedback_ = RecoveryFeedback::Copied;
        schedule_recovery_close(session, generation);
    }
    else
    {
        // Keep the complete result and make the same action retryable. A failed attempt must not
        // leave an older successful auto-close pending.
        cancel_recovery_close();
        recovery_feedback_ = RecoveryFeedback::Failed;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WaveOverlay::schedule_recovery_close(std::uint64_t session, std::uint64_t generation)
{
    cancel_recovery_close();
    if (!hwnd_ || !recovery_mode_ || session != recovery_shown_session_ || generation != recovery_shown_generation_)
        return;

    {
        std::lock_guard<std::mutex> lock(recovery_mutex_);
        if (!recovery_active_ || recovery_session_ != session || recovery_generation_ != generation)
            return;
    }

    if (next_recovery_timer_id_ == 0 || next_recovery_timer_id_ == kTimerId)
        next_recovery_timer_id_ = kRecoveryCloseTimerIdBase;
    const UINT_PTR timer_id = next_recovery_timer_id_++;
    if (next_recovery_timer_id_ == 0 || next_recovery_timer_id_ == kTimerId)
        next_recovery_timer_id_ = kRecoveryCloseTimerIdBase;

    if (SetTimer(hwnd_, timer_id, kRecoveryCloseDelayMs, nullptr) == 0)
        return; // Keep the success feedback visible; the close action remains available.

    recovery_timer_id_ = timer_id;
    recovery_timer_session_ = session;
    recovery_timer_generation_ = generation;
}

void WaveOverlay::cancel_recovery_close()
{
    if (hwnd_ && recovery_timer_id_ != 0)
        KillTimer(hwnd_, recovery_timer_id_);
    recovery_timer_id_ = 0;
    recovery_timer_session_ = 0;
    recovery_timer_generation_ = 0;
}

void WaveOverlay::on_recovery_timer()
{
    if (recovery_timer_id_ == 0)
        return;

    const UINT_PTR timer_id = recovery_timer_id_;
    const std::uint64_t session = recovery_timer_session_;
    const std::uint64_t generation = recovery_timer_generation_;
    recovery_timer_id_ = 0;
    recovery_timer_session_ = 0;
    recovery_timer_generation_ = 0;
    if (hwnd_)
        KillTimer(hwnd_, timer_id);

    if (!recovery_mode_ || session != recovery_shown_session_ || generation != recovery_shown_generation_)
        return;

    const bool cleared = clear_recovery_model(session, generation);
    const bool dismissed = dismiss_recovery_ui(session, generation);
    if (cleared && dismissed && action_handler_)
        action_handler_(Action::Close);
}

const wchar_t *WaveOverlay::recovery_copy_label() const
{
    switch (recovery_feedback_)
    {
    case RecoveryFeedback::Copied:
        return L"已复制";
    case RecoveryFeedback::Failed:
        return L"重试复制";
    case RecoveryFeedback::None:
    default:
        return L"复制全文";
    }
}

bool WaveOverlay::ensure_render_target()
{
    if (render_target_)
    {
        return true;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
    const HRESULT hr = factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(hwnd_, size), &render_target_);

    if (FAILED(hr))
    {
        return false;
    }

    const D2D1_COLOR_F background =
        light_theme_ ? D2D1::ColorF(0.985f, 0.986f, 0.988f, 0.97f) : D2D1::ColorF(0.086f, 0.090f, 0.102f, 0.96f);
    const D2D1_COLOR_F bars = light_theme_ ? D2D1::ColorF(0.16f, 0.45f, 0.38f) : D2D1::ColorF(0.49f, 0.75f, 0.66f);
    const D2D1_COLOR_F text_color =
        light_theme_ ? D2D1::ColorF(0.11f, 0.12f, 0.14f) : D2D1::ColorF(0.93f, 0.94f, 0.95f);
    const D2D1_COLOR_F border =
        light_theme_ ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f);
    const D2D1_COLOR_F action_background =
        light_theme_ ? D2D1::ColorF(0.945f, 0.950f, 0.958f) : D2D1::ColorF(0.140f, 0.145f, 0.160f);

    if (FAILED(render_target_->CreateSolidColorBrush(background, &bg_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(bars, &bar_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(border, &border_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(action_background, &action_bg_brush_)) ||
        FAILED(render_target_->CreateSolidColorBrush(text_color, &text_brush_)))
    {
        release_render_target();
        return false;
    }

    update_dpi_scale(resolve_anchor());
    return true;
}

void WaveOverlay::release_render_target()
{
    safe_release(&text_brush_);
    safe_release(&bar_brush_);
    safe_release(&bg_brush_);
    safe_release(&border_brush_);
    safe_release(&action_bg_brush_);
    safe_release(&render_target_);
}

void WaveOverlay::release_text_layout()
{
    safe_release(&text_layout_);
    layout_source_transcript_.clear();
}

bool WaveOverlay::ensure_text_layout(const std::wstring &transcript, float width, float height)
{
    if (text_layout_ && layout_source_transcript_ == transcript)
        return true;
    release_text_layout();
    if (!write_factory_ || !text_format_ || transcript.empty())
        return false;

    const auto line_count_for = [&](const std::wstring &candidate) {
        IDWriteTextLayout *layout = nullptr;
        UINT32 line_count = 0;
        if (SUCCEEDED(write_factory_->CreateTextLayout(candidate.c_str(), static_cast<UINT32>(candidate.size()),
                                                       text_format_, width, 4096.0f, &layout)))
        {
            layout->GetLineMetrics(nullptr, 0, &line_count);
            layout->Release();
        }
        return line_count;
    };

    std::wstring visible = transcript;
    if (line_count_for(visible) > kMaxTranscriptLines)
    {
        size_t low = 0;
        size_t high = transcript.size();
        while (low < high)
        {
            const size_t middle = low + (high - low) / 2;
            const std::wstring candidate = L"…" + transcript.substr(middle);
            if (line_count_for(candidate) <= kMaxTranscriptLines)
                high = middle;
            else
                low = middle + 1;
        }

        // Do not split a UTF-16 surrogate pair when keeping the newest text.
        if (low < transcript.size() && LOW_SURROGATE_START <= transcript[low] && transcript[low] <= LOW_SURROGATE_END)
            ++low;
        visible = L"…" + transcript.substr(low);
    }

    if (FAILED(write_factory_->CreateTextLayout(visible.c_str(), static_cast<UINT32>(visible.size()), text_format_,
                                                width, height, &text_layout_)))
        return false;
    layout_source_transcript_ = transcript;
    return true;
}

VoiceOverlayAnchor WaveOverlay::resolve_anchor() const
{
    {
        std::lock_guard<std::mutex> lock(anchor_mutex_);
        if (anchor_.valid)
            return anchor_;
    }
    return CaptureVoiceOverlayAnchor();
}

void WaveOverlay::refresh_anchor_for_display_change()
{
    std::lock_guard<std::mutex> lock(anchor_mutex_);
    if (anchor_.valid)
        anchor_ = RefreshVoiceOverlayAnchorForDisplayChange(anchor_);
}

void WaveOverlay::update_dpi_scale(const VoiceOverlayAnchor &anchor)
{
    if (!hwnd_)
        return;

    // The anchor captured at recording start owns the scale for this session. Only a fixture that
    // never called set_anchor falls back to the foreground monitor.
    const float scale = anchor.valid ? anchor.scale() : mvi_utils::GetForegroundMonitorScale();
    scale_x_ = scale;
    scale_y_ = scale;
    dpi_ = static_cast<UINT>(std::lround(scale * USER_DEFAULT_SCREEN_DPI));

    if (render_target_)
        render_target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
}

void WaveOverlay::update_window_bounds()
{
    if (!hwnd_)
        return;
    // 每次重新定位都按当前目标显示器刷新缩放，使 show()、状态切换、分辨率/DPI 变化
    // 走到的尺寸计算始终基于最新的显示器 DPI（与候选框每次显示都重算的行为一致）。
    const VoiceOverlayAnchor anchor = resolve_anchor();
    update_dpi_scale(anchor);
    bool has_transcript = false;
    if (show_transcript_.load())
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        has_transcript = !transcript_.empty();
    }
    const bool has_compact_status = compact_status_.load() != CompactStatus::None;
    const bool has_actions = actions_visible_.load();
    const int logical_width =
        recovery_mode_
            ? kRecoveryWidth
            : (has_actions
                   ? kActionWidth
                   : (has_compact_status ? kProcessingWidth : (has_transcript ? kTranscriptWidth : kCompactWidth)));
    const int logical_height =
        recovery_mode_
            ? kRecoveryHeight
            : (has_actions
                   ? kActionHeight
                   : (has_compact_status ? kProcessingHeight : (has_transcript ? kTranscriptHeight : kCompactHeight)));
    const int width = static_cast<int>(std::lround(logical_width * scale_x_));
    const int height = static_cast<int>(std::lround(logical_height * scale_y_));
    RECT target = VoiceOverlayBottomAnchoredRect(anchor, width, height);
    if (target.right <= target.left || target.bottom <= target.top)
    {
        // Standalone fixture fallback only: no explicit anchor and no capturable monitor. Keep the
        // same work-area center / 24 DIP bottom rule using the existing monitor utility.
        const mvi_utils::MonitorMetrics metrics = mvi_utils::GetForegroundMonitorMetrics();
        const int work_width = static_cast<int>(metrics.work.right - metrics.work.left);
        const int work_height = static_cast<int>(metrics.work.bottom - metrics.work.top);
        if (work_width <= 0 || work_height <= 0)
            return;
        const int clamped_width = (std::max)(1, (std::min)(width, work_width));
        const int clamped_height = (std::max)(1, (std::min)(height, work_height));
        const int margin = VoiceOverlayDipToPx(static_cast<float>(kVoiceOverlayBottomMarginDip), dpi_);
        const int x = metrics.work.left + (work_width - clamped_width) / 2;
        int y = metrics.work.bottom - margin - clamped_height;
        y = (std::max)(static_cast<int>(metrics.work.top),
                       (std::min)(y, static_cast<int>(metrics.work.bottom) - clamped_height));
        target = RECT{x, y, x + clamped_width, y + clamped_height};
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOACTIVATE);
}

bool WaveOverlay::hit_test_action(float x, float y, Action &action) const
{
    if (!hwnd_)
        return false;
    if (recovery_mode_)
    {
        const float close_dx = x - kRecoveryCloseCenterX;
        const float close_dy = y - kRecoveryCloseCenterY;
        if (close_dx * close_dx + close_dy * close_dy <= kRecoveryCloseRadius * kRecoveryCloseRadius)
        {
            action = Action::Close;
            return true;
        }
        if (x >= kRecoveryCopyLeft && x <= kRecoveryCopyRight && y >= kRecoveryCopyTop && y <= kRecoveryCopyBottom)
        {
            action = Action::Copy;
            return true;
        }
        return false;
    }
    if (!actions_visible_.load())
        return false;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float width = static_cast<float>(rc.right - rc.left) / scale_x_;
    const float height = static_cast<float>(rc.bottom - rc.top) / scale_y_;
    const float center_y = height * 0.5f;
    const auto inside = [&](float center_x) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        return dx * dx + dy * dy <= kActionRadius * kActionRadius;
    };
    if (inside(kActionCenterInset))
    {
        action = Action::Cancel;
        return true;
    }
    if (inside(width - kActionCenterInset))
    {
        action = Action::Confirm;
        return true;
    }
    return false;
}

void WaveOverlay::update_wave_levels()
{
    const bool listening = listening_.load();
    const float level = listening ? input_level_.load() : 0.0f;
    const float t = static_cast<float>(GetTickCount64() * 0.001);

    for (int i = 0; i < kBarCount; ++i)
    {
        const float p = phases_[i];
        const float f = freqs_[i];
        const float center = 0.5f * static_cast<float>(kBarCount - 1);
        const float dist = std::abs(static_cast<float>(i) - center) / std::max(1.0f, center);
        const float center_boost = 1.0f + 0.75f * (1.0f - dist); // stronger expansion near center when speaking

        // Multi-harmonic motion. Keep irregularity, but avoid excessive high-frequency jitter.
        const float n1 = 0.5f + 0.5f * std::sin(t * f + p);
        const float n2 = 0.5f + 0.5f * std::sin(t * (0.57f * f) + 1.7f * p + 0.9f);
        const float n3 = 0.5f + 0.5f * std::sin(t * (1.23f * f) + 0.6f * p + 2.1f);
        const float irregular = std::min(1.0f, 0.60f * n1 + 0.30f * n2 + 0.10f * n3);

        // When signal is weak, floor also drops so bars settle back to dots smoothly.
        const float floor = level * (0.06f + 0.12f * n3);
        float target = level * amplitudes_[i] * center_boost * std::max(floor, irregular);

        // Add transient punch on rising edge so start feels more energetic.
        if (target > levels_[i])
        {
            const float transient = 0.22f * level * (1.0f - levels_[i]);
            target = std::min(1.0f, target + transient);
        }

        const float attack = 0.52f;
        const float decay = listening ? 0.07f : 0.045f;
        const float smooth = target > levels_[i] ? attack : decay;
        levels_[i] = levels_[i] * (1.0f - smooth) + target * smooth;
    }
}

void WaveOverlay::draw()
{
    if (!ensure_render_target())
    {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    const float w = static_cast<float>(rc.right - rc.left) / scale_x_;
    const float h = static_cast<float>(rc.bottom - rc.top) / scale_y_;
    std::wstring transcript;
    const bool show_transcript = show_transcript_.load();
    if (show_transcript)
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        transcript = transcript_;
    }
    const CompactStatus compact_status = compact_status_.load();
    const bool has_compact_status = compact_status != CompactStatus::None;
    const bool has_actions = actions_visible_.load();
    const bool has_transcript = !has_compact_status && show_transcript && !transcript.empty();
    const float content_left = has_actions ? 35.0f : 0.0f;
    const float content_right = has_actions ? w - 35.0f : w;
    const float content_width = content_right - content_left;
    const float wave_width =
        has_transcript ? (std::min)(78.0f, w) : (has_actions ? (std::min)(78.0f, content_width) : w);
    const float wave_left = has_transcript ? (w - wave_width) * 0.5f : 0.0f;
    const float centered_wave_left = has_actions ? content_left + (content_width - wave_width) * 0.5f : wave_left;
    const float center_y = has_transcript ? 18.0f : h * 0.5f;
    const float side_margin = kDotRadius + 0.25f;
    const float track_width = std::max(1.0f, wave_width - 2.0f * side_margin);
    const float base_step = (kBarCount > 1) ? (track_width / static_cast<float>(kBarCount - 1)) : 0.0f;
    const float step = base_step * 0.75f; // 减小一点间距，让波形更紧凑一些
    const float used_width = step * static_cast<float>(kBarCount - 1);
    const float start_x = centered_wave_left + (wave_width - used_width) * 0.5f;
    const float dot_radius = kDotRadius;
    const float bar_width = std::max(dot_radius * 2.0f, base_step * 0.26f);

    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float corner = (has_compact_status || has_actions) ? h * 0.5f : (has_transcript ? 14.0f : 10.0f);
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), corner, corner);
    render_target_->FillRoundedRectangle(panel, bg_brush_);
    render_target_->DrawRoundedRectangle(panel, border_brush_, 1.0f);

    if (recovery_mode_)
    {
        constexpr wchar_t title[] = L"转录已保留";
        render_target_->DrawTextW(title, static_cast<UINT32>(wcslen(title)), processing_text_format_,
                                  D2D1::RectF(0.0f, 5.0f, w, 37.0f), text_brush_);

        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kRecoveryCloseCenterX, kRecoveryCloseCenterY),
                                                  kRecoveryCloseRadius, kRecoveryCloseRadius),
                                    action_bg_brush_);
        constexpr float close_half = 3.5f;
        render_target_->DrawLine(D2D1::Point2F(kRecoveryCloseCenterX - close_half, kRecoveryCloseCenterY - close_half),
                                 D2D1::Point2F(kRecoveryCloseCenterX + close_half, kRecoveryCloseCenterY + close_half),
                                 text_brush_, 1.6f);
        render_target_->DrawLine(D2D1::Point2F(kRecoveryCloseCenterX + close_half, kRecoveryCloseCenterY - close_half),
                                 D2D1::Point2F(kRecoveryCloseCenterX - close_half, kRecoveryCloseCenterY + close_half),
                                 text_brush_, 1.6f);

        const D2D1_ROUNDED_RECT copy_button = D2D1::RoundedRect(
            D2D1::RectF(kRecoveryCopyLeft, kRecoveryCopyTop, kRecoveryCopyRight, kRecoveryCopyBottom), 10.0f, 10.0f);
        render_target_->FillRoundedRectangle(copy_button, action_bg_brush_);
        const wchar_t *label = recovery_copy_label();
        render_target_->DrawTextW(
            label, static_cast<UINT32>(wcslen(label)), processing_text_format_,
            D2D1::RectF(kRecoveryCopyLeft, kRecoveryCopyTop, kRecoveryCopyRight, kRecoveryCopyBottom), text_brush_);

        const HRESULT recovery_hr = render_target_->EndDraw();
        if (recovery_hr == D2DERR_RECREATE_TARGET)
            release_render_target();
        return;
    }

    if (has_compact_status)
    {
        const wchar_t *label = compact_status == CompactStatus::Recognizing ? L"识别中..." : L"处理中...";
        render_target_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), processing_text_format_,
                                  D2D1::RectF(content_left, 0.0f, content_right, h), text_brush_);
    }
    else
    {
        for (int i = 0; i < kBarCount; ++i)
        {
            const float x = start_x + i * step;
            const float display_level = has_transcript ? (std::min)(1.0f, levels_[i] * 1.35f) : levels_[i];
            const float max_half_height = has_transcript ? 14.0f : kMaxHalfHeight;
            const float half = dot_radius + display_level * (max_half_height - dot_radius);
            if (display_level < 0.06f)
            {
                render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, center_y), dot_radius, dot_radius),
                                            bar_brush_);
            }
            else
            {
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                    D2D1::RectF(x - bar_width * 0.5f, center_y - half, x + bar_width * 0.5f, center_y + half),
                    bar_width * 0.5f, bar_width * 0.5f);
                render_target_->FillRoundedRectangle(rr, bar_brush_);
            }
        }
    }

    if (has_transcript && text_format_)
    {
        const float text_width = w - 2.0f * kTranscriptHorizontalPadding;
        const float text_height = h - kTranscriptTextTop - kTranscriptTextBottom;
        if (ensure_text_layout(transcript, text_width, text_height))
        {
            render_target_->DrawTextLayout(D2D1::Point2F(kTranscriptHorizontalPadding, kTranscriptTextTop),
                                           text_layout_, text_brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    if (has_actions)
    {
        const float button_y = h * 0.5f;
        const float left_x = kActionCenterInset;
        const float right_x = w - kActionCenterInset;
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left_x, button_y), kActionRadius, kActionRadius),
                                    action_bg_brush_);
        render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(right_x, button_y), kActionRadius, kActionRadius),
                                    action_bg_brush_);

        constexpr float x_half = 3.0f;
        render_target_->DrawLine(D2D1::Point2F(left_x - x_half, button_y - x_half),
                                 D2D1::Point2F(left_x + x_half, button_y + x_half), text_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(left_x + x_half, button_y - x_half),
                                 D2D1::Point2F(left_x - x_half, button_y + x_half), text_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(right_x - 3.8f, button_y),
                                 D2D1::Point2F(right_x - 0.8f, button_y + 3.0f), text_brush_, 1.7f);
        render_target_->DrawLine(D2D1::Point2F(right_x - 0.8f, button_y + 3.0f),
                                 D2D1::Point2F(right_x + 4.2f, button_y - 3.5f), text_brush_, 1.7f);
    }

    const HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        release_render_target();
    }
}
