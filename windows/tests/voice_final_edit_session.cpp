#include "Edit/VoiceFinalEditSession.h"

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using VoiceFinal::ExecutionHooks;
using VoiceFinal::Task;
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

class MockOwner final : public IUnknown
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
        if (!IsEqualIID(riid, IID_IUnknown))
            return E_NOINTERFACE;
        *object = static_cast<IUnknown *>(this);
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

class MockContext final : public ITfContext
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
        if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_ITfContext))
            return E_NOINTERFACE;
        *object = static_cast<ITfContext *>(this);
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

  private:
    ITfEditSession *queued_ = nullptr;
    std::atomic<ULONG> refs_{1};
};

struct AckState
{
    int count = 0;
    std::vector<Contract::AckResult> results;
    std::uint64_t deliveryId = 0;
    std::uint64_t focusToken = 0;
    std::uint64_t generation = 0;
};

struct Fixture
{
    MockOwner owner;
    MockContext context;
    MockRange range;
    MockComposition composition;
    std::uint64_t now = 100;
    std::uint64_t focusToken = 0x1111;
    std::uint64_t generation = 0x2222;
    std::uint64_t epoch = 0x3333;
    ITfContext *activeContext = nullptr;
    bool identityValid = true;

    Fixture()
    {
        context.selectionRange = &range;
        composition.range_ = &range;
        activeContext = &context;
    }

    ExecutionHooks Hooks()
    {
        ExecutionHooks hooks;
        hooks.now = [this]() { return now; };
        hooks.validateIdentity = [this](const Task &task, ITfContext *context, ITfComposition *) {
            return identityValid && activeContext == context && focusToken == task.FocusToken() &&
                   generation == task.WorkerGeneration() && epoch == task.CompositionEpoch();
        };
        return hooks;
    }
};

std::shared_ptr<Task> MakeTask(const std::u16string &text, Contract::Operation operation,
                               const std::shared_ptr<AckState> &ack, std::uint64_t deadline = 1000,
                               std::uint64_t focusToken = 0x1111, std::uint64_t generation = 0x2222,
                               std::uint64_t epoch = 0x3333)
{
    return std::make_shared<Task>(0xAABBCCDDu, focusToken, generation, epoch, deadline, operation, text,
                                  [ack](const Task &task, Contract::AckResult result) {
                                      ++ack->count;
                                      ack->results.push_back(result);
                                      ack->deliveryId = task.DeliveryId();
                                      ack->focusToken = task.FocusToken();
                                      ack->generation = task.WorkerGeneration();
                                  });
}

HRESULT Request(Fixture &fixture, const std::shared_ptr<Task> &task, HRESULT *sessionResult,
                ITfComposition *composition = nullptr)
{
    return VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, composition, task,
                                                    fixture.Hooks(), TF_ES_ASYNC | TF_ES_READWRITE, sessionResult);
}

bool TestInsertWritesCompleteTextOnceAndCompletesOnce()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"hello", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.context.requestCalls == 1);
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(fixture.range.lastSetText == u"hello");
    CHECK(fixture.range.collapseCalls == 1);
    CHECK(fixture.context.setSelectionCalls == 1);
    CHECK(fixture.context.lastSelectionRange == &fixture.range);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::Applied);
    CHECK(ack->deliveryId == task->DeliveryId());
    CHECK(task->CurrentState() == Task::State::Finished);
    CHECK(task->Result() == Contract::AckResult::Applied);
    CHECK(!task->Finish(Contract::AckResult::UnknownOrPartial));
    CHECK(!task->Cancel());
    CHECK(ack->count == 1);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.range.RefCount() == 1);
    return true;
}

bool TestEmptyInsertionSelectionIsAValidWriteTarget()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"empty-selection", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(fixture.range.lastSetText == u"empty-selection");
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::Applied);
    return true;
}

bool TestSelectionFailuresRejectBeforeWrite()
{
    for (int kind = 0; kind < 4; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"must-not-write", Contract::Operation::Insert, ack);
        if (kind == 0)
        {
            fixture.context.getSelectionResult = E_FAIL;
            fixture.context.fetchedCount = 0;
        }
        else if (kind == 1)
        {
            fixture.context.getSelectionResult = S_FALSE;
        }
        else if (kind == 2)
        {
            fixture.context.fetchedCount = 0;
        }
        else
        {
            fixture.context.selectionRange = nullptr;
        }

        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.context.getSelectionCalls == 1);
        CHECK(fixture.range.setTextCalls == 0);
        CHECK(fixture.range.collapseCalls == 0);
        CHECK(fixture.context.setSelectionCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestLongTextAndEmbeddedNulAreWrittenWithoutWcslen()
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
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(text, Contract::Operation::Insert, ack);
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult) == S_OK);
        CHECK(sessionResult == S_OK);
        CHECK(fixture.range.setTextCalls == 1);
        CHECK(fixture.range.lastSetText.size() == text.size());
        CHECK(fixture.range.lastSetText == text);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::Applied);
    }
    return true;
}

