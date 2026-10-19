// Reuse SDK vtable mocks; the original test entry point is not run here.
#define main unused_strict_session_tests
#include "voice_final_edit_session.cpp"
#undef main
#include "Edit/VoicePreviewEditSession.h"
#include "IPC/VoiceDeliveryRegistry.h"
#include <deque>
#include <map>
using namespace FanyImeVoiceFinal;
using VoiceWork = VoiceDeliveryRegistry::Work;
#include "voice_reset_state.h"

namespace Global
{
bool g_connected = false;
}
namespace GlobalIme
{
std::wstring word_for_creating_word, pending_create_word_preedit;
}
namespace MsimeStats
{
struct CharClassCounts
{
};
void QueueStatisticsEvent(const CharClassCounts &)
{
}
} // namespace MsimeStats
std::wstring g_toggleImeFallbackBuffer;
constexpr int CATEGORY_COMPOSING = 1, FUNCTION_CANCEL = 2;
constexpr UINT TIMER_CONNECT_ALL_NAMEDPIPE = 1, WM_IpcReconnect = WM_APP + 1;
constexpr int CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS = 20;
struct _KEYSTROKE_STATE
{
    int Category = 0, Function = 0;
};
HRESULT SafeRangeSetText(ITfRange *r, TfEditCookie ec, DWORD flags, const WCHAR *text, LONG size)
{
    return r->SetText(ec, flags, text, size);
}
HRESULT SafeEndComposition(ITfComposition *c, TfEditCookie ec)
{
    return c->EndComposition(ec);
}

// Only unrelated engine/transport/UI dependencies are doubles. Reset, cancellation,
// ownership guard, termination, strict final write and preview cancellation are production code.
class CMetasequoiaIME
{
  public:
    std::shared_ptr<VoiceDeliveryUiState> _voiceDeliveryUi = std::make_shared<VoiceDeliveryUiState>();
    std::atomic<UINT> _localSessionResetToken{1};
    std::atomic<bool> _localSessionResetPending{true}, _workerCommitReady{true};
    std::atomic<std::uint64_t> _compositionEpoch{0x3333};
    UINT _localResyncResetToken = 0, _queuedLocalResetToken = 0;
    bool _localResetEditSessionQueued = false, _voiceCompositionActive = true;
    ITfComposition *_pComposition = nullptr;
    ITfContext *_pContext = nullptr;
    ITfThreadMgr *_pThreadMgr = nullptr;
    void *_pCandidateListUIPresenter = nullptr;
    HWND _msgWndHandle = nullptr;
    UINT _ipcReconnectDelayMs = 0;
    int resetDispatches = 0;
    bool _ShouldPreserveVoiceComposition(ITfComposition *) const;
    void _RequestLocalSessionReset(ITfContext *, UINT);
    HRESULT _HandleCancel(TfEditCookie, ITfContext *);
    HRESULT _RemoveDummyCompositionForComposing(TfEditCookie, ITfComposition *);
    void _TerminateComposition(TfEditCookie, ITfContext *, BOOL = FALSE, HRESULT * = nullptr);
    std::uint64_t _CaptureCompositionEpoch() const
    {
        return _compositionEpoch.load();
    }
    bool _IsComposing() const
    {
        return _pComposition != nullptr;
    }
    void _ClearPendingIpcRequests()
    {
        for (auto &entry : _voiceDeliveryUi->bindings)
            entry.second->owner->cancelled = true;
    }
    void _OnLocalSessionResetReleased(UINT)
    {
    }
    void _ScheduleDeferredKeyDownDrain()
    {
    }
    void _DeleteCandidateList(BOOL, ITfContext *)
    {
    }
    void _ClearCompositionDisplayAttributes(TfEditCookie, ITfContext *, ITfComposition *)
    {
    }
    bool _CaptureCompositionStats(TfEditCookie, ITfComposition *, MsimeStats::CharClassCounts *)
    {
        return false;
    }
    HRESULT _InvokeKeyHandler(ITfContext *context, UINT, WCHAR, DWORD, _KEYSTROKE_STATE state, int,
                              std::initializer_list<int>, UINT)
    {
        ++resetDispatches;
        return state.Function == FUNCTION_CANCEL ? _HandleCancel(0x51, context) : E_FAIL;
    }
};
#include "voice_reset_source.h"

