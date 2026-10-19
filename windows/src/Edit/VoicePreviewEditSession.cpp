#include "VoicePreviewEditSession.h"

#include <new>
#include <utility>

namespace VoicePreview
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

std::uint64_t ReadNow(const PreviewExecutionHooks &hooks)
{
    if (hooks.now)
        return hooks.now();
    return GetTickCount64();
}

bool PreviewShapeIsValid(const VoiceFinal::Task &task)
{
    if (task.DeliveryId() == 0 || task.FocusToken() == 0 || task.DeadlineTick() == 0)
        return false;
    if (task.OperationKind() != Operation::Preview)
        return false;
    if (task.Text().empty() || task.Text().size() > FanyImeVoiceFinal::kMaxTextUnits)
        return false;
    return FanyImeVoiceFinal::IsValidUtf16(task.Text());
}

bool CancelShapeIsValid(const VoiceFinal::Task &task, ITfComposition *expectedComposition)
{
    if (task.DeliveryId() == 0 || task.FocusToken() == 0 || task.DeadlineTick() == 0)
        return false;
    if (task.OperationKind() != Operation::Cancel || !task.Text().empty())
        return false;
    return expectedComposition != nullptr;
}

bool DeadlineRemaining(const VoiceFinal::Task &task, const PreviewExecutionHooks &hooks)
{
    return ReadNow(hooks) < task.DeadlineTick();
}

bool Fence(const VoiceFinal::Task &task, ITfContext *context, ITfComposition *composition,
           const PreviewExecutionHooks &hooks)
{
    if (hooks.validateIdentity && !hooks.validateIdentity(task, context, composition))
        return false;
    // validateIdentity may re-enter COM and consume unbounded time. The clock is authoritative
    // after it, including for the fence immediately before StartComposition/SetText.
    return DeadlineRemaining(task, hooks);
}

bool FirstPreviewHasHolder(bool cancel, ITfComposition *expectedComposition, const PreviewExecutionHooks &hooks)
{
    return cancel || expectedComposition != nullptr || hooks.startedCompositionHolder != nullptr;
}

bool CanPreview(const VoiceFinal::Task &task, ITfContext *context, ITfComposition *composition,
                const PreviewExecutionHooks &hooks)
{
    if (!PreviewShapeIsValid(task))
        return false;
    try
    {
        return Fence(task, context, composition, hooks);
    }
    catch (...)
    {
        return false;
    }
}

bool CanCancel(const VoiceFinal::Task &task, ITfContext *context, ITfComposition *composition,
               const PreviewExecutionHooks &hooks)
{
    if (!CancelShapeIsValid(task, composition))
        return false;
    try
    {
        if (!Fence(task, context, composition, hooks))
            return false;
        if (hooks.canCancel && !hooks.canCancel(task, context, composition))
            return false;
        // canCancel may re-enter COM too. Recheck after it, before the empty write and before
        // EndComposition.
        if (!DeadlineRemaining(task, hooks))
            return false;
    }
    catch (...)
    {
        return false;
    }
    return true;
}