bool TestSetTextFailureIsUnknownAndNeverRetried()
{
    const HRESULT failures[] = {E_FAIL, S_FALSE};
    for (HRESULT failure : failures)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"partial", Contract::Operation::Insert, ack);
        fixture.range.setTextResult = failure;
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.range.setTextCalls == 1);
        CHECK(fixture.range.lastSetText == u"partial");
        CHECK(fixture.range.collapseCalls == 0);
        CHECK(fixture.context.setSelectionCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
        CHECK(fixture.composition.endCompositionCalls == 0);
    }
    return true;
}

bool TestEmptyTextIsRejectedBeforeQueueAndWrite()
{
    const Contract::Operation operations[] = {Contract::Operation::Insert, Contract::Operation::CommitComposition};
    for (Contract::Operation operation : operations)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"", operation, ack);
        ITfComposition *composition =
            operation == Contract::Operation::CommitComposition ? &fixture.composition : nullptr;
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult, composition) == S_FALSE);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.range.setTextCalls == 0);
        CHECK(fixture.composition.getRangeCalls == 0);
        CHECK(fixture.composition.endCompositionCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
        CHECK(task->CurrentState() == Task::State::Finished);
    }
    return true;
}

bool TestPostWriteSelectionFailuresAreUnknown()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"written-once", Contract::Operation::Insert, ack);
        if (kind == 0)
            fixture.range.collapseResult = E_FAIL;
        else
            fixture.context.setSelectionResult = E_FAIL;
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult) == S_OK);
        CHECK(fixture.range.setTextCalls == 1);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
    }
    return true;
}

bool TestCommitCompositionUsesExpectedRangeAndEndsOnce()
{
    Fixture fixture;
    MockRange selectionRange;
    MockRange compositionRange;
    fixture.context.selectionRange = &selectionRange;
    fixture.composition.range_ = &compositionRange;
    auto ack = std::make_shared<AckState>();
    std::vector<std::string> events;
    const std::shared_ptr<Task> task =
        std::make_shared<Task>(0xAABBCCDDu, fixture.focusToken, fixture.generation, fixture.epoch, 1000,
                               Contract::Operation::CommitComposition, u"commit-full-text",
                               [ack, &events](const Task &completed, Contract::AckResult result) {
                                   ++ack->count;
                                   ack->results.push_back(result);
                                   ack->deliveryId = completed.DeliveryId();
                                   ack->focusToken = completed.FocusToken();
                                   ack->generation = completed.WorkerGeneration();
                                   events.push_back("ack");
                               });

    int beforeCalls = 0;
    int afterCalls = 0;
    bool beforeCookieMatches = false;
    bool afterCookieMatches = false;
    HRESULT afterResult = E_UNEXPECTED;
    HRESULT endResult = E_UNEXPECTED;
    ExecutionHooks hooks = fixture.Hooks();
    hooks.beforeEndComposition = [&](TfEditCookie cookie) {
        ++beforeCalls;
        beforeCookieMatches = cookie == fixture.context.cookie;
        events.push_back("before");
    };
    hooks.afterEndComposition = [&](TfEditCookie cookie, HRESULT result) {
        ++afterCalls;
        afterCookieMatches = cookie == fixture.context.cookie;
        afterResult = result;
        events.push_back("after");
    };
    fixture.composition.onEndComposition = [&](HRESULT result) {
        endResult = result;
        events.push_back("end");
    };

    HRESULT sessionResult = E_FAIL;
    CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition, task,
                                                   hooks, TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
    CHECK(sessionResult == S_OK);
    CHECK(fixture.composition.getRangeCalls == 1);
    CHECK(fixture.composition.endCompositionCalls == 1);
    CHECK(selectionRange.setTextCalls == 0);
    CHECK(selectionRange.collapseCalls == 0);
    CHECK(compositionRange.setTextCalls == 1);
    CHECK(compositionRange.lastSetText == u"commit-full-text");
    CHECK(compositionRange.collapseCalls == 1);
    CHECK(fixture.context.setSelectionCalls == 1);
    CHECK(fixture.context.lastSelectionRange == &compositionRange);
    CHECK(beforeCalls == 1);
    CHECK(afterCalls == 1);
    CHECK(beforeCookieMatches);
    CHECK(afterCookieMatches);
    CHECK(endResult == S_OK);
    CHECK(afterResult == S_OK);
    CHECK(events == std::vector<std::string>({"before", "end", "after", "ack"}));
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.composition.RefCount() == 1);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    return true;
}

