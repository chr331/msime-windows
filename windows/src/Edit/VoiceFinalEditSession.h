#pragma once

// Strict, standalone execution of a complete final voice delivery in one TSF edit session.
// This unit deliberately does not include Private.h or the IME singleton: the production
// WindowProc can request it directly, and native SDK tests can link the same implementation.
#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "../../../engine/contracts/voice_final_delivery.h"

namespace VoiceFinal
{
class Task
{
  public:
    enum class State
    {
        Queued,
        Applying,
        Finished,
    };

    using Completion = std::function<void(const Task &, FanyImeVoiceFinal::AckResult)>;

    Task(std::uint64_t deliveryId, std::uint64_t focusToken, std::uint64_t workerGeneration,
         std::uint64_t compositionEpoch, std::uint64_t deadlineTick, FanyImeVoiceFinal::Operation operation,
         std::u16string text, Completion completion = {});

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    std::uint64_t DeliveryId() const noexcept
    {
        return delivery_id_;
    }
    std::uint64_t FocusToken() const noexcept
    {
        return focus_token_;
    }
    std::uint64_t WorkerGeneration() const noexcept
    {
        return worker_generation_;
    }
    std::uint64_t CompositionEpoch() const noexcept
    {
        return composition_epoch_;
    }
    std::uint64_t DeadlineTick() const noexcept
    {
        return deadline_tick_;
    }
    FanyImeVoiceFinal::Operation OperationKind() const noexcept
    {
        return operation_;
    }
    const std::u16string &Text() const noexcept
    {
        return text_;
    }

    State CurrentState() const;
    bool HasResult() const;
    FanyImeVoiceFinal::AckResult Result() const;

    // Cancel is allowed only before execution starts. Applying and Finished tasks are never
    // converted back to a no-write result by a late composition/global cancellation.
    bool Cancel();

    // This is the only transition from Queued to Applying. It is intentionally one-way.
    bool BeginApply();

    // Finishes the task once. The completion callback runs without the task lock held.
    bool Finish(FanyImeVoiceFinal::AckResult result);

  private:
    void CompleteLocked(FanyImeVoiceFinal::AckResult result, Completion &completion);

    const std::uint64_t delivery_id_;
    const std::uint64_t focus_token_;
    const std::uint64_t worker_generation_;
    const std::uint64_t composition_epoch_;
    const std::uint64_t deadline_tick_;
    const FanyImeVoiceFinal::Operation operation_;
    const std::u16string text_;
    Completion completion_;

    mutable std::mutex mutex_;
    State state_ = State::Queued;
    bool has_result_ = false;
    FanyImeVoiceFinal::AckResult result_ = FanyImeVoiceFinal::AckResult::RejectedNoWrite;
};

struct ExecutionHooks
{
    // Revalidates the live owner state: expected context, focus, composition epoch and worker
    // generation. It is called before queueing and again immediately before the write starts.
    std::function<bool(const Task &, ITfContext *, ITfComposition *)> validateIdentity;

    // Monotonic GetTickCount64-compatible clock. Empty means the production clock.
    std::function<std::uint64_t()> now;

    // Called only for CommitComposition, after the final full SetText/Collapse/SetSelection all
    // returned S_OK and immediately before the sole EndComposition. It may re-enter COM. If it
    // throws or the identity/deadline fence no longer holds afterwards, EndComposition is not
    // called. It is not called when an earlier write or selection operation fails.
    std::function<void(TfEditCookie)> beforeEndComposition;

    // Called once for this end stage with the actual EndComposition result, or E_ABORT when the
    // before hook failed or the post-hook fence rejected the end. It is not called when the
    // write/selection phase fails before this stage. It is for local cleanup and cannot replace
    // or retry the TSF document operations.
    std::function<void(TfEditCookie, HRESULT)> afterEndComposition;
};

HRESULT CreateVoiceFinalEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                    std::shared_ptr<Task> task, ExecutionHooks hooks, ITfEditSession **out);

// Calls the real ITfContext::RequestEditSession. S_OK/TF_S_ASYNC means the context accepted the
// request; it does not mean the text was applied. The completion callback still owns the ACK.
HRESULT RequestVoiceFinalEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                     ITfComposition *expectedComposition, std::shared_ptr<Task> task,
                                     ExecutionHooks hooks, DWORD flags, HRESULT *sessionResult);
} // namespace VoiceFinal