bool TestUnknownAndApplyingSurviveTransportReset()
{
    for (int mode = 0; mode != 3; ++mode)
    {
        Fixture fixture;
        CMetasequoiaIME ime;
        auto binding = std::make_shared<VoiceDeliveryUiState::Binding>();
        Frame frame;
        frame.deliveryId = 10;
        frame.focusToken = fixture.focusToken;
        binding->owner = std::make_shared<VoiceDeliveryRegistry::Owner>(fixture.generation, frame, fixture.epoch,
                                                                        VoiceDeliveryRegistry::Sender{});
        binding->epoch = fixture.epoch;
        binding->composition = &fixture.composition;
        fixture.composition.AddRef();
        binding->context = &fixture.context;
        fixture.context.AddRef();
        ime._pComposition = &fixture.composition;
        fixture.composition.AddRef();
        ime._pContext = &fixture.context;
        fixture.context.AddRef();
        ime._voiceDeliveryUi->bindings[{fixture.generation, 10}] = binding;
        VoiceDeliveryRegistry registry([] { return true; });
        VoiceWork work{binding->owner, Operation::CommitComposition};
        auto task = std::make_shared<Task>(10, fixture.focusToken, fixture.generation, fixture.epoch, 1000,
                                           Operation::CommitComposition, u"keep this complete transcript",
                                           [&](const Task &, AckResult result) { registry.Complete(work, result); });
        binding->active = task;
        binding->activeOperation = Operation::CommitComposition;
        if (mode == 0)
            fixture.context.setSelectionResult = E_FAIL;
        if (mode == 1)
            fixture.composition.endCompositionResult = E_FAIL;
        if (mode == 2)
            fixture.range.onSetText = [&] { ime._RequestLocalSessionReset(nullptr, 1); };
        HRESULT result;
        auto hooks = fixture.Hooks();
        hooks.validateIdentity = [&](const Task &, ITfContext *, ITfComposition *composition) {
            return ime._pComposition == composition && ime._CaptureCompositionEpoch() == binding->epoch &&
                   !binding->owner->cancelled;
        };
        hooks.beforeEndComposition = [](TfEditCookie) {};
        CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition,
                                                       task, hooks, TF_ES_ASYNC | TF_ES_READWRITE, &result) == S_OK);
        if (mode != 2)
        {
            CHECK(binding->owner->finalUnknown);
            ime._RequestLocalSessionReset(nullptr, 1);
        }
        CHECK(ime.resetDispatches == 1);
        CHECK(fixture.range.setTextCalls == 1);
        CHECK(fixture.range.lastSetText == u"keep this complete transcript");
        CHECK(ime._pComposition == nullptr);
    }
    return true;
}

bool TestUnrelatedCompositionStillCancels()
{
    for (bool staleVoiceOwner : {false, true})
    {
        Fixture fixture;
        CMetasequoiaIME ime;
        ime._pComposition = &fixture.composition;
        fixture.composition.AddRef();
        ime._pContext = &fixture.context;
        fixture.context.AddRef();
        if (staleVoiceOwner)
        {
            Frame frame;
            frame.deliveryId = 12;
            frame.focusToken = fixture.focusToken;
            auto binding = std::make_shared<VoiceDeliveryUiState::Binding>();
            binding->owner = std::make_shared<VoiceDeliveryRegistry::Owner>(fixture.generation, frame, fixture.epoch,
                                                                            VoiceDeliveryRegistry::Sender{});
            binding->owner->finalUnknown = true;
            binding->epoch = fixture.epoch - 1;
            binding->composition = &fixture.composition;
            fixture.composition.AddRef();
            ime._voiceDeliveryUi->bindings[{fixture.generation, 12}] = binding;
        }
        ime._RequestLocalSessionReset(nullptr, 1);
        CHECK(fixture.range.setTextCalls == 1 && fixture.range.lastSetText.empty());
        CHECK(fixture.composition.endCompositionCalls == 1);
    }
    return true;
}

bool TestCancelledCleanupSurvivesRepeatedDrain()
{
    Fixture fixture;
    Frame frame;
    frame.deliveryId = 11;
    frame.focusToken = fixture.focusToken;
    VoiceDeliveryUiState::Binding binding;
    binding.owner = std::make_shared<VoiceDeliveryRegistry::Owner>(fixture.generation, frame, fixture.epoch,
                                                                   VoiceDeliveryRegistry::Sender{});
    binding.owner->cancelled = true;
    auto ack = std::make_shared<AckState>();
    binding.active = MakeTask({}, Operation::Cancel, ack);
    binding.activeOperation = Operation::Cancel;
    fixture.context.mode = MockContext::RequestMode::Async;
    VoicePreview::PreviewExecutionHooks hooks;
    hooks.now = [&] { return fixture.now; };
    hooks.validateIdentity = [](const Task &, ITfContext *, ITfComposition *) { return true; };
    hooks.canCancel = hooks.validateIdentity;
    HRESULT result;
    CHECK(VoicePreview::RequestVoicePreviewCancelEditSession(0x77, &fixture.owner, &fixture.context,
                                                             &fixture.composition, binding.active, hooks,
                                                             TF_ES_ASYNC | TF_ES_READWRITE, &result) == S_OK);
    binding.CancelSupersededTask();
    binding.CancelSupersededTask();
    CHECK(fixture.context.RunQueued() == S_OK);
    CHECK(fixture.range.setTextCalls == 1 && fixture.range.lastSetText.empty());
    CHECK(fixture.composition.endCompositionCalls == 1);
    return true;
}

int main()
{
    if (!TestUnknownAndApplyingSurviveTransportReset() || !TestCancelledCleanupSurvivesRepeatedDrain() ||
        !TestUnrelatedCompositionStillCancels())
        return 1;
    std::puts("voice_reset_integration: three final/reset interleavings, queued cleanup and two ordinary/stale reset "
              "cases passed");
    return 0;
}