bool TestCommitGetRangeFailureRejectsBeforeWrite()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"not-written", Contract::Operation::CommitComposition, ack);
    fixture.composition.getRangeResult = E_FAIL;
    HRESULT sessionResult = E_FAIL;
    CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition, task,
                                                   fixture.Hooks(), TF_ES_ASYNC | TF_ES_READWRITE,
                                                   &sessionResult) == S_OK);
    CHECK(fixture.composition.getRangeCalls == 1);
    CHECK(fixture.composition.endCompositionCalls == 0);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestEndCompositionFailureIsUnknown()
{
    const HRESULT failures[] = {E_FAIL, S_FALSE};
    for (HRESULT failure : failures)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"composed-but-not-ended", Contract::Operation::CommitComposition, ack);
        fixture.composition.endCompositionResult = failure;
        int beforeCalls = 0;
        int afterCalls = 0;
        HRESULT observedEndResult = E_UNEXPECTED;
        ExecutionHooks hooks = fixture.Hooks();
        hooks.beforeEndComposition = [&beforeCalls](TfEditCookie) { ++beforeCalls; };
        hooks.afterEndComposition = [&afterCalls, &observedEndResult](TfEditCookie, HRESULT result) {
            ++afterCalls;
            observedEndResult = result;
        };
        HRESULT sessionResult = E_FAIL;
        CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition,
                                                       task, hooks, TF_ES_ASYNC | TF_ES_READWRITE,
                                                       &sessionResult) == S_OK);
        CHECK(fixture.composition.endCompositionCalls == 1);
        CHECK(beforeCalls == 1);
        CHECK(afterCalls == 1);
        CHECK(observedEndResult == failure);
        CHECK(fixture.range.setTextCalls == 1);
        CHECK(fixture.range.lastSetText == u"composed-but-not-ended");
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
    }

    // With beforeEndComposition empty, the original write-then-one-End path remains and the
    // after hook still receives the real EndComposition failure.
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"no-before-hook", Contract::Operation::CommitComposition, ack);
    fixture.composition.endCompositionResult = E_FAIL;
    int afterCalls = 0;
    HRESULT observedEndResult = E_UNEXPECTED;
    ExecutionHooks hooks = fixture.Hooks();
    hooks.afterEndComposition = [&afterCalls, &observedEndResult](TfEditCookie, HRESULT result) {
        ++afterCalls;
        observedEndResult = result;
    };
    HRESULT sessionResult = E_FAIL;
    CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition, task,
                                                   hooks, TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
    CHECK(fixture.composition.endCompositionCalls == 1);
    CHECK(afterCalls == 1);
    CHECK(observedEndResult == E_FAIL);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
    return true;
}

bool TestBeforeEndCompositionThrowSkipsEndAndIsUnknown()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"before-throws", Contract::Operation::CommitComposition, ack);
    int beforeCalls = 0;
    int afterCalls = 0;
    HRESULT observedEndResult = E_UNEXPECTED;
    std::vector<std::string> events;
    ExecutionHooks hooks = fixture.Hooks();
    hooks.beforeEndComposition = [&](TfEditCookie) {
        ++beforeCalls;
        events.push_back("before");
        throw 17;
    };
    hooks.afterEndComposition = [&](TfEditCookie, HRESULT result) {
        ++afterCalls;
        observedEndResult = result;
        events.push_back("after");
    };
    fixture.composition.onEndComposition = [&](HRESULT) { events.push_back("end"); };

    HRESULT sessionResult = E_FAIL;
    CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition, task,
                                                   hooks, TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
    CHECK(sessionResult == E_ABORT);
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(fixture.range.collapseCalls == 1);
    CHECK(fixture.context.setSelectionCalls == 1);
    CHECK(beforeCalls == 1);
    CHECK(fixture.composition.endCompositionCalls == 0);
    CHECK(afterCalls == 1);
    CHECK(observedEndResult == E_ABORT);
    CHECK(events == std::vector<std::string>({"before", "after"}));
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
    CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
    CHECK(task->CurrentState() == Task::State::Finished);
    return true;
}

