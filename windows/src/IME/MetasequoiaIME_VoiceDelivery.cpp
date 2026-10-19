#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "../IPC/VoiceDeliveryRegistry.h"
#include "../Edit/VoicePreviewEditSession.h"
#include "CompositionProcessorEngine.h"
#include "stats_collector.h"
#include "../../../engine/contracts/ipc_negotiation.h"

using namespace FanyImeVoiceFinal;
using VoiceWork = VoiceDeliveryRegistry::Work;

struct VoiceDeliveryUiState
{
    struct Binding
    {
        std::shared_ptr<VoiceDeliveryRegistry::Owner> owner;
        ITfContext *context = nullptr;
        ITfComposition *composition = nullptr;
        std::uint64_t epoch = 0;
        std::shared_ptr<VoicePreview::StartedCompositionHolder> started =
            std::make_shared<VoicePreview::StartedCompositionHolder>();
        std::deque<VoiceWork> pending;
        std::shared_ptr<VoiceFinal::Task> active;
        Operation activeOperation = Operation::Cancel;
        std::uint64_t activeRevision = 0;
        bool cleanupRequested = false;
        bool initialized = false;
        bool ended = false;
        void CancelSupersededTask()
        {
            // Cleanup is already the response to cancellation and must still reach its grant.
            if (active && activeOperation != Operation::Cancel && (owner->cancelled || owner->failed))
                active->Cancel();
        }
        ~Binding()
        {
            if (composition)
                composition->Release();
            if (context)
                context->Release();
        }
    };
    // UI-only: neither registry nor worker callbacks retain any of these COM references.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<Binding>> bindings;
    bool draining = false;
};

void CMetasequoiaIME::_InitializeVoiceDelivery()
{
    const HWND window = _msgWndHandle;
    _voiceDeliveries = std::make_shared<VoiceDeliveryRegistry>(
        [window] { return window && PostMessageW(window, WM_VoiceDelivery, 0, 0) != FALSE; });
    _voiceDeliveryUi = std::make_shared<VoiceDeliveryUiState>();
}

void CMetasequoiaIME::_InvalidateVoiceDeliveries()
{
    if (_voiceDeliveries)
        _voiceDeliveries->Retire();
    _DrainVoiceDeliveries();
}

bool CMetasequoiaIME::_ShouldPreserveVoiceComposition(ITfComposition *composition) const
{
    if (!composition || !_voiceDeliveryUi)
        return false;
    for (const auto &entry : _voiceDeliveryUi->bindings)
    {
        const auto &binding = entry.second;
        if (binding->composition != composition || _pComposition != composition ||
            _CaptureCompositionEpoch() != binding->epoch)
            continue;
        if (binding->owner->finalUnknown ||
            (binding->active && binding->activeOperation == Operation::CommitComposition &&
             binding->active->CurrentState() == VoiceFinal::Task::State::Applying))
            return true;
    }
    return false;
}