class PreviewEditSession final : public ITfEditSession
{
  public:
    PreviewEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                       std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks, bool cancel)
        : owner_(owner), context_(context), expected_composition_(expectedComposition), task_(std::move(task)),
          hooks_(std::move(hooks)), cancel_(cancel), ref_count_(1)
    {
        owner_->AddRef();
        context_->AddRef();
        if (expected_composition_)
            expected_composition_->AddRef();
    }

    PreviewEditSession(const PreviewEditSession &) = delete;
    PreviewEditSession &operator=(const PreviewEditSession &) = delete;

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
        if (!task_ || task_->CurrentState() != VoiceFinal::Task::State::Queued)
            return S_FALSE;
        return cancel_ ? DoCancel(ec) : DoPreview(ec);
    }

    ~PreviewEditSession()
    {
        if (task_)
        {
            const VoiceFinal::Task::State state = task_->CurrentState();
            if (state == VoiceFinal::Task::State::Queued)
                task_->Finish(AckResult::RejectedNoWrite);
            else if (state == VoiceFinal::Task::State::Applying)
                task_->Finish(AckResult::UnknownOrPartial);
        }
        ReleaseUnknown(expected_composition_);
        ReleaseUnknown(context_);
        ReleaseUnknown(owner_);
    }

  private:
    HRESULT RejectPreviewBeforeWrite(HRESULT cause)
    {
        task_->Finish(AckResult::RejectedNoWrite);
        return cause == S_OK ? E_ABORT : cause;
    }

    HRESULT FinishUnknown(HRESULT cause)
    {
        task_->Finish(AckResult::UnknownOrPartial);
        return cause == S_OK ? E_FAIL : cause;
    }

    HRESULT ReadSelection(TfEditCookie ec, TF_SELECTION *selection)
    {
        ULONG fetched = 0;
        HRESULT hr = context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, selection, &fetched);
        if (hr != S_OK || fetched != 1 || selection->range == nullptr)
        {
            ReleaseUnknown(selection->range);
            selection->range = nullptr;
            return RejectPreviewBeforeWrite(FAILED(hr) ? hr : E_FAIL);
        }
        return S_OK;
    }

    HRESULT ReadCompositionRange(ITfComposition *composition, ITfRange **range)
    {
        *range = nullptr;
        const HRESULT hr = composition->GetRange(range);
        if (hr != S_OK || *range == nullptr)
        {
            ReleaseUnknown(*range);
            *range = nullptr;
            return FAILED(hr) ? hr : E_FAIL;
        }
        return S_OK;
    }

    HRESULT DoPreview(TfEditCookie ec)
    {
        if (!CanPreview(*task_, context_, expected_composition_, hooks_))
            return RejectPreviewBeforeWrite(S_FALSE);

        TF_SELECTION selection = {};
        HRESULT hr = ReadSelection(ec, &selection);
        if (hr != S_OK)
            return hr;

        ITfRange *target = nullptr;
        if (expected_composition_)
        {
            ReleaseUnknown(selection.range);
            selection.range = nullptr;
            hr = ReadCompositionRange(expected_composition_, &target);
            if (hr != S_OK)
                return RejectPreviewBeforeWrite(hr);
        }
        else
        {
            target = selection.range;
            selection.range = nullptr;
        }

        // The request was accepted while queued; host state can advance before execution and
        // again while selecting the range. Fence immediately before any possible write.
        bool canApply = false;
        try
        {
            canApply = CanPreview(*task_, context_, expected_composition_, hooks_);
        }
        catch (...)
        {
            canApply = false;
        }
        if (!canApply)
        {
            ReleaseUnknown(target);
            return RejectPreviewBeforeWrite(S_FALSE);
        }
        // A first preview must have its UI holder allocated before the request. A non-empty
        // holder here is stale; do not create a second composition behind an existing binding.
        if (expected_composition_ == nullptr &&
            (!hooks_.startedCompositionHolder || hooks_.startedCompositionHolder->Get() != nullptr))
        {
            ReleaseUnknown(target);
            return RejectPreviewBeforeWrite(S_FALSE);
        }
        if (!task_->BeginApply())
        {
            ReleaseUnknown(target);
            return S_FALSE;
        }

        ITfComposition *composition = expected_composition_;
        ITfComposition *startedComposition = nullptr;
        if (expected_composition_ == nullptr)
        {
            ITfContextComposition *contextComposition = nullptr;
            hr = context_->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void **>(&contextComposition));
            if (hr != S_OK || contextComposition == nullptr)
            {
                ReleaseUnknown(contextComposition);
                task_->Finish(AckResult::RejectedNoWrite);
                ReleaseUnknown(target);
                return FAILED(hr) ? hr : E_NOINTERFACE;
            }

            ITfCompositionSink *sink = nullptr;
            hr = owner_->QueryInterface(IID_ITfCompositionSink, reinterpret_cast<void **>(&sink));
            if (hr != S_OK || sink == nullptr)
            {
                ReleaseUnknown(sink);
                contextComposition->Release();
                task_->Finish(AckResult::RejectedNoWrite);
                ReleaseUnknown(target);
                return FAILED(hr) ? hr : E_NOINTERFACE;
            }

            // QueryInterface can re-enter the host. Keep the final fence adjacent to the first
            // operation that can create a composition.
            canApply = false;
            try
            {
                canApply = CanPreview(*task_, context_, expected_composition_, hooks_);
            }
            catch (...)
            {
                canApply = false;
            }
            if (!canApply || !hooks_.startedCompositionHolder ||
                hooks_.startedCompositionHolder->Get() != nullptr)
            {
                ReleaseUnknown(sink);
                contextComposition->Release();
                task_->Finish(AckResult::RejectedNoWrite);
                ReleaseUnknown(target);
                return E_ABORT;
            }

            hr = contextComposition->StartComposition(ec, target, sink, &startedComposition);
            ReleaseUnknown(sink);
            contextComposition->Release();
            if (hr != S_OK || startedComposition == nullptr)
            {
                ReleaseUnknown(startedComposition);
                task_->Finish(AckResult::RejectedNoWrite);
                ReleaseUnknown(target);
                return FAILED(hr) ? hr : E_FAIL;
            }

            // StartComposition has already created the document composition. Retain the exact
            // object in the caller's UI holder before the only hook that can fail here, so the
            // owner can perform targeted cleanup without guessing at the current composition.
            if (!hooks_.startedCompositionHolder->Retain(startedComposition))
            {
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(startedComposition);
                ReleaseUnknown(target);
                return E_FAIL;
            }

            composition = startedComposition;
            bool adopted = false;
            try
            {
                adopted = hooks_.adoptComposition && hooks_.adoptComposition(ec, context_, composition);
            }
            catch (...)
            {
                adopted = false;
            }
            if (!adopted)
            {
                // The composition may already exist in the document. It is not safe to end an
                // arbitrary current composition, so leave it to the owner's precise cleanup.
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(startedComposition);
                ReleaseUnknown(target);
                return E_FAIL;
            }

            ReleaseUnknown(target);
            target = nullptr;
            hr = ReadCompositionRange(composition, &target);
            if (hr != S_OK)
            {
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(startedComposition);
                return FAILED(hr) ? hr : E_FAIL;
            }

            // adoptComposition may have advanced the owner's local binding/epoch. Revalidate
            // against the just-bound composition instead of the pre-start expected pointer.
            canApply = false;
            try
            {
                canApply = CanPreview(*task_, context_, composition, hooks_);
            }
            catch (...)
            {
                canApply = false;
            }
            if (!canApply)
            {
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(target);
                ReleaseUnknown(startedComposition);
                return E_ABORT;
            }
        }

        hr = target->SetText(ec, 0, reinterpret_cast<const WCHAR *>(task_->Text().data()),
                             static_cast<LONG>(task_->Text().size()));
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            ReleaseUnknown(startedComposition);
            return FAILED(hr) ? hr : E_FAIL;
        }

        hr = target->Collapse(ec, TF_ANCHOR_END);
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            ReleaseUnknown(startedComposition);
            return FAILED(hr) ? hr : E_FAIL;
        }

        TF_SELECTION afterWrite = {};
        afterWrite.range = target;
        afterWrite.style.ase = TF_AE_NONE;
        afterWrite.style.fInterimChar = FALSE;
        hr = context_->SetSelection(ec, 1, &afterWrite);
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(target);
            ReleaseUnknown(startedComposition);
            return FAILED(hr) ? hr : E_FAIL;
        }

        if (hooks_.afterPreview)
        {
            try
            {
                hooks_.afterPreview(ec, context_, composition);
            }
            catch (...)
            {
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(target);
                ReleaseUnknown(startedComposition);
                return E_FAIL;
            }
        }

        task_->Finish(AckResult::Applied);
        ReleaseUnknown(target);
        ReleaseUnknown(startedComposition);
        return S_OK;
    }

    HRESULT DoCancel(TfEditCookie ec)
    {
        if (!CanCancel(*task_, context_, expected_composition_, hooks_))
            return RejectPreviewBeforeWrite(S_FALSE);

        ITfRange *range = nullptr;
        HRESULT hr = ReadCompositionRange(expected_composition_, &range);
        if (hr != S_OK)
            return RejectPreviewBeforeWrite(hr);

        bool canApply = false;
        try
        {
            canApply = CanCancel(*task_, context_, expected_composition_, hooks_);
        }
        catch (...)
        {
            canApply = false;
        }
        if (!canApply)
        {
            ReleaseUnknown(range);
            return RejectPreviewBeforeWrite(S_FALSE);
        }
        if (!task_->BeginApply())
        {
            ReleaseUnknown(range);
            return S_FALSE;
        }

        hr = range->SetText(ec, 0, nullptr, 0);
        if (hr != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(range);
            return FAILED(hr) ? hr : E_FAIL;
        }

        canApply = false;
        try
        {
            canApply = CanCancel(*task_, context_, expected_composition_, hooks_);
        }
        catch (...)
        {
            canApply = false;
        }
        if (!canApply)
        {
            if (hooks_.afterEndComposition)
            {
                try
                {
                    hooks_.afterEndComposition(ec, expected_composition_, E_ABORT);
                }
                catch (...)
                {
                }
            }
            task_->Finish(AckResult::UnknownOrPartial);
            ReleaseUnknown(range);
            return E_ABORT;
        }

        const HRESULT endResult = expected_composition_->EndComposition(ec);
        if (hooks_.afterEndComposition)
        {
            try
            {
                hooks_.afterEndComposition(ec, expected_composition_, endResult);
            }
            catch (...)
            {
                task_->Finish(AckResult::UnknownOrPartial);
                ReleaseUnknown(range);
                return E_FAIL;
            }
        }

        ReleaseUnknown(range);
        if (endResult != S_OK)
        {
            task_->Finish(AckResult::UnknownOrPartial);
            return FAILED(endResult) ? endResult : E_FAIL;
        }

        task_->Finish(AckResult::Applied);
        return S_OK;
    }

    IUnknown *owner_;
    ITfContext *context_;
    ITfComposition *expected_composition_;
    std::shared_ptr<VoiceFinal::Task> task_;
    PreviewExecutionHooks hooks_;
    bool cancel_;
    LONG ref_count_;
};