bool TestAfterEndCompositionThrowIsUnknown()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"after-throws", Contract::Operation::CommitComposition, ack);
    int beforeCalls = 0;
    int afterCalls = 0;
    ExecutionHooks hooks = fixture.Hooks();
    hooks.beforeEndComposition = [&beforeCalls](TfEditCookie) { ++beforeCalls; };
    hooks.afterEndComposition = [&afterCalls](TfEditCookie, HRESULT) {
        ++afterCalls;
        throw 23;
    };

    HRESULT sessionResult = E_FAIL;
    CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition, task,
                                                   hooks, TF_ES_ASYNC | TF_ES_READWRITE, &sessionResult) == S_OK);
    CHECK(sessionResult == E_FAIL);
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(fixture.composition.endCompositionCalls == 1);
    CHECK(beforeCalls == 1);
    CHECK(afterCalls == 1);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
    CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
    CHECK(!task->Finish(Contract::AckResult::Applied));
    CHECK(fixture.composition.RefCount() == 1);
    return true;
}

bool TestBeforeEndCompositionFenceChangeSkipsEnd()
{
    for (int kind = 0; kind < 5; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"post-hook-fence", Contract::Operation::CommitComposition, ack);
        int beforeCalls = 0;
        int afterCalls = 0;
        int setTextCallsAtBefore = 0;
        int collapseCallsAtBefore = 0;
        int setSelectionCallsAtBefore = 0;
        HRESULT observedEndResult = E_UNEXPECTED;
        ExecutionHooks hooks = fixture.Hooks();
        hooks.beforeEndComposition = [&](TfEditCookie) {
            ++beforeCalls;
            setTextCallsAtBefore = fixture.range.setTextCalls;
            collapseCallsAtBefore = fixture.range.collapseCalls;
            setSelectionCallsAtBefore = fixture.context.setSelectionCalls;
            if (kind == 0)
                ++fixture.epoch;
            else if (kind == 1)
                ++fixture.focusToken;
            else if (kind == 2)
                fixture.activeContext = nullptr;
            else if (kind == 3)
                fixture.now = 1000;
            else
                ++fixture.generation;
        };
        hooks.afterEndComposition = [&afterCalls, &observedEndResult](TfEditCookie, HRESULT result) {
            ++afterCalls;
            observedEndResult = result;
        };

        HRESULT sessionResult = E_FAIL;
        CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition,
                                                       task, hooks, TF_ES_ASYNC | TF_ES_READWRITE,
                                                       &sessionResult) == S_OK);
        CHECK(sessionResult == E_ABORT);
        CHECK(setTextCallsAtBefore == 1);
        CHECK(collapseCallsAtBefore == 1);
        CHECK(setSelectionCallsAtBefore == 1);
        CHECK(beforeCalls == 1);
        CHECK(fixture.composition.endCompositionCalls == 0);
        CHECK(afterCalls == 1);
        CHECK(observedEndResult == E_ABORT);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
        CHECK(task->Result() == Contract::AckResult::UnknownOrPartial);
    }
    return true;
}

bool TestCompositionEndHooksSkipPreEndFailures()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"pre-end-failure", Contract::Operation::CommitComposition, ack);
        if (kind == 0)
            fixture.range.setTextResult = E_FAIL;
        else if (kind == 1)
            fixture.range.collapseResult = E_FAIL;
        else
            fixture.context.setSelectionResult = E_FAIL;

        int beforeCalls = 0;
        int afterCalls = 0;
        ExecutionHooks hooks = fixture.Hooks();
        hooks.beforeEndComposition = [&beforeCalls](TfEditCookie) { ++beforeCalls; };
        hooks.afterEndComposition = [&afterCalls](TfEditCookie, HRESULT) { ++afterCalls; };

        HRESULT sessionResult = E_FAIL;
        CHECK(VoiceFinal::RequestVoiceFinalEditSession(0x77, &fixture.owner, &fixture.context, &fixture.composition,
                                                       task, hooks, TF_ES_ASYNC | TF_ES_READWRITE,
                                                       &sessionResult) == S_OK);
        CHECK(beforeCalls == 0);
        CHECK(fixture.composition.endCompositionCalls == 0);
        CHECK(afterCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::UnknownOrPartial);
    }
    return true;
}

