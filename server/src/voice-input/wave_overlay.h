#pragma once

#include "voice_overlay_layout.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class WaveOverlay
{
  public:
    enum class CompactStatus
    {
        None,
        Recognizing,
        Processing,
    };

    enum class Action
    {
        Cancel,
        Confirm,
        // Recovery-mode actions. Copy never means "stop recording" and Confirm keeps its original
        // recording-stop meaning in the normal overlay flow.
        Copy,
        Close,
    };

    using ClipboardWriter = std::function<bool(HWND, std::uint64_t, const std::wstring &)>;

    WaveOverlay();
    ~WaveOverlay();

    WaveOverlay(const WaveOverlay &) = delete;
    WaveOverlay &operator=(const WaveOverlay &) = delete;

    bool init(HINSTANCE instance, std::function<void(Action)> action_handler, ClipboardWriter clipboard_writer = {});
    void shutdown();
    bool is_available() const
    {
        return post_hwnd_.load(std::memory_order_acquire) != nullptr;
    }

    void show();
    void hide();

    // Independent retained-result mode on the already initialized overlay. The full transcript and
    // session live under recovery_mutex_; callers only post a UI-thread notification. All Win32
    // window operations run on the overlay UI thread. Copy never reuses Action::Confirm. The recovery
    // timer is bound to the session and generation created here.
    bool show_recovery(std::uint64_t session, const std::wstring &text);
    bool dismiss_recovery(std::uint64_t session);
    // Fence ordinary recognition-completion messages to the session that currently owns the
    // overlay. A delayed finish from an older session cannot hide a newer recording or result.
    void set_session(std::uint64_t session);
    void clear_recovery();
    bool recovery_active_for(std::uint64_t session) const;
    // Recognition completion may hide ordinary status UI, but must not hide a recovery result
    // that was just published for the same session.
    void finish_recognition(std::uint64_t session);

    // Thread-safe. Pass the anchor captured once at recording start; an invalid anchor restores
    // the foreground-monitor fallback used by standalone fixtures.
    void set_anchor(const VoiceOverlayAnchor &anchor);

    void set_listening(bool listening);
    void set_input_level(float level);
    void set_transcript(const std::wstring &text);
    // When false, the overlay stays compact and never paints live ASR text.
    void set_show_transcript(bool show);
    void set_compact_status(CompactStatus status);
    void set_actions_visible(bool visible);

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT handle_message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ensure_render_target();
    bool ensure_text_layout(const std::wstring &transcript, float width, float height);
    void release_render_target();
    void release_text_layout();
    void update_wave_levels();
    void draw();
    VoiceOverlayAnchor resolve_anchor() const;
    void refresh_anchor_for_display_change();
    void update_dpi_scale(const VoiceOverlayAnchor &anchor);
    void update_window_bounds();
    bool hit_test_action(float x, float y, Action &action) const;
    void show_window();
    void show_ordinary_ui(std::uint64_t session);
    void hide_ordinary_ui(std::uint64_t session);
    bool clear_recovery_model(std::uint64_t session, std::uint64_t generation);
    void apply_recovery_to_window();
    void clear_recovery_ui(std::uint64_t generation);
    bool dismiss_recovery_ui(std::uint64_t session, std::uint64_t generation);
    bool dismiss_recovery_ui_authoritative(std::uint64_t session, std::uint64_t cleared_generation);
    void finish_recognition_ui(std::uint64_t session);
    void on_recovery_copy(std::uint64_t session, std::uint64_t generation);
    void schedule_recovery_close(std::uint64_t session, std::uint64_t generation);
    void cancel_recovery_close();
    void on_recovery_timer();
    const wchar_t *recovery_copy_label() const;

  private:
    static constexpr int kLevelCount = 12;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    // Safe publication target for background callers; cleared before DestroyWindow.
    std::atomic<HWND> post_hwnd_{nullptr};
    UINT dpi_ = 96;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;

    std::atomic<bool> listening_{false};
    std::atomic<float> input_level_{0.0f};
    std::atomic<bool> show_transcript_{true};
    std::atomic<CompactStatus> compact_status_{CompactStatus::None};
    std::atomic<bool> actions_visible_{false};
    Action pressed_action_ = Action::Confirm;
    bool action_pressed_ = false;
    std::function<void(Action)> action_handler_;
    ClipboardWriter clipboard_writer_;
    std::mutex transcript_mutex_;
    std::wstring transcript_;
    mutable std::mutex anchor_mutex_;
    VoiceOverlayAnchor anchor_;
    float levels_[kLevelCount] = {};
    float amplitudes_[kLevelCount] = {};
    float phases_[kLevelCount] = {};
    float freqs_[kLevelCount] = {};
    bool light_theme_ = false;

    struct ID2D1Factory *factory_ = nullptr;
    struct ID2D1HwndRenderTarget *render_target_ = nullptr;
    struct ID2D1SolidColorBrush *bar_brush_ = nullptr;
    struct ID2D1SolidColorBrush *bg_brush_ = nullptr;
    struct ID2D1SolidColorBrush *border_brush_ = nullptr;
    struct ID2D1SolidColorBrush *action_bg_brush_ = nullptr;
    struct ID2D1SolidColorBrush *text_brush_ = nullptr;
    struct IDWriteFactory *write_factory_ = nullptr;
    struct IDWriteTextFormat *text_format_ = nullptr;
    struct IDWriteTextFormat *processing_text_format_ = nullptr;
    struct IDWriteTextLayout *text_layout_ = nullptr;
    std::wstring layout_source_transcript_;

    enum class RecoveryFeedback
    {
        None,
        Copied,
        Failed,
    };

    // Background callers only publish this monotonic fence. UI messages reconcile against it without
    // holding a lock across Win32 calls, so the service send path never waits for the UI thread.
    std::atomic<std::uint64_t> session_fence_{0};

    // Cross-thread retained result. Only complete values are stored here; window messages carry
    // no transcript text or pointers.
    mutable std::mutex recovery_mutex_;
    bool recovery_active_ = false;
    std::uint64_t recovery_session_ = 0;
    std::uint64_t recovery_generation_ = 0;
    std::wstring recovery_text_;

    // UI-thread recovery state and timer binding.
    bool recovery_mode_ = false;
    RecoveryFeedback recovery_feedback_ = RecoveryFeedback::None;
    UINT_PTR recovery_timer_id_ = 0;
    UINT_PTR next_recovery_timer_id_ = 2;
    std::uint64_t recovery_timer_session_ = 0;
    std::uint64_t recovery_timer_generation_ = 0;
    std::uint64_t recovery_shown_session_ = 0;
    std::uint64_t recovery_shown_generation_ = 0;
    std::uint64_t pressed_recovery_session_ = 0;
    std::uint64_t pressed_recovery_generation_ = 0;

  public:
#ifdef MSIME_VOICE_OVERLAY_TEST_HOOKS
    HWND window_for_test() const
    {
        return post_hwnd_.load(std::memory_order_acquire);
    }
    std::wstring recovery_text_for_test() const;
    std::uint64_t recovery_generation_for_test() const;
    void post_recovery_dismiss_for_test(std::uint64_t session, std::uint64_t generation);
    void set_finish_recognition_ui_test_hook(std::function<void()> hook);
#endif
};
