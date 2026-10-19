#include "VoiceFinalEditSession.h"

#include <new>
#include <utility>

namespace VoiceFinal
{
namespace
{
using FanyImeVoiceFinal::AckResult;
using FanyImeVoiceFinal::Operation;

void ReleaseUnknown(IUnknown *unknown)
{
    if (unknown)
        unknown->Release();
}

std::uint64_t ReadNow(const ExecutionHooks &hooks)
{
    if (hooks.now)
        return hooks.now();
    return GetTickCount64();
}

bool TaskShapeIsValid(const Task &task, ITfComposition *expectedComposition)
{
    if (task.DeliveryId() == 0 || task.FocusToken() == 0 || task.DeadlineTick() == 0)
        return false;
    if (task.OperationKind() != Operation::Insert && task.OperationKind() != Operation::CommitComposition)
        return false;
    if (task.OperationKind() == Operation::CommitComposition && expectedComposition == nullptr)
        return false;
    if (task.Text().empty() || task.Text().size() > FanyImeVoiceFinal::kMaxTextUnits)
        return false;
    return FanyImeVoiceFinal::IsValidUtf16(task.Text());
}

bool CanExecute(const Task &task, ITfContext *context, ITfComposition *expectedComposition, const ExecutionHooks &hooks)
{
    if (!TaskShapeIsValid(task, expectedComposition))
        return false;

    try
    {
        if (ReadNow(hooks) >= task.DeadlineTick())
            return false;
        if (hooks.validateIdentity && !hooks.validateIdentity(task, context, expectedComposition))
            return false;
    }
    catch (...)
    {
        return false;
    }
    return true;
}

void NotifyCompletion(const Task::Completion &completion, const Task &task, AckResult result) noexcept
{
    if (!completion)
        return;
    try
    {
        completion(task, result);
    }
    catch (...)
    {
        // A transport callback must never unwind through the TSF edit-session call.
    }
}

class StrictEditSession final : public ITfEditSession
{
  public:
    StrictEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                      std::shared_ptr<Task> task, ExecutionHooks hooks)
        : owner_(owner), context_(context), expected_composition_(expectedComposition), task_(std::move(task)),
          hooks_(std::move(hooks)), ref_count_(1)
    {
        owner_->AddRef();
        context_->AddRef();
        if (expected_composition_)
            expected_composition_->AddRef();
    }

    StrictEditSession(const StrictEditSession &) = delete;
    StrictEditSession &operator=(const StrictEditSession &) = delete;

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr)
            return E_INVALIDARG;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
        {
            *object = static_cast<ITfEditSession *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        if (!task_ || task_->CurrentState() != Task::State::Queued)
            return S_FALSE;
        if (!CanExecute(*task_, context_, expected_composition_, hooks_))
            return RejectBeforeWrite(S_FALSE);

        TF_SELECTION selection = {};
        ULONG fetched = 0;
        HRESULT hr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (hr != S_OK || fetched != 1 || selection.range == nullptr)
        {
            ReleaseUnknown(selection.range);
            return RejectBeforeWrite(FAILED(hr) ? hr : E_FAIL);
        }

        ITfRange *target = nullptr;
        if (task_->OperationKind() == Operation::CommitComposition)
        {
            hr = expected_composition_->GetRange(&target);
            ReleaseUnknown(selection.range);
            selection.range = nullptr;
            if (hr != S_OK || target == nullptr)
            {
                ReleaseUnknown(target);
                return RejectBeforeWrite(FAILED(hr) ? hr : E_FAIL);
            }
        }
        else
        {
            target = selection.range;
        }

        // The request was validated before it entered the queue. Host state may have advanced
        // while queued, so repeat the complete fence immediately before the first possible write.
        if (!CanExecute(*task_, context_, expected_composition_, hooks_))
        {
            ReleaseUnknown(target);
            return RejectBeforeWrite(S_FALSE);
        }
        if (!task_->BeginApply())
        {
            ReleaseUnknown(target);
            return S_FALSE;
        }

        hr = target->SetText(ec, 0, reinterpret_cast<const WCHAR *>(task_->Text().data()),
                             static_cast<LONG>(task_->Text().size()));
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            return FAILED(hr) ? hr : E_FAIL;
        }