bool TestAsyncAcceptanceDoesNotAckUntilRun()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"queued", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(fixture.context.HasQueuedSession());
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 0);
    CHECK(task->CurrentState() == Task::State::Queued);
    CHECK(fixture.owner.RefCount() == 2);
    CHECK(fixture.context.RefCount() == 2);
    CHECK(fixture.range.RefCount() == 1);

    CHECK(fixture.context.RunQueued() == S_OK);
    CHECK(!fixture.context.HasQueuedSession());
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::Applied);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    CHECK(fixture.range.RefCount() == 1);
    return true;
}

bool TestDroppedQueuedSessionFinishesRejected()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"dropped", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(ack->count == 0);
    fixture.context.DropQueued();
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    CHECK(task->CurrentState() == Task::State::Finished);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    return true;
}

bool TestCancelBeforeRunPreventsWrite()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"cancelled", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    CHECK(task->Cancel());
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    CHECK(fixture.context.RunQueued() == S_FALSE);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(!task->Cancel());
    return true;
}

bool TestDeadlineBeforeQueuedExecutionRejects()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"late", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    fixture.now = 1000;
    CHECK(fixture.context.RunQueued() == S_FALSE);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestFenceChangedAfterRangeFetchRejectsBeforeWrite()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        const Contract::Operation operation =
            kind == 0 ? Contract::Operation::Insert : Contract::Operation::CommitComposition;
        auto task = MakeTask(u"post-fetch-fence", operation, ack);
        bool cancelResult = false;
        ITfComposition *composition =
            operation == Contract::Operation::CommitComposition ? &fixture.composition : nullptr;
        if (kind == 0)
        {
            fixture.context.onGetSelection = [&fixture]() { ++fixture.epoch; };
        }
        else if (kind == 1)
        {
            fixture.composition.onGetRange = [&fixture]() { fixture.now = 1000; };
        }
        else
        {
            fixture.composition.onGetRange = [&task, &cancelResult]() { cancelResult = task->Cancel(); };
        }

        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult, composition) == S_OK);
        CHECK(fixture.context.getSelectionCalls == 1);
        if (kind == 2)
            CHECK(cancelResult);
        if (operation == Contract::Operation::CommitComposition)
            CHECK(fixture.composition.getRangeCalls == 1);
        CHECK(fixture.range.setTextCalls == 0);
        CHECK(fixture.range.collapseCalls == 0);
        CHECK(fixture.context.setSelectionCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
        CHECK(task->CurrentState() == Task::State::Finished);
    }
    return true;
}

bool TestAsyncCommitHoldsCompositionReferenceUntilRunOrDrop()
{
    for (int action = 0; action < 2; ++action)
    {
        Fixture fixture;
        fixture.context.mode = MockContext::RequestMode::Async;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"queued-commit", Contract::Operation::CommitComposition, ack);
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult, &fixture.composition) == S_OK);
        CHECK(sessionResult == TF_S_ASYNC);
        CHECK(fixture.context.HasQueuedSession());
        CHECK(fixture.composition.RefCount() == 2);
        CHECK(fixture.owner.RefCount() == 2);
        CHECK(fixture.context.RefCount() == 2);
        CHECK(ack->count == 0);

        if (action == 0)
        {
            CHECK(fixture.context.RunQueued() == S_OK);
            CHECK(ack->count == 1);
            CHECK(ack->results[0] == Contract::AckResult::Applied);
        }
        else
        {
            fixture.context.DropQueued();
            CHECK(ack->count == 1);
            CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
        }

        CHECK(!fixture.context.HasQueuedSession());
        CHECK(fixture.composition.RefCount() == 1);
        CHECK(fixture.owner.RefCount() == 1);
        CHECK(fixture.context.RefCount() == 1);
    }
    return true;
}

bool RunIdentityFenceCase(int field)
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Async;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"old-fence", Contract::Operation::Insert, ack, 1000, fixture.focusToken, fixture.generation,
                         fixture.epoch);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(sessionResult == TF_S_ASYNC);
    if (field == 0)
        fixture.activeContext = nullptr;
    else if (field == 1)
        ++fixture.focusToken;
    else if (field == 2)
        ++fixture.epoch;
    else
        ++fixture.generation;
    CHECK(fixture.context.RunQueued() == S_FALSE);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    return true;
}

bool TestContextFocusEpochAndGenerationAreFencedBeforeWrite()
{
    for (int field = 0; field < 4; ++field)
    {
        if (!RunIdentityFenceCase(field))
            return false;
    }
    return true;
}