void CMetasequoiaIME::_DrainVoiceDeliveries()
{
    const auto registry = _voiceDeliveries;
    const auto ui = _voiceDeliveryUi;
    if (!registry || !ui || ui->draining)
        return;
    ui->draining = true;
    struct DrainGuard
    {
        bool &value;
        ~DrainGuard()
        {
            value = false;
        }
    } guard{ui->draining};
    for (auto &work : registry->Take())
    {
        auto owner = work.owner;
        const auto key = std::make_pair(owner->generation, owner->id);
        auto &slot = ui->bindings[key];
        if (!slot)
        {
            slot = std::make_shared<VoiceDeliveryUiState::Binding>();
            slot->owner = owner;
            slot->epoch = owner->initialEpoch;
        }
        auto binding = slot;
        if (work.completion)
        {
            if (binding->activeOperation == work.operation && binding->activeRevision == work.revision)
                binding->active.reset();
            if (work.operation == Operation::Preview && work.result == AckResult::Applied)
                binding->initialized = true;
            if (work.operation == Operation::Cancel && work.result == AckResult::Applied)
                binding->ended = true;
            if (work.operation == Operation::Insert || work.operation == Operation::CommitComposition)
            {
                owner->finalApplying = false;
                if (work.result == AckResult::Applied)
                    binding->ended = true;
                if (work.result == AckResult::RejectedNoWrite)
                    owner->failed = true;
            }
        }
        else if (work.operation != Operation::Cancel)
        {
            if (binding->pending.size() >= 64)
                owner->failed = true;
            binding->pending.push_back(std::move(work));
        }
    }

    // Iterate a snapshot: COM callbacks may invalidate the registry while RequestEditSession
    // is executing. No map iterator or registry lock survives a COM call.
    std::vector<std::shared_ptr<VoiceDeliveryUiState::Binding>> snapshot;
    for (const auto &entry : ui->bindings)
        snapshot.push_back(entry.second);
    for (auto &binding : snapshot)
    {
        const auto owner = binding->owner;
        const auto basicCurrent = [this, owner]() {
            return Global::g_connected && _workerCommitReady.load(std::memory_order_acquire) &&
                   !_localSessionResetPending.load(std::memory_order_acquire) &&
                   _workerPipeGeneration.load(std::memory_order_acquire) == owner->generation &&
                   _expectedWorkerFocusToken.load(std::memory_order_acquire) == owner->focus &&
                   _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) == owner->focus &&
                   (_voiceServerCapabilities.load(std::memory_order_acquire) & FanyImeProtocol::VoiceDeliveryAck);
        };
        // COM identity comparison is followed by all numeric fences again; QueryInterface and
        // GetFocus/GetTop may re-enter the service and replace its context or generation.
        const auto validate = [this, binding, owner, basicCurrent](const VoiceFinal::Task &task, ITfContext *context,
                                                                   ITfComposition *composition) {
            const bool first =
                task.OperationKind() == Operation::Preview && task.CompositionEpoch() == 1 && !binding->initialized;
            if (!basicCurrent() || owner->cancelled || owner->failed || binding->ended ||
                (task.OperationKind() == Operation::Preview && owner->sealed && !first))
                return false;
            ITfDocumentMgr *document = nullptr;
            ITfContext *top = nullptr;
            bool same = _pThreadMgr && SUCCEEDED(_pThreadMgr->GetFocus(&document)) && document &&
                        SUCCEEDED(document->GetTop(&top)) && top && _IsSameComObject(top, context);
            if (top)
                top->Release();
            if (document)
                document->Release();
            if (!same || !basicCurrent() || owner->cancelled || owner->failed || binding->ended ||
                (task.OperationKind() == Operation::Preview && owner->sealed && !first) ||
                _CaptureCompositionEpoch() != binding->epoch)
                return false;
            if (composition)
            {
                if (binding->composition != composition || _pComposition != composition || !_pContext)
                    return false;
                same = _IsSameComObject(_pContext, context);
                if (!same || !basicCurrent() || owner->cancelled || owner->failed || binding->ended ||
                    (task.OperationKind() == Operation::Preview && owner->sealed && !first) ||
                    _CaptureCompositionEpoch() != binding->epoch || _pComposition != composition)
                    return false;
            }
            else if (_pComposition || (task.OperationKind() == Operation::CommitComposition))
                return false;
            if (task.OperationKind() != Operation::Preview && task.CurrentState() == VoiceFinal::Task::State::Applying)
                owner->finalApplying = true;
            return true;
        };
        const auto exactCleanup = [this, binding, owner](const VoiceFinal::Task &, ITfContext *context,
                                                         ITfComposition *composition) {
            if (owner->finalApplying || owner->finalUnknown || !composition || binding->ended)
                return false;
            if (_pComposition == composition && _CaptureCompositionEpoch() == binding->epoch)
                return _pContext && _IsSameComObject(_pContext, context) && _pComposition == composition &&
                       _CaptureCompositionEpoch() == binding->epoch && !owner->finalApplying && !owner->finalUnknown;
            // A failed adoption still belongs to the preallocated holder. Never erase a newer
            // composition, even if the host reused the same context object.
            return !_pComposition && binding->started->Get() == composition &&
                   _CaptureCompositionEpoch() == owner->initialEpoch;
        };
        const auto detach = [this, binding](TfEditCookie ec, ITfComposition *composition, HRESULT result) {
            if (result != S_OK)
                return;
            binding->ended = true;
            if (_pComposition != composition || _CaptureCompositionEpoch() != binding->epoch)
                return;
            ITfComposition *old = _pComposition;
            ITfContext *context = _pContext;
            _pComposition = nullptr;
            _pContext = nullptr;
            _voiceCompositionActive = false;
            auto epoch = _compositionEpoch.fetch_add(1) + 1;
            if (!epoch)
                _compositionEpoch.fetch_add(1);
            // Publish detached ownership before any Release, attribute or candidate callback.
            old->Release();
            if (_pComposition == nullptr && _CaptureCompositionEpoch() == (epoch ? epoch : 1))
            {
                if (context)
                    _ClearCompositionDisplayAttributes(ec, context, composition);
                if (_pComposition == nullptr)
                    _DeleteCandidateList(FALSE, context);
                if (_pComposition == nullptr)
                    _ClearCreatingWordState();
            }
            if (context)
                context->Release();
        };

        binding->CancelSupersededTask();
        if (binding->active)
            continue;
        while (!binding->pending.empty() && !binding->active)
        {
            VoiceWork work = std::move(binding->pending.front());
            binding->pending.pop_front();
            const bool isPreview = work.operation == Operation::Preview;
            const bool initialize = isPreview && work.revision == 1 && !binding->composition;
            if (owner->cancelled || owner->failed || binding->ended || (isPreview && owner->sealed && !initialize) ||
                GetTickCount64() >= work.deadline)
            {
                if (!isPreview)
                    registry->Complete(work, AckResult::RejectedNoWrite);
                continue;
            }
            if (!binding->context)
            {
                ITfDocumentMgr *document = nullptr;
                if (_pThreadMgr && SUCCEEDED(_pThreadMgr->GetFocus(&document)) && document)
                {
                    document->GetTop(&binding->context);
                    document->Release();
                }
            }
            if (!binding->context)
            {
                registry->Complete(work, AckResult::RejectedNoWrite);
                continue;
            }
            // Task completion owns only numeric/text work and a weak registry. UI COM bindings
            // are owned by the edit-session hooks, never by the worker or completion callback.
            const std::weak_ptr<VoiceDeliveryRegistry> weak = registry;
            auto completionWork = work;
            completionWork.text.clear();
            auto task = std::make_shared<VoiceFinal::Task>(
                owner->id, owner->focus, owner->generation, work.revision, work.deadline, work.operation,
                std::move(work.text), [weak, completionWork](const VoiceFinal::Task &, AckResult result) {
                    if (auto registry = weak.lock())
                        registry->Complete(completionWork, result);
                });
            binding->active = task;
            binding->activeOperation = work.operation;
            binding->activeRevision = work.revision;
            HRESULT sessionResult = E_FAIL;
            if (isPreview)
            {
                VoicePreview::PreviewExecutionHooks hooks;
                hooks.validateIdentity = validate;
                hooks.startedCompositionHolder = binding->started;
                hooks.adoptComposition = [this, binding, owner, basicCurrent](TfEditCookie, ITfContext *context,
                                                                              ITfComposition *composition) {
                    if (!basicCurrent() || owner->cancelled || owner->failed || _pComposition ||
                        _CaptureCompositionEpoch() != owner->initialEpoch || _pContext)
                        return false;
                    // The IME and UI binding each own a reference; the session/holder own theirs.
                    composition->AddRef();
                    _SetComposition(composition);
                    _SaveCompositionContext(context);
                    composition->AddRef();
                    binding->composition = composition;
                    binding->epoch = _CaptureCompositionEpoch();
                    _voiceCompositionActive = true;
                    return basicCurrent() && !owner->cancelled && !owner->failed;
                };
                VoicePreview::RequestVoicePreviewEditSession(
                    _tfClientId, static_cast<ITfTextInputProcessorEx *>(this), binding->context, binding->composition,
                    task, std::move(hooks), TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult);
            }
            else
            {
                VoiceFinal::ExecutionHooks hooks;
                hooks.validateIdentity = validate;
                struct Stats
                {
                    bool prior = false, captured = false;
                    MsimeStats::CharClassCounts counts{};
                };
                auto stats = std::make_shared<Stats>();
                hooks.beforeEndComposition = [this, binding, stats](TfEditCookie ec) {
                    stats->prior = _compositionStatsCaptured.load();
                    stats->captured = _CaptureCompositionStats(ec, binding->composition, &stats->counts);
                    _ClearCompositionDisplayAttributes(ec, binding->context, binding->composition);
                };
                hooks.afterEndComposition = [this, binding, stats, detach](TfEditCookie ec, HRESULT result) {
                    if (result == S_OK && stats->captured)
                        MsimeStats::QueueStatisticsEvent(stats->counts);
                    if (result != S_OK && !stats->prior && _pComposition == binding->composition &&
                        _CaptureCompositionEpoch() == binding->epoch)
                        _compositionStatsCaptured.store(false);
                    detach(ec, binding->composition, result);
                };
                VoiceFinal::RequestVoiceFinalEditSession(_tfClientId, static_cast<ITfTextInputProcessorEx *>(this),
                                                         binding->context, binding->composition, task, std::move(hooks),
                                                         TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult);
            }
        }
        if (!binding->active && (owner->cancelled || owner->failed) && !owner->finalUnknown && !owner->finalApplying &&
            !binding->cleanupRequested && !binding->ended)
        {
            auto composition = binding->composition ? binding->composition : binding->started->Get();
            if (composition && binding->context)
            {
                binding->cleanupRequested = true;
                VoiceWork work{owner, Operation::Cancel, 0, GetTickCount64() + 1500};
                const std::weak_ptr<VoiceDeliveryRegistry> weak = registry;
                auto task = std::make_shared<VoiceFinal::Task>(
                    owner->id, owner->focus, owner->generation, 0, work.deadline, Operation::Cancel, std::u16string{},
                    [weak, work](const VoiceFinal::Task &, AckResult result) {
                        if (auto registry = weak.lock())
                            registry->Complete(work, result);
                    });
                binding->active = task;
                binding->activeOperation = Operation::Cancel;
                binding->activeRevision = 0;
                VoicePreview::PreviewExecutionHooks hooks;
                hooks.validateIdentity = exactCleanup;
                hooks.canCancel = exactCleanup;
                hooks.afterEndComposition = detach;
                HRESULT result = E_FAIL;
                VoicePreview::RequestVoicePreviewCancelEditSession(
                    _tfClientId, static_cast<ITfTextInputProcessorEx *>(this), binding->context, composition, task,
                    std::move(hooks), TF_ES_ASYNC | TF_ES_READWRITE, &result);
            }
        }
        if (!binding->active && binding->pending.empty() &&
            (binding->ended || binding->cleanupRequested ||
             (owner->finalUnknown && _pComposition != binding->composition) ||
             ((owner->cancelled || owner->failed) && !binding->composition && !binding->started->Get())))
            ui->bindings.erase(std::make_pair(owner->generation, owner->id));
    }
}