HRESULT CreateSession(bool cancel, IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                      std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks, ITfEditSession **out)
{
    if (out == nullptr)
    {
        if (task && task->CurrentState() == VoiceFinal::Task::State::Queued)
            task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (!task || owner == nullptr || context == nullptr)
    {
        if (task && task->CurrentState() == VoiceFinal::Task::State::Queued)
            task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }
    if (task->CurrentState() != VoiceFinal::Task::State::Queued)
        return S_FALSE;
    if (cancel ? !CancelShapeIsValid(*task, expectedComposition)
               : (!PreviewShapeIsValid(*task) || !FirstPreviewHasHolder(false, expectedComposition, hooks)))
    {
        task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }

    auto *session =
        new (std::nothrow) PreviewEditSession(owner, context, expectedComposition, task, std::move(hooks), cancel);
    if (session == nullptr)
    {
        task->Finish(AckResult::RejectedNoWrite);
        return E_OUTOFMEMORY;
    }
    *out = session;
    return S_OK;
}

HRESULT RequestSession(bool cancel, TfClientId clientId, IUnknown *owner, ITfContext *context,
                       ITfComposition *expectedComposition, std::shared_ptr<VoiceFinal::Task> task,
                       PreviewExecutionHooks hooks, DWORD flags, HRESULT *sessionResult)
{
    HRESULT ignored = E_FAIL;
    if (sessionResult == nullptr)
        sessionResult = &ignored;
    *sessionResult = E_FAIL;

    if (!task)
        return E_INVALIDARG;
    if (owner == nullptr || context == nullptr)
    {
        if (task->CurrentState() == VoiceFinal::Task::State::Queued)
            task->Finish(AckResult::RejectedNoWrite);
        return E_INVALIDARG;
    }
    if (task->CurrentState() != VoiceFinal::Task::State::Queued)
        return S_FALSE;
    if (!FirstPreviewHasHolder(cancel, expectedComposition, hooks))
    {
        task->Finish(AckResult::RejectedNoWrite);
        *sessionResult = E_INVALIDARG;
        return E_INVALIDARG;
    }
    const bool canExecute = cancel ? CanCancel(*task, context, expectedComposition, hooks)
                                   : CanPreview(*task, context, expectedComposition, hooks);
    if (!canExecute)
    {
        task->Finish(AckResult::RejectedNoWrite);
        *sessionResult = E_ABORT;
        return S_FALSE;
    }

    ITfEditSession *session = nullptr;
    HRESULT hr = CreateSession(cancel, owner, context, expectedComposition, task, std::move(hooks), &session);
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
} // namespace

HRESULT CreateVoicePreviewEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                      std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                      ITfEditSession **out)
{
    return CreateSession(false, owner, context, expectedComposition, std::move(task), std::move(hooks), out);
}

HRESULT RequestVoicePreviewEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                       ITfComposition *expectedComposition, std::shared_ptr<VoiceFinal::Task> task,
                                       PreviewExecutionHooks hooks, DWORD flags, HRESULT *sessionResult)
{
    return RequestSession(false, clientId, owner, context, expectedComposition, std::move(task), std::move(hooks),
                          flags, sessionResult);
}

HRESULT CreateVoicePreviewCancelEditSession(IUnknown *owner, ITfContext *context, ITfComposition *expectedComposition,
                                            std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                            ITfEditSession **out)
{
    return CreateSession(true, owner, context, expectedComposition, std::move(task), std::move(hooks), out);
}

HRESULT RequestVoicePreviewCancelEditSession(TfClientId clientId, IUnknown *owner, ITfContext *context,
                                             ITfComposition *expectedComposition,
                                             std::shared_ptr<VoiceFinal::Task> task, PreviewExecutionHooks hooks,
                                             DWORD flags, HRESULT *sessionResult)
{
    return RequestSession(true, clientId, owner, context, expectedComposition, std::move(task), std::move(hooks), flags,
                          sessionResult);
}
} // namespace VoicePreview