bool TestPreQueueExpiredTaskNeverCallsRequestEditSession()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        Fixture fixture;
        auto ack = std::make_shared<AckState>();
        auto task = MakeTask(u"preflight", Contract::Operation::Insert, ack);
        if (kind == 0)
            fixture.now = 1000;
        else
            fixture.identityValid = false;
        HRESULT sessionResult = E_FAIL;
        CHECK(Request(fixture, task, &sessionResult) == S_FALSE);
        CHECK(fixture.context.requestCalls == 0);
        CHECK(fixture.range.setTextCalls == 0);
        CHECK(ack->count == 1);
        CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    }
    return true;
}

bool TestOuterRequestRejectionFinishesWithoutWrite()
{
    Fixture fixture;
    fixture.context.mode = MockContext::RequestMode::Reject;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"rejected", Contract::Operation::Insert, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == E_FAIL);
    CHECK(fixture.context.requestCalls == 1);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
    CHECK(fixture.owner.RefCount() == 1);
    CHECK(fixture.context.RefCount() == 1);
    return true;
}

bool TestCancelDuringSetTextCannotInterruptApplying()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"in-flight", Contract::Operation::Insert, ack);
    bool cancelResult = true;
    fixture.range.onSetText = [&task, &cancelResult]() { cancelResult = task->Cancel(); };
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_OK);
    CHECK(!cancelResult);
    CHECK(fixture.range.setTextCalls == 1);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::Applied);
    CHECK(task->Result() == Contract::AckResult::Applied);
    CHECK(!task->Cancel());
    CHECK(ack->count == 1);
    return true;
}

bool TestInvalidCommitWithoutCompositionRejectsBeforeQueue()
{
    Fixture fixture;
    auto ack = std::make_shared<AckState>();
    auto task = MakeTask(u"no-composition", Contract::Operation::CommitComposition, ack);
    HRESULT sessionResult = E_FAIL;
    CHECK(Request(fixture, task, &sessionResult) == S_FALSE);
    CHECK(fixture.context.requestCalls == 0);
    CHECK(fixture.range.setTextCalls == 0);
    CHECK(ack->count == 1);
    CHECK(ack->results[0] == Contract::AckResult::RejectedNoWrite);
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
        {"insert_once", TestInsertWritesCompleteTextOnceAndCompletesOnce},
        {"empty_selection", TestEmptyInsertionSelectionIsAValidWriteTarget},
        {"selection_rejections", TestSelectionFailuresRejectBeforeWrite},
        {"long_text", TestLongTextAndEmbeddedNulAreWrittenWithoutWcslen},
        {"set_text_failure", TestSetTextFailureIsUnknownAndNeverRetried},
        {"empty_text", TestEmptyTextIsRejectedBeforeQueueAndWrite},
        {"post_write_failure", TestPostWriteSelectionFailuresAreUnknown},
        {"commit_success", TestCommitCompositionUsesExpectedRangeAndEndsOnce},
        {"commit_get_range_failure", TestCommitGetRangeFailureRejectsBeforeWrite},
        {"end_composition_failure", TestEndCompositionFailureIsUnknown},
        {"before_end_throw", TestBeforeEndCompositionThrowSkipsEndAndIsUnknown},
        {"after_end_throw", TestAfterEndCompositionThrowIsUnknown},
        {"before_end_fence", TestBeforeEndCompositionFenceChangeSkipsEnd},
        {"pre_end_hook_skip", TestCompositionEndHooksSkipPreEndFailures},
        {"async_acceptance", TestAsyncAcceptanceDoesNotAckUntilRun},
        {"drop_queued", TestDroppedQueuedSessionFinishesRejected},
        {"cancel_before_run", TestCancelBeforeRunPreventsWrite},
        {"deadline_before_run", TestDeadlineBeforeQueuedExecutionRejects},
        {"post_fetch_fence", TestFenceChangedAfterRangeFetchRejectsBeforeWrite},
        {"async_commit_references", TestAsyncCommitHoldsCompositionReferenceUntilRunOrDrop},
        {"identity_fences", TestContextFocusEpochAndGenerationAreFencedBeforeWrite},
        {"pre_queue_rejection", TestPreQueueExpiredTaskNeverCallsRequestEditSession},
        {"outer_rejection", TestOuterRequestRejectionFinishesWithoutWrite},
        {"cancel_in_set_text", TestCancelDuringSetTextCannotInterruptApplying},
        {"commit_without_composition", TestInvalidCommitWithoutCompositionRejectsBeforeQueue},
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
        std::printf("voice_final_edit_session: %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return failures == 0 ? 0 : 1;
}