        hr = target->Collapse(ec, TF_ANCHOR_END);
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            return FAILED(hr) ? hr : E_FAIL;
        }

        TF_SELECTION after_write = {};
        after_write.range = target;
        after_write.style.ase = TF_AE_NONE;
        after_write.style.fInterimChar = FALSE;
        hr = context_->SetSelection(ec, 1, &after_write);
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            return FAILED(hr) ? hr : E_FAIL;
        }

        if (task_->OperationKind() == Operation::CommitComposition)
        {
            HRESULT endResult = E_ABORT;
            bool endInvoked = false;

            if (hooks_.beforeEndComposition)
            {
                bool beforeSucceeded = true;
                try
                {
                    hooks_.beforeEndComposition(ec);
                }
                catch (...)
                {
                    beforeSucceeded = false;
                }

                // The before hook may have re-entered COM and advanced focus, the composition
                // epoch, the worker generation or the monotonic deadline. The task is already
                // Applying, so re-use CanExecute, which checks the fencing fields but not State.
                if (beforeSucceeded && CanExecute(*task_, context_, expected_composition_, hooks_))
                {
                    endInvoked = true;
                    endResult = expected_composition_->EndComposition(ec);
                }
            }
            else
            {
                endInvoked = true;
                endResult = expected_composition_->EndComposition(ec);
            }

            if (!endInvoked)
                endResult = E_ABORT;

            if (hooks_.afterEndComposition)
            {
                try
                {
                    hooks_.afterEndComposition(ec, endResult);
                }
                catch (...)
                {
                    // The document may already contain the full write and possibly an ended
                    // composition. A cleanup exception can never turn this into no-write, and
                    // the EndComposition call is never retried.
                    task_->Finish(AckResult::UnknownOrPartial);
                    ReleaseUnknown(target);
                    return E_FAIL;
                }
            }

            if (!endInvoked || endResult != S_OK)
            {
                const HRESULT failure = endInvoked ? (FAILED(endResult) ? endResult : E_FAIL) : E_ABORT;
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(target);
                return failure;
            }
        }

        task_->Finish(AckResult::Applied);
        ReleaseUnknown(target);
        return S_OK;
    }

  public:
    ~StrictEditSession()
    {
        if (task_)
        {
            const Task::State state = task_->CurrentState();
            if (state == Task::State::Queued)
                task_->Finish(AckResult::RejectedNoWrite);
            else if (state == Task::State::Applying)
                task_->Finish(AckResult::UnknownOrPartial);
        }
        ReleaseUnknown(expected_composition_);
        ReleaseUnknown(context_);
        ReleaseUnknown(owner_);
    }

  private:
    HRESULT RejectBeforeWrite(HRESULT cause)
    {
        task_->Finish(AckResult::RejectedNoWrite);
        return cause == S_OK ? E_ABORT : cause;
    }

    IUnknown *owner_;
    ITfContext *context_;
    ITfComposition *expected_composition_;
    std::shared_ptr<Task> task_;
    ExecutionHooks hooks_;
    LONG ref_count_;
};
} // namespace

Task::Task(std::uint64_t deliveryId, std::uint64_t focusToken, std::uint64_t workerGeneration,
           std::uint64_t compositionEpoch, std::uint64_t deadlineTick, FanyImeVoiceFinal::Operation operation,
           std::u16string text, Completion completion)
    : delivery_id_(deliveryId), focus_token_(focusToken), worker_generation_(workerGeneration),
      composition_epoch_(compositionEpoch), deadline_tick_(deadlineTick), operation_(operation), text_(std::move(text)),
      completion_(std::move(completion))
{
}

Task::State Task::CurrentState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool Task::HasResult() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return has_result_;
}

FanyImeVoiceFinal::AckResult Task::Result() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

bool Task::Cancel()
{
    Completion completion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Queued)
            return false;
        CompleteLocked(FanyImeVoiceFinal::AckResult::RejectedNoWrite, completion);
    }
    NotifyCompletion(completion, *this, FanyImeVoiceFinal::AckResult::RejectedNoWrite);
    return true;
}

bool Task::BeginApply()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Queued)
        return false;
    state_ = State::Applying;
    return true;
}

bool Task::Finish(FanyImeVoiceFinal::AckResult result)
{
    Completion completion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Finished)
            return false;
        CompleteLocked(result, completion);
    }
    NotifyCompletion(completion, *this, result);
    return true;
}

void Task::CompleteLocked(FanyImeVoiceFinal::AckResult result, Completion &completion)
{
    state_ = State::Finished;
    has_result_ = true;
    result_ = result;
    completion = std::move(completion_);
}

HRESULT CreateVoiceFinalEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                    std::shared_ptr<Task> task, ExecutionHooks hooks, ITfEditSession **out)
{
    if (out == nullptr)
    {
        if (task && task->CurrentState() == Task::State::Queued)
            task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (!task)
        return E_INVALIDARG;
    if (task->CurrentState() != Task::State::Queued)
        return S_FALSE;
    if (owner == nullptr || context == nullptr)
    {
        task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }
    if (!TaskShapeIsValid(*task, expectedComposition))
    {
        task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }

    auto *session = new (std::nothrow) StrictEditSession(owner, context, expectedComposition, task, std::move(hooks));
    if (session == nullptr)
    {
        task->Finish(AckResult::RejectedNoWrite);
        return E_OUTOFMEMORY;
    }
    *out = session;
    return S_OK;
}

HRESULT RequestVoiceFinalEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                     ITfComposition *expectedComposition, std::shared_ptr<Task> task,
                                     ExecutionHooks hooks, DWORD flags, HRESULT *sessionResult)
{
    HRESULT ignored = E_FAIL;
    if (sessionResult == nullptr)
        sessionResult = &ignored;
    *sessionResult = E_FAIL;

    if (!task)
        return E_INVALIDARG;
    if (task->CurrentState() != Task::State::Queued)
        return S_FALSE;
    if (owner == nullptr || context == nullptr || !CanExecute(*task, context, expectedComposition, hooks))
    {
        task->Finish(AckResult::RejectedNoWrite);
        *sessionResult = E_ABORT;
        return S_FALSE;
    }

    ITfEditSession *session = nullptr;
    HRESULT hr = CreateVoiceFinalEditSession(owner, context, expectedComposition, task, std::move(hooks), &session);
    if (hr != S_OK)
    {
        task->Finish(AckResult::RejectedNoWrite);
        *sessionResult = hr;
        return hr;
    }

    const HRESULT requestHr = context->RequestEditSession(clientId, session, flags, sessionResult);
    session->Release();
    if (FAILED(requestHr))
    {
        task->Finish(AckResult::RejectedNoWrite);
        return requestHr;
    }
    if (*sessionResult != TF_S_ASYNC && *sessionResult != S_OK)
        task->Finish(AckResult::RejectedNoWrite);
    return requestHr;
}
} // namespace VoiceFinal
