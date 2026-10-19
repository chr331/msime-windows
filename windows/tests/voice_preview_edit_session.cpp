#include "Edit/VoicePreviewEditSession.h"

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using VoiceFinal::Task;
using VoicePreview::PreviewExecutionHooks;
namespace Contract = FanyImeVoiceFinal;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::printf("CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);                                \
            return false;                                                                                              \
        }                                                                                                              \
    } while (false)

class MockOwner final : public ITfCompositionSink
{
  public:
    ULONG RefCount() const noexcept
    {
        return refs_.load();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr)
            return E_INVALIDARG;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown))
        {
            *object = static_cast<IUnknown *>(this);
        }
        else if (IsEqualIID(riid, IID_ITfCompositionSink))
        {
            if (sinkDisabled)
                return E_NOINTERFACE;
            *object = static_cast<ITfCompositionSink *>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return ++refs_;
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP OnCompositionTerminated(TfEditCookie, ITfComposition *) override
    {
        ++terminatedCalls;
        return S_OK;
    }

    bool sinkDisabled = false;
    int terminatedCalls = 0;

  private:
    std::atomic<ULONG> refs_{1};
};

class MockRange final : public ITfRange
{
  public:
    ULONG RefCount() const noexcept
    {
        return refs_.load();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr)
            return E_INVALIDARG;
        *object = nullptr;
        if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_ITfRange))
            return E_NOINTERFACE;
        *object = static_cast<ITfRange *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return ++refs_;
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP GetText(TfEditCookie, DWORD, WCHAR *, ULONG, ULONG *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP SetText(TfEditCookie, DWORD, const WCHAR *text, LONG count) override
    {
        ++setTextCalls;
        lastTextPointerNull = text == nullptr;
        lastTextCount = count;
        lastSetText.clear();
        if (text != nullptr && count > 0)
            lastSetText.assign(reinterpret_cast<const char16_t *>(text), static_cast<std::size_t>(count));
        if (onSetText)
            onSetText();
        return setTextResult;
    }

    STDMETHODIMP GetFormattedText(TfEditCookie, IDataObject **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetEmbedded(TfEditCookie, REFGUID, REFIID, IUnknown **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP InsertEmbedded(TfEditCookie, DWORD, IDataObject *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftStart(TfEditCookie, LONG, LONG *, const TF_HALTCOND *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftEnd(TfEditCookie, LONG, LONG *, const TF_HALTCOND *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftStartToRange(TfEditCookie, ITfRange *, TfAnchor) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftEndToRange(TfEditCookie, ITfRange *, TfAnchor) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftStartRegion(TfEditCookie, TfShiftDir, BOOL *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftEndRegion(TfEditCookie, TfShiftDir, BOOL *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP IsEmpty(TfEditCookie, BOOL *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP Collapse(TfEditCookie, TfAnchor) override
    {
        ++collapseCalls;
        return collapseResult;
    }

    STDMETHODIMP IsEqualStart(TfEditCookie, ITfRange *, TfAnchor, BOOL *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP IsEqualEnd(TfEditCookie, ITfRange *, TfAnchor, BOOL *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP CompareStart(TfEditCookie, ITfRange *, TfAnchor, LONG *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP CompareEnd(TfEditCookie, ITfRange *, TfAnchor, LONG *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP AdjustForInsert(TfEditCookie, ULONG, BOOL *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetGravity(TfGravity *, TfGravity *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetGravity(TfEditCookie, TfGravity, TfGravity) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Clone(ITfRange **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetContext(ITfContext **) override
    {
        return E_NOTIMPL;
    }

    int setTextCalls = 0;
    int collapseCalls = 0;
    LONG lastTextCount = -1;
    bool lastTextPointerNull = false;
    HRESULT setTextResult = S_OK;
    HRESULT collapseResult = S_OK;
    std::u16string lastSetText;
    std::function<void()> onSetText;

  private:
    std::atomic<ULONG> refs_{1};
};

class MockComposition final : public ITfComposition
{
  public:
    ULONG RefCount() const noexcept
    {
        return refs_.load();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr)
            return E_INVALIDARG;
        *object = nullptr;
        if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_ITfComposition))
            return E_NOINTERFACE;
        *object = static_cast<ITfComposition *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return ++refs_;
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP GetRange(ITfRange **range) override
    {
        ++getRangeCalls;
        if (range == nullptr)
            return E_INVALIDARG;
        *range = nullptr;
        if (getRangeResult != S_OK || range_ == nullptr)
            return getRangeResult;
        range_->AddRef();
        *range = range_;
        if (onGetRange)
            onGetRange();
        return S_OK;
    }

    STDMETHODIMP ShiftStart(TfEditCookie, ITfRange *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP ShiftEnd(TfEditCookie, ITfRange *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP EndComposition(TfEditCookie) override
    {
        ++endCompositionCalls;
        const HRESULT result = endCompositionResult;
        if (onEndComposition)
            onEndComposition(result);
        return result;
    }

    MockRange *range_ = nullptr;
    HRESULT getRangeResult = S_OK;
    HRESULT endCompositionResult = S_OK;
    int getRangeCalls = 0;
    int endCompositionCalls = 0;
    std::function<void()> onGetRange;
    std::function<void(HRESULT)> onEndComposition;

  private:
    std::atomic<ULONG> refs_{1};
};

class MockContext final : public ITfContext, public ITfContextComposition
{
  public:
    enum class RequestMode
    {
        Immediate,
        Async,
        Reject,
    };

    ULONG RefCount() const noexcept
    {
        return refs_.load();
    }
    bool HasQueuedSession() const noexcept
    {
        return queued_ != nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **object) override
    {
        if (object == nullptr)
            return E_INVALIDARG;
        *object = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfContext))
        {
            *object = static_cast<ITfContext *>(this);
        }
        else if (IsEqualIID(riid, IID_ITfContextComposition))
        {
            if (contextCompositionDisabled)
                return E_NOINTERFACE;
            *object = static_cast<ITfContextComposition *>(this);
            if (onContextCompositionQuery)
                onContextCompositionQuery();
        }
        else
        {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return ++refs_;
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = --refs_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    STDMETHODIMP RequestEditSession(TfClientId, ITfEditSession *session, DWORD, HRESULT *sessionResult) override
    {
        ++requestCalls;
        if (sessionResult == nullptr)
            return E_INVALIDARG;
        if (mode == RequestMode::Reject)
        {
            *sessionResult = rejectedSessionResult;
            return rejectedOuterResult;
        }
        if (mode == RequestMode::Async)
        {
            queued_ = session;
            if (queued_)
                queued_->AddRef();
            *sessionResult = TF_S_ASYNC;
            return S_OK;
        }
        *sessionResult = session ? session->DoEditSession(cookie) : E_POINTER;
        return S_OK;
    }

    STDMETHODIMP InWriteSession(TfClientId, BOOL *) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP GetSelection(TfEditCookie, ULONG, ULONG, TF_SELECTION *selection, ULONG *fetched) override
    {
        ++getSelectionCalls;
        if (selection == nullptr || fetched == nullptr)
            return E_INVALIDARG;
        *fetched = 0;
        selection->range = nullptr;
        selection->style.ase = TF_AE_NONE;
        selection->style.fInterimChar = FALSE;
        if (selectionRange)
        {
            selectionRange->AddRef();
            selection->range = selectionRange;
        }
        if (getSelectionResult == S_OK)
            *fetched = fetchedCount;
        if (onGetSelection)
            onGetSelection();
        return getSelectionResult;
    }

    STDMETHODIMP SetSelection(TfEditCookie, ULONG count, const TF_SELECTION *selection) override
    {
        ++setSelectionCalls;
        lastSelectionCount = count;
        if (selection != nullptr)
            lastSelectionRange = selection[0].range;
        return setSelectionResult;
    }

    STDMETHODIMP GetStart(TfEditCookie, ITfRange **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetEnd(TfEditCookie, ITfRange **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetActiveView(ITfContextView **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP EnumViews(IEnumTfContextViews **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetStatus(TF_STATUS *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetProperty(REFGUID, ITfProperty **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetAppProperty(REFGUID, ITfReadOnlyProperty **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP TrackProperties(const GUID **, ULONG, const GUID **, ULONG, ITfReadOnlyProperty **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP EnumProperties(IEnumTfProperties **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP GetDocumentMgr(ITfDocumentMgr **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP CreateRangeBackup(TfEditCookie, ITfRange *, ITfRangeBackup **) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP StartComposition(TfEditCookie, ITfRange *range, ITfCompositionSink *sink,
                                  ITfComposition **composition) override
    {
        ++startCalls;
        startRange = range;
        startSink = sink;
        if (events)
            events->push_back("start");
        if (composition == nullptr)
            return E_INVALIDARG;
        *composition = nullptr;
        if (startResult != S_OK || startedComposition == nullptr)
            return startResult;
        startedComposition->AddRef();
        *composition = startedComposition;
        return S_OK;
    }

    STDMETHODIMP EnumCompositions(IEnumITfCompositionView **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP FindComposition(TfEditCookie, ITfRange *, IEnumITfCompositionView **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP TakeOwnership(TfEditCookie, ITfCompositionView *, ITfCompositionSink *, ITfComposition **) override
    {
        return E_NOTIMPL;
    }

    HRESULT RunQueued()
    {
        ITfEditSession *session = queued_;
        queued_ = nullptr;
        if (session == nullptr)
            return S_FALSE;
        const HRESULT hr = session->DoEditSession(cookie);
        session->Release();
        return hr;
    }

    void DropQueued()
    {
        ITfEditSession *session = queued_;
        queued_ = nullptr;
        if (session)
            session->Release();
    }

    RequestMode mode = RequestMode::Immediate;
    HRESULT rejectedOuterResult = E_FAIL;
    HRESULT rejectedSessionResult = E_FAIL;
    TfEditCookie cookie = 0x51;
    MockRange *selectionRange = nullptr;
    HRESULT getSelectionResult = S_OK;
    ULONG fetchedCount = 1;
    HRESULT setSelectionResult = S_OK;
    int requestCalls = 0;
    int getSelectionCalls = 0;
    int setSelectionCalls = 0;
    ULONG lastSelectionCount = 0;
    ITfRange *lastSelectionRange = nullptr;
    std::function<void()> onGetSelection;

    bool contextCompositionDisabled = false;
    std::function<void()> onContextCompositionQuery;
    HRESULT startResult = S_OK;
    MockComposition *startedComposition = nullptr;
    int startCalls = 0;
    ITfRange *startRange = nullptr;
    ITfCompositionSink *startSink = nullptr;
    std::vector<std::string> *events = nullptr;

  private:
    ITfEditSession *queued_ = nullptr;
    std::atomic<ULONG> refs_{1};
};

struct CompletionState
{
    int count = 0;
    std::vector<Contract::AckResult> results;
    std::uint64_t deliveryId = 0;
};

struct Fixture
{
    MockOwner owner;
    MockContext context;
    MockRange selectionRange;
    MockRange compositionRange;
    MockComposition existingComposition;
    MockComposition startedComposition;
    std::vector<std::string> events;
    std::shared_ptr<VoicePreview::StartedCompositionHolder> startedHolder =
        std::make_shared<VoicePreview::StartedCompositionHolder>();
    std::uint64_t now = 100;
    std::uint64_t focusToken = 0x1111;
    std::uint64_t generation = 0x2222;
    std::uint64_t epoch = 0x3333;
    std::uint64_t boundEpoch = 0x3333;
    ITfComposition *boundComposition = nullptr;
    ITfContext *activeContext = nullptr;
    bool identityValid = true;
    bool adoptAllowed = true;
    bool adoptThrows = false;
    bool afterThrows = false;
    bool cancelAllowed = true;
    int adoptCalls = 0;
    int afterCalls = 0;
    int validateCalls = 0;
    int advanceDeadlineOnValidateCall = 0;
    int canCancelCalls = 0;
    int afterEndCalls = 0;
    HRESULT lastAfterEndResult = E_FAIL;

    Fixture()
    {
        context.selectionRange = &selectionRange;
        context.startedComposition = &startedComposition;
        context.events = &events;
        existingComposition.range_ = &compositionRange;
        startedComposition.range_ = &compositionRange;
        compositionRange.onSetText = [this]() { events.push_back("settext"); };
        activeContext = &context;
    }

    void UseExistingComposition()
    {
        boundComposition = &existingComposition;
        boundEpoch = epoch;
    }

    PreviewExecutionHooks Hooks()
    {
        PreviewExecutionHooks hooks;
        hooks.startedCompositionHolder = startedHolder;
        hooks.now = [this]() { return now; };
        hooks.validateIdentity = [this](const Task &task, ITfContext *context, ITfComposition *composition) {
            ++validateCalls;
            if (advanceDeadlineOnValidateCall != 0 && validateCalls == advanceDeadlineOnValidateCall)
                now = task.DeadlineTick();
            if (!identityValid || activeContext != context || focusToken != task.FocusToken() ||
                generation != task.WorkerGeneration())
                return false;
            return composition == boundComposition && epoch == boundEpoch;
        };
        hooks.adoptComposition = [this](TfEditCookie, ITfContext *, ITfComposition *composition) {
            ++adoptCalls;
            events.push_back("adopt");
            if (adoptThrows)
                throw std::runtime_error("adopt");
            if (!adoptAllowed)
                return false;
            boundComposition = composition;
            boundEpoch = epoch;
            return true;
        };
        hooks.afterPreview = [this](TfEditCookie, ITfContext *, ITfComposition *) {
            ++afterCalls;
            events.push_back("after");
            if (afterThrows)
                throw std::runtime_error("after");
        };
        hooks.canCancel = [this](const Task &, ITfContext *context, ITfComposition *composition) {
            ++canCancelCalls;
            return cancelAllowed && identityValid && activeContext == context && composition == boundComposition &&
                   epoch == boundEpoch;
        };
        hooks.afterEndComposition = [this](TfEditCookie, ITfComposition *, HRESULT result) {
            ++afterEndCalls;
            lastAfterEndResult = result;
            events.push_back("afterend");
        };
        return hooks;
    }
};

std::shared_ptr<Task> MakeTask(const std::u16string &text, Contract::Operation operation,
                               const std::shared_ptr<CompletionState> &completion, std::uint64_t deadline = 1000,
                               std::uint64_t focusToken = 0x1111, std::uint64_t generation = 0x2222,
                               std::uint64_t epoch = 0x3333, std::uint64_t deliveryId = 0xAABBCCDDu)
{
    return std::make_shared<Task>(deliveryId, focusToken, generation, epoch, deadline, operation, text,
                                  [completion](const Task &task, Contract::AckResult result) {
                                      ++completion->count;
                                      completion->results.push_back(result);
                                      completion->deliveryId = task.DeliveryId();
                                  });
}

HRESULT RequestPreview(Fixture &fixture, const std::shared_ptr<Task> &task, HRESULT *sessionResult,
                       ITfComposition *expected = nullptr)
{
    return VoicePreview::RequestVoicePreviewEditSession(0x77, &fixture.owner, &fixture.context, expected, task,
                                                        fixture.Hooks(), TF_ES_ASYNC | TF_ES_READWRITE, sessionResult);
}

HRESULT RequestCancel(Fixture &fixture, const std::shared_ptr<Task> &task, HRESULT *sessionResult,
                      ITfComposition *expected)
{
    return VoicePreview::RequestVoicePreviewCancelEditSession(0x77, &fixture.owner, &fixture.context, expected, task,
                                                              fixture.Hooks(), TF_ES_ASYNC | TF_ES_READWRITE,
                                                              sessionResult);
}

bool TestFirstPreviewStartsAdoptsWritesAndKeepsComposition()
{
    Fixture fixture;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"hello preview", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.context.requestCalls == 1);
    CHECK(fixture.context.startCalls == 1);
    CHECK(fixture.context.startRange == &fixture.selectionRange);
    CHECK(fixture.context.startSink != nullptr);
    CHECK(fixture.adoptCalls == 1);
    CHECK(fixture.boundComposition == &fixture.startedComposition);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 1);
    CHECK(fixture.compositionRange.lastSetText == u"hello preview");
    CHECK(fixture.compositionRange.lastTextCount == 13);
    CHECK(fixture.compositionRange.collapseCalls == 1);
    CHECK(fixture.context.setSelectionCalls == 1);
    CHECK(fixture.context.lastSelectionRange == &fixture.compositionRange);
    CHECK(fixture.afterCalls == 1);
    CHECK(fixture.startedComposition.endCompositionCalls == 0);
    CHECK(fixture.events.size() == 4);
    CHECK(fixture.events[0] == "start");
    CHECK(fixture.events[1] == "adopt");
    CHECK(fixture.events[2] == "settext");
    CHECK(fixture.events[3] == "after");
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);
    CHECK(task->CurrentState() == Task::State::Finished);
    CHECK(!task->Finish(Contract::AckResult::UnknownOrPartial));
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.selectionRange.RefCount() == 1);
    CHECK(fixture.compositionRange.RefCount() == 1);
    CHECK(fixture.startedComposition.RefCount() == 2);
    CHECK(fixture.startedHolder->Get() == &fixture.startedComposition);
    return true;
}

bool TestLongPreviewTextPreservesFullUtf16()
{
    std::vector<std::u16string> texts;
    texts.emplace_back(200, u'a');
    std::u16string longText(2046, u'b');
    longText.push_back(u'\0');
    longText.push_back(static_cast<char16_t>(0xD83D));
    longText.push_back(static_cast<char16_t>(0xDE00));
    texts.push_back(longText);

    for (const std::u16string &text : texts)
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(text, Contract::Operation::Preview, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
        CHECK(sessionResult == S_OK);
        CHECK(fixture.compositionRange.setTextCalls == 1);
        CHECK(fixture.compositionRange.lastTextCount == static_cast<LONG>(text.size()));
        CHECK(fixture.compositionRange.lastSetText.size() == text.size());
        CHECK(fixture.compositionRange.lastSetText == text);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::Applied);
    }
    return true;
}

bool TestUpdateWritesExpectedCompositionWithoutStarting()
{
    Fixture fixture;
    fixture.UseExistingComposition();
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"updated preview", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.adoptCalls == 0);
    CHECK(fixture.existingComposition.getRangeCalls == 1);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 1);
    CHECK(fixture.compositionRange.lastSetText == u"updated preview");
    CHECK(fixture.compositionRange.collapseCalls == 1);
    CHECK(fixture.context.lastSelectionRange == &fixture.compositionRange);
    CHECK(fixture.existingComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.existingComposition.RefCount() == 1);
    CHECK(fixture.compositionRange.RefCount() == 1);
    return true;
}

bool TestAsyncAcceptanceDoesNotWriteUntilRun()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"queued", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(fixture.context.HasQueuedSession());
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(completion->count == 0);
    CHECK(fixture.owner.RefCount() == 2);
    CHECK(fixture.context.RefCount() == 2);
    CHECK(fixture.context.RunQueued() == S_OK);
    CHECK(fixture.context.startCalls == 1);
    CHECK(fixture.compositionRange.setTextCalls == 1);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    return true;
}

bool TestDroppedQueuedPreviewFinishesRejected()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"dropped", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    fixture.context.DropQueued();
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    CHECK(task->CurrentState() == Task::State::Finished);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    return true;
}

bool TestCancelBeforeRunPreventsWrite()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"cancelled", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(task->Cancel());
    CHECK(fixture.context.RunQueued() == S_FALSE);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool RunQueuedPreviewFenceCase(int field, bool update)
{
    Fixture fixture;
    if (update)
        fixture.UseExistingComposition();
    fixture.context.mode = MockContext::RequestMode::Async;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"fenced", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    ITfComposition *expected = update ? &fixture.existingComposition : nullptr;
    CHECK(RequestPreview(fixture, task, &sessionResult, expected) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    if (field == 0)
        fixture.activeContext = nullptr;
    else if (field == 1)
        ++fixture.focusToken;
    else if (field == 2)
        ++fixture.generation;
    else if (field == 3)
        ++fixture.epoch;
    else
        fixture.now = 1000;
    CHECK(fixture.context.RunQueued() == S_FALSE);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.startedComposition.endCompositionCalls == 0);
    CHECK(fixture.existingComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestQueuedIdentityAndDeadlineFences()
{
    for (int field = 0; field < 5; ++field)
    {
        if (!RunQueuedPreviewFenceCase(field, false))
            return false;
    }
    for (int field = 0; field < 5; ++field)
    {
        if (!RunQueuedPreviewFenceCase(field, true))
            return false;
    }
    return true;
}

bool TestSelectionAndStartFailuresRejectBeforeWrite()
{
    for (int kind = 0; kind < 6; ++kind)
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"no-write", Contract::Operation::Preview, completion);
        if (kind == 0)
            fixture.context.getSelectionResult = E_FAIL;
        else if (kind == 1)
            fixture.context.fetchedCount = 0;
        else if (kind == 2)
            fixture.context.selectionRange = nullptr;
        else if (kind == 3)
            fixture.context.startResult = E_FAIL;
        else if (kind == 4)
            fixture.context.contextCompositionDisabled = true;
        else
            fixture.owner.sinkDisabled = true;

        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.adoptCalls == 0);
        CHECK(fixture.selectionRange.setTextCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
        CHECK(fixture.owner.RefCount() == 1);
        CHECK(fixture.context.RefCount() == 1);
        CHECK(fixture.selectionRange.RefCount() == 1);
        CHECK(fixture.startedComposition.RefCount() == 1);
    }
    return true;
}

bool TestAdoptRejectionOrThrowIsUnknownWithoutEnd()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        if (kind == 0)
            fixture.adoptAllowed = false;
        else
            fixture.adoptThrows = true;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"adopt-failure", Contract::Operation::Preview, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.context.startCalls == 1);
        CHECK(fixture.adoptCalls == 1);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(fixture.selectionRange.setTextCalls == 0);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
        // The preallocated holder received the exact object before adopt failed and keeps a
        // strong reference after the preview session has returned.
        CHECK(fixture.startedHolder->Get() == &fixture.startedComposition);
        CHECK(fixture.startedComposition.RefCount() == 2);

        // A dedicated targeted cancel uses that exact composition and ends it exactly once.
        fixture.boundComposition = fixture.startedHolder->Get();
        fixture.boundEpoch = fixture.epoch;
        auto cancelCompletion = std::make_shared<CompletionState>();
        auto cancelTask = MakeTask(u"", Contract::Operation::Cancel, cancelCompletion, 2000);
        HRESULT cancelResult = E_FAIL;
        CHECK(RequestCancel(fixture, cancelTask, &cancelResult, fixture.startedHolder->Get()) == S_OK);
        CHECK(cancelResult == S_OK);
        CHECK(fixture.compositionRange.setTextCalls == 1);
        CHECK(fixture.compositionRange.lastTextPointerNull);
        CHECK(fixture.startedComposition.endCompositionCalls == 1);
        CHECK(cancelCompletion->count == 1);
        CHECK(cancelCompletion->results[0] == Contract::AckResult::Applied);
        CHECK(fixture.startedHolder->Get() == &fixture.startedComposition);
        CHECK(fixture.startedComposition.RefCount() == 2);
    }
    return true;
}

bool TestCompositionRangeFailureAfterStartIsUnknown()
{
    Fixture fixture;
    fixture.startedComposition.getRangeResult = E_FAIL;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"no-range", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(fixture.context.startCalls == 1);
    CHECK(fixture.adoptCalls == 1);
    CHECK(fixture.startedComposition.getRangeCalls == 1);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.startedComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
    return true;
}

bool TestUpdateRangeFailureRejectsBeforeWrite()
{
    Fixture fixture;
    fixture.UseExistingComposition();
    fixture.existingComposition.getRangeResult = E_FAIL;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"update-no-range", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
    CHECK(fixture.existingComposition.getRangeCalls == 1);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.existingComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestSetTextAndPostWriteFailuresAreUnknownAndNotRetried()
{
    for (int kind = 0; kind < 5; ++kind)
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"write-failure", Contract::Operation::Preview, completion);
        if (kind == 0)
            fixture.compositionRange.setTextResult = E_FAIL;
        else if (kind == 1)
            fixture.compositionRange.setTextResult = S_FALSE;
        else if (kind == 2)
            fixture.compositionRange.collapseResult = E_FAIL;
        else if (kind == 3)
            fixture.context.setSelectionResult = S_FALSE;
        else
            fixture.afterThrows = true;

        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.compositionRange.setTextCalls == 1);
        if (kind <= 1)
        {
            CHECK(fixture.compositionRange.collapseCalls == 0);
            CHECK(fixture.context.setSelectionCalls == 0);
        }
        else
        {
            CHECK(fixture.compositionRange.collapseCalls == 1);
        }
        if (kind <= 2)
            CHECK(fixture.context.setSelectionCalls == 0);
        else
            CHECK(fixture.context.setSelectionCalls == 1);
        if (kind < 4)
            CHECK(fixture.afterCalls == 0);
        else
            CHECK(fixture.afterCalls == 1);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
        CHECK(fixture.owner.RefCount() == 1);
        CHECK(fixture.context.RefCount() == 1);
        CHECK(fixture.startedComposition.RefCount() == 2);
        CHECK(fixture.startedHolder->Get() == &fixture.startedComposition);
    }
    return true;
}

bool TestPostFetchFenceRejectsBeforeWrite()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        Fixture fixture;
        if (kind == 2)
            fixture.UseExistingComposition();
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"post-fetch", Contract::Operation::Preview, completion);
        if (kind == 0)
            fixture.context.onGetSelection = [&fixture]() { ++fixture.epoch; };
        else if (kind == 1)
            fixture.context.onGetSelection = [&fixture]() { fixture.now = 1000; };
        else
            fixture.existingComposition.onGetRange = [&fixture]() { ++fixture.boundEpoch; };

        HRESULT sessionResult = E_FAIL;
        ITfComposition *expected = kind == 2 ? &fixture.existingComposition : nullptr;
        CHECK(RequestPreview(fixture, task, &sessionResult, expected) == S_OK);
        if (kind < 2)
        {
            CHECK(fixture.context.getSelectionCalls == 1);
            CHECK(fixture.context.startCalls == 0);
        }
        else
        {
            CHECK(fixture.existingComposition.getRangeCalls == 1);
        }
        CHECK(fixture.selectionRange.setTextCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(fixture.existingComposition.endCompositionCalls == 0);
    }
    return true;
}

bool TestPreStartFenceAfterQueryInterfaceRejectsBeforeStart()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        fixture.context.onContextCompositionQuery = [&fixture, kind]() {
            if (kind == 0)
                ++fixture.epoch;
            else
                fixture.now = 1000;
        };
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"pre-start-fence", Contract::Operation::Preview, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.context.startCalls == 0);
        CHECK(fixture.selectionRange.setTextCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(fixture.adoptCalls == 0);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestAdoptReentrancyUsesUpdatedBinding()
{
    Fixture fixture;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"rebound", Contract::Operation::Preview, completion);
    PreviewExecutionHooks hooks = fixture.Hooks();
    hooks.adoptComposition = [&fixture](TfEditCookie, ITfContext *, ITfComposition *composition) {
        ++fixture.adoptCalls;
        fixture.events.push_back("adopt");
        fixture.boundComposition = composition;
        fixture.epoch = 0x4444;
        fixture.boundEpoch = 0x4444;
        return true;
    };
    HRESULT sessionResult = E_FAIL;
    CHECK(VoicePreview::RequestVoicePreviewEditSession(0x77, &fixture.owner, &fixture.context, nullptr, task, hooks,
                                                       TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.adoptCalls == 1);
    CHECK(fixture.compositionRange.setTextCalls == 1);
    CHECK(fixture.compositionRange.lastSetText == u"rebound");
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);

    Fixture changed;
    auto changedCompletion = std::make_shared<CompletionState>();
    auto changedTask = MakeTask(u"focus-changed", Contract::Operation::Preview, changedCompletion);
    PreviewExecutionHooks changedHooks = changed.Hooks();
    changedHooks.adoptComposition = [&changed](TfEditCookie, ITfContext *, ITfComposition *composition) {
        ++changed.adoptCalls;
        changed.boundComposition = composition;
        changed.boundEpoch = changed.epoch;
        ++changed.focusToken;
        return true;
    };
    sessionResult = E_FAIL;
    CHECK(VoicePreview::RequestVoicePreviewEditSession(0x77, &changed.owner, &changed.context, nullptr, changedTask,
                                                       changedHooks, TF_ES_ASYNC | TF_ES_READWRITE,
                                                       &sessionResult) == S_OK);
    CHECK(changed.adoptCalls == 1);
    CHECK(changed.compositionRange.setTextCalls == 0);
    CHECK(changed.startedComposition.endCompositionCalls == 0);
    CHECK(changedCompletion->count == 1);
    CHECK(changedCompletion->results[0] == Contract::AckResult::UnknownOrPartial);
    return true;
}

bool TestAsyncPreviewHoldsStrongReferences()
{
    for (int action = 0; action < 2; ++action)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        fixture.context.mode = MockContext::RequestMode::Async;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"referenced", Contract::Operation::Preview, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestPreview(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
        CHECK(sessionResult == TF_S_ASYNC);
        CHECK(fixture.owner.RefCount() == 2);
        CHECK(fixture.context.RefCount() == 2);
        CHECK(fixture.existingComposition.RefCount() == 2);
        if (action == 0)
        {
            CHECK(fixture.context.RunQueued() == S_OK);
            CHECK(completion->count == 1);
            CHECK(completion->results[0] == Contract::AckResult::Applied);
        }
        else
        {
            fixture.context.DropQueued();
            CHECK(completion->count == 1);
            CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
        }
        CHECK(fixture.owner.RefCount() == 1);
        CHECK(fixture.context.RefCount() == 1);
        CHECK(fixture.existingComposition.RefCount() == 1);
    }
    return true;
}

bool TestCreateRejectsInvalidPreviewShapes()
{
    for (int kind = 0; kind < 7; ++kind)
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        std::u16string text = u"shape";
        Contract::Operation operation = Contract::Operation::Preview;
        std::uint64_t deadline = 1000;
        std::uint64_t focus = 0x1111;
        std::uint64_t delivery = 0xAABBCCDDu;
        if (kind == 0)
            operation = Contract::Operation::Insert;
        else if (kind == 1)
            text.clear();
        else if (kind == 2)
        {
            text.clear();
            text.push_back(static_cast<char16_t>(0xD800));
        }
        else if (kind == 3)
            text.assign(65537, u'a');
        else if (kind == 4)
            delivery = 0;
        else if (kind == 5)
            focus = 0;
        else
            deadline = 0;

        auto task = MakeTask(text, operation, completion, deadline, focus, 0x2222, 0x3333, delivery);
        ITfEditSession *session = nullptr;
        CHECK(VoicePreview::CreateVoicePreviewEditSession(&fixture.owner, &fixture.context, nullptr, task,
                                                          fixture.Hooks(), &session) == E_INVALIDARG);
        CHECK(session == nullptr);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }

    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"null-out", Contract::Operation::Preview, completion);
        CHECK(VoicePreview::CreateVoicePreviewEditSession(&fixture.owner, &fixture.context, nullptr, task,
                                                          fixture.Hooks(), nullptr) == E_INVALIDARG);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }

    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"already-finished", Contract::Operation::Preview, completion);
        CHECK(task->Cancel());
        ITfEditSession *session = nullptr;
        CHECK(VoicePreview::CreateVoicePreviewEditSession(&fixture.owner, &fixture.context, nullptr, task,
                                                          fixture.Hooks(), &session) == S_FALSE);
        CHECK(session == nullptr);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestFirstPreviewRequiresPreallocatedHolder()
{
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"needs-holder", Contract::Operation::Preview, completion);
        PreviewExecutionHooks hooks = fixture.Hooks();
        hooks.startedCompositionHolder.reset();
        ITfEditSession *session = nullptr;
        CHECK(VoicePreview::CreateVoicePreviewEditSession(&fixture.owner, &fixture.context, nullptr, task, hooks,
                                                          &session) == E_INVALIDARG);
        CHECK(session == nullptr);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.context.startCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    {
        Fixture fixture;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"needs-holder", Contract::Operation::Preview, completion);
        PreviewExecutionHooks hooks = fixture.Hooks();
        hooks.startedCompositionHolder.reset();
        HRESULT sessionResult = E_FAIL;
        CHECK(VoicePreview::RequestVoicePreviewEditSession(0x77, &fixture.owner, &fixture.context, nullptr, task, hooks,
                                                           TF_ES_ASYNC | TF_ES_READWRITE,
                                                           &sessionResult) == E_INVALIDARG);
        CHECK(sessionResult == E_INVALIDARG);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.context.startCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestDeadlineAfterLastIdentityFenceBeforeFirstStart()
{
    Fixture fixture;
    fixture.advanceDeadlineOnValidateCall = 4;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"start-fence", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(fixture.validateCalls == 4);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.adoptCalls == 0);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.startedHolder->Get() == nullptr);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestDeadlineAfterLastIdentityFenceBeforeUpdateWrite()
{
    Fixture fixture;
    fixture.UseExistingComposition();
    fixture.advanceDeadlineOnValidateCall = 3;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"update-fence", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
    CHECK(fixture.validateCalls == 3);
    CHECK(fixture.existingComposition.getRangeCalls == 1);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.existingComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestDeadlineAfterAdoptIsUnknownAndHolderAllowsTargetedCleanup()
{
    Fixture fixture;
    fixture.advanceDeadlineOnValidateCall = 5;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"adopt-fence", Contract::Operation::Preview, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestPreview(fixture, task, &sessionResult) == S_OK);
    CHECK(fixture.validateCalls == 5);
    CHECK(fixture.context.startCalls == 1);
    CHECK(fixture.adoptCalls == 1);
    CHECK(fixture.selectionRange.setTextCalls == 0);
    CHECK(fixture.compositionRange.setTextCalls == 0);
    CHECK(fixture.startedComposition.endCompositionCalls == 0);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
    CHECK(fixture.startedHolder->Get() == &fixture.startedComposition);
    CHECK(fixture.startedComposition.RefCount() == 2);

    auto cancelCompletion = std::make_shared<CompletionState>();
    auto cancelTask = MakeTask(u"", Contract::Operation::Cancel, cancelCompletion, 2000);
    HRESULT cancelResult = E_FAIL;
    CHECK(RequestCancel(fixture, cancelTask, &cancelResult, fixture.startedHolder->Get()) == S_OK);
    CHECK(cancelResult == S_OK);
    CHECK(fixture.compositionRange.lastTextPointerNull);
    CHECK(fixture.startedComposition.endCompositionCalls == 1);
    CHECK(cancelCompletion->count == 1);
    CHECK(cancelCompletion->results[0] == Contract::AckResult::Applied);
    return true;
}

bool TestCancelDeadlineAfterCanCancelFence()
{
    for (int stage = 3; stage <= 4; ++stage)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        PreviewExecutionHooks hooks = fixture.Hooks();
        const auto baseCanCancel = hooks.canCancel;
        int cancelHookCalls = 0;
        hooks.canCancel = [&fixture, &cancelHookCalls, &baseCanCancel, stage](const Task &queued,
                                                                               ITfContext *context,
                                                                               ITfComposition *composition) {
            ++cancelHookCalls;
            if (cancelHookCalls == stage)
            {
                fixture.now = queued.DeadlineTick();
                return true;
            }
            return baseCanCancel(queued, context, composition);
        };
        HRESULT sessionResult = E_FAIL;
        CHECK(VoicePreview::RequestVoicePreviewCancelEditSession(
                  0x77, &fixture.owner, &fixture.context, &fixture.existingComposition, task, hooks,
                  TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
        CHECK(cancelHookCalls == stage);
        CHECK(fixture.existingComposition.endCompositionCalls == 0);
        if (stage == 3)
        {
            CHECK(fixture.compositionRange.setTextCalls == 0);
            CHECK(fixture.afterEndCalls == 0);
            CHECK(completion->count == 1);
            CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
        }
        else
        {
            CHECK(fixture.compositionRange.setTextCalls == 1);
            CHECK(fixture.compositionRange.lastTextPointerNull);
            CHECK(fixture.afterEndCalls == 1);
            CHECK(fixture.lastAfterEndResult == E_ABORT);
            CHECK(completion->count == 1);
            CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
        }
    }
    return true;
}

bool TestCancelClearsExactCompositionOnce()
{
    Fixture fixture;
    fixture.UseExistingComposition();
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestCancel(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.context.startCalls == 0);
    CHECK(fixture.existingComposition.getRangeCalls == 1);
    CHECK(fixture.compositionRange.setTextCalls == 1);
    CHECK(fixture.compositionRange.lastTextPointerNull);
    CHECK(fixture.compositionRange.lastTextCount == 0);
    CHECK(fixture.compositionRange.collapseCalls == 0);
    CHECK(fixture.context.setSelectionCalls == 0);
    CHECK(fixture.existingComposition.endCompositionCalls == 1);
    CHECK(fixture.afterEndCalls == 1);
    CHECK(fixture.lastAfterEndResult == S_OK);
    CHECK(fixture.canCancelCalls >= 3);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.existingComposition.RefCount() == 1);
    CHECK(fixture.compositionRange.RefCount() == 1);
    return true;
}

bool TestCancelQueuedFencesDoNotClearNewComposition()
{
    for (int kind = 0; kind < 4; ++kind)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        fixture.context.mode = MockContext::RequestMode::Async;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestCancel(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
        CHECK(sessionResult == TF_S_ASYNC);
        CHECK(fixture.existingComposition.RefCount() == 2);
        if (kind == 0)
            CHECK(task->Cancel());
        else if (kind == 1)
            fixture.cancelAllowed = false;
        else if (kind == 2)
        {
            fixture.boundComposition = &fixture.startedComposition;
            fixture.boundEpoch = fixture.epoch;
        }
        else
            fixture.activeContext = nullptr;

        CHECK(fixture.context.RunQueued() == S_FALSE);
        CHECK(fixture.existingComposition.getRangeCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(fixture.existingComposition.endCompositionCalls == 0);
        CHECK(fixture.startedComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
        CHECK(fixture.existingComposition.RefCount() == 1);
    }
    return true;
}

bool TestCancelPreflightAndFinalApplyingRejectWithoutWrite()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        fixture.cancelAllowed = false;
        HRESULT sessionResult = E_FAIL;
        ITfComposition *expected = kind == 0 ? &fixture.existingComposition : nullptr;
        CHECK(RequestCancel(fixture, task, &sessionResult, expected) == S_FALSE);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.compositionRange.setTextCalls == 0);
        CHECK(fixture.existingComposition.endCompositionCalls == 0);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestCancelWriteFailureAndPostWriteFenceAreUnknown()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        if (kind == 0)
            fixture.compositionRange.setTextResult = E_FAIL;
        else
            fixture.compositionRange.onSetText = [&fixture]() { ++fixture.focusToken; };

        HRESULT sessionResult = E_FAIL;
        CHECK(RequestCancel(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
        CHECK(fixture.compositionRange.setTextCalls == 1);
        CHECK(fixture.existingComposition.endCompositionCalls == 0);
        if (kind == 1)
        {
            CHECK(fixture.afterEndCalls == 1);
            CHECK(fixture.lastAfterEndResult == E_ABORT);
        }
        else
        {
            CHECK(fixture.afterEndCalls == 0);
        }
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(fixture.existingComposition.RefCount() == 1);
        CHECK(fixture.compositionRange.RefCount() == 1);
    }
    return true;
}

bool TestCancelEndFailureAndHookFailuresAreUnknownOnce()
{
    const HRESULT endFailures[] = {E_FAIL, S_FALSE};
    for (HRESULT endFailure : endFailures)
    {
        Fixture fixture;
        fixture.UseExistingComposition();
        fixture.existingComposition.endCompositionResult = endFailure;
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        HRESULT sessionResult = E_FAIL;
        CHECK(RequestCancel(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
        CHECK(fixture.existingComposition.endCompositionCalls == 1);
        CHECK(fixture.afterEndCalls == 1);
        CHECK(fixture.lastAfterEndResult == endFailure);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(!task->Finish(Contract::AckResult::Applied));
        CHECK(completion->count == 1);
    }

    {
        Fixture fixture;
        fixture.UseExistingComposition();
        auto completion = std::make_shared<CompletionState>();
        auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
        PreviewExecutionHooks hooks = fixture.Hooks();
        hooks.afterEndComposition = [](TfEditCookie, ITfComposition *, HRESULT) {
            throw std::runtime_error("after-end");
        };
        HRESULT sessionResult = E_FAIL;
        CHECK(VoicePreview::RequestVoicePreviewCancelEditSession(
                  0x77, &fixture.owner, &fixture.context, &fixture.existingComposition, task, hooks,
                  TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
        CHECK(fixture.existingComposition.endCompositionCalls == 1);
        CHECK(completion->count == 1);
        CHECK(completion->results[0] == Contract::AckResult::UnknownOrPartial);
    }
    return true;
}

bool TestCancelAsyncHoldsStrongReferences()
{
    Fixture fixture;
    fixture.UseExistingComposition();
    fixture.context.mode = MockContext::RequestMode::Async;
    auto completion = std::make_shared<CompletionState>();
    auto task = MakeTask(u"", Contract::Operation::Cancel, completion);
    HRESULT sessionResult = E_FAIL;
    CHECK(RequestCancel(fixture, task, &sessionResult, &fixture.existingComposition) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(fixture.owner.RefCount() == 2);
    CHECK(fixture.context.RefCount() == 2);
    CHECK(fixture.existingComposition.RefCount() == 2);
    CHECK(completion->count == 0);
    CHECK(fixture.context.RunQueued() == S_OK);
    CHECK(fixture.existingComposition.endCompositionCalls == 1);
    CHECK(completion->count == 1);
    CHECK(completion->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.existingComposition.RefCount() == 1);
    return true;
}
} // namespace

int main()
{
    struct TestCase
    {
        const char *name;
        bool (*run)();
    };
    const TestCase tests[] = {
        {"first_preview", TestFirstPreviewStartsAdoptsWritesAndKeepsComposition},
        {"long_text", TestLongPreviewTextPreservesFullUtf16},
        {"update_preview", TestUpdateWritesExpectedCompositionWithoutStarting},
        {"async_acceptance", TestAsyncAcceptanceDoesNotWriteUntilRun},
        {"drop_queued", TestDroppedQueuedPreviewFinishesRejected},
        {"cancel_before_run", TestCancelBeforeRunPreventsWrite},
        {"queued_fences", TestQueuedIdentityAndDeadlineFences},
        {"selection_start_failures", TestSelectionAndStartFailuresRejectBeforeWrite},
        {"adopt_failures", TestAdoptRejectionOrThrowIsUnknownWithoutEnd},
        {"composition_range_failure", TestCompositionRangeFailureAfterStartIsUnknown},
        {"update_range_failure", TestUpdateRangeFailureRejectsBeforeWrite},
        {"write_failures", TestSetTextAndPostWriteFailuresAreUnknownAndNotRetried},
        {"post_fetch_fence", TestPostFetchFenceRejectsBeforeWrite},
        {"pre_start_fence", TestPreStartFenceAfterQueryInterfaceRejectsBeforeStart},
        {"adopt_reentrancy", TestAdoptReentrancyUsesUpdatedBinding},
        {"async_references", TestAsyncPreviewHoldsStrongReferences},
        {"invalid_shapes", TestCreateRejectsInvalidPreviewShapes},
        {"holder_required", TestFirstPreviewRequiresPreallocatedHolder},
        {"deadline_before_start", TestDeadlineAfterLastIdentityFenceBeforeFirstStart},
        {"deadline_before_update", TestDeadlineAfterLastIdentityFenceBeforeUpdateWrite},
        {"deadline_after_adopt", TestDeadlineAfterAdoptIsUnknownAndHolderAllowsTargetedCleanup},
        {"cancel_deadline_fence", TestCancelDeadlineAfterCanCancelFence},
        {"cancel_success", TestCancelClearsExactCompositionOnce},
        {"cancel_queued_fences", TestCancelQueuedFencesDoNotClearNewComposition},
        {"cancel_rejections", TestCancelPreflightAndFinalApplyingRejectWithoutWrite},
        {"cancel_write_fence", TestCancelWriteFailureAndPostWriteFenceAreUnknown},
        {"cancel_end_failures", TestCancelEndFailureAndHookFailuresAreUnknownOnce},
        {"cancel_async_references", TestCancelAsyncHoldsStrongReferences},
    };

    int failures = 0;
    for (const TestCase &test : tests)
    {
        if (!test.run())
        {
            std::printf("FAIL %s\n", test.name);
            ++failures;
        }
    }
    if (failures == 0)
        std::printf("voice_preview_edit_session: %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return failures == 0 ? 0 : 1;
}
