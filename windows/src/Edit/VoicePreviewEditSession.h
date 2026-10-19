#pragma once

// Preview execution for one voice owner: create or update the exact TSF composition that the
// owner has bound, write the complete preview text once, and keep the composition open. This is
// a local execution unit; it does not emit the final delivery acknowledgement.
#include "VoiceFinalEditSession.h"

#include <functional>
#include <memory>

namespace VoicePreview
{
// UI-only strong holder for the exact composition this component created. The IME preallocates
// one per first preview and keeps it on the UI thread. StartComposition success retains into this
// holder before adoptComposition runs, so a failing/throw hook still leaves the exact composition
// available to targeted cleanup. It is not thread-safe and must never be stored in a worker-side
// registry; the owner must release it on the UI thread.
class StartedCompositionHolder final
{
  public:
    StartedCompositionHolder() noexcept = default;
    ~StartedCompositionHolder()
    {
        Reset();
    }

    StartedCompositionHolder(const StartedCompositionHolder &) = delete;
    StartedCompositionHolder &operator=(const StartedCompositionHolder &) = delete;

    // Retains composition and replaces any previous value. No locks; UI thread only.
    bool Retain(ITfComposition *composition) noexcept
    {
        if (composition == nullptr)
            return false;
        composition->AddRef();
        ITfComposition *previous = composition_;
        composition_ = composition;
        if (previous)
            previous->Release();
        return true;
    }

    ITfComposition *Get() const noexcept
    {
        return composition_;
    }

    void Reset() noexcept
    {
        ITfComposition *previous = composition_;
        composition_ = nullptr;
        if (previous)
            previous->Release();
    }

  private:
    ITfComposition *composition_ = nullptr;
};

struct PreviewExecutionHooks
{
    // Revalidates the live owner state: expected context, focus, worker generation and the exact
    // composition binding. It is called before queueing, immediately before StartComposition and
    // immediately before the document write. During the first preview, expectedComposition is
    // null until adoptComposition has bound the new composition.
    std::function<bool(const VoiceFinal::Task &, ITfContext *, ITfComposition *)> validateIdentity;

    // UI-only holder that receives the exact composition created by the first preview. Required
    // for a first preview (expectedComposition == null); ignored for updates and cancel sessions.
    std::shared_ptr<StartedCompositionHolder> startedCompositionHolder;

    // Monotonic GetTickCount64-compatible clock. Empty means the production clock.
    std::function<std::uint64_t()> now;

    // Called synchronously after StartComposition succeeded and before any text is written. The
    // exact new composition has already been retained in startedCompositionHolder. The hook
    // validates the initial wire revision and binds the exact new composition/local epoch. It may
    // re-enter COM. Returning false or throwing leaves the task UnknownOrPartial; cleanup must use
    // the holder's exact composition, and this component does not end an arbitrary current one.
    std::function<bool(TfEditCookie, ITfContext *, ITfComposition *)> adoptComposition;

    // Called after the full preview text, Collapse and SetSelection all returned S_OK. The
    // composition remains open. It may set display attributes and may re-enter COM. An exception
    // leaves the task UnknownOrPartial because the text is already in the document.
    std::function<void(TfEditCookie, ITfContext *, ITfComposition *)> afterPreview;

    // Additional cancellation fence. It must verify that the owner still owns this exact
    // composition and that no final request is applying. Called before queueing, immediately
    // before the empty write and immediately before EndComposition.
    std::function<bool(const VoiceFinal::Task &, ITfContext *, ITfComposition *)> canCancel;

    // Called once for the cancel end stage with the actual EndComposition result, or E_ABORT when
    // the post-write fence rejected the end. It is for precise local cleanup and cannot replace
    // or retry the TSF operation.
    std::function<void(TfEditCookie, ITfComposition *, HRESULT)> afterEndComposition;
};

HRESULT CreateVoicePreviewEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                      std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                      ITfEditSession **out);

HRESULT RequestVoicePreviewEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                       ITfComposition *expectedComposition, std::shared_ptr<VoiceFinal::Task> task,
                                       PreviewExecutionHooks hooks, DWORD flags, HRESULT *sessionResult);

HRESULT CreateVoicePreviewCancelEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                            std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                            ITfEditSession **out);

HRESULT RequestVoicePreviewCancelEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                             ITfComposition *expectedComposition,
                                             std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                             DWORD flags, HRESULT *sessionResult);
} // namespace VoicePreview
