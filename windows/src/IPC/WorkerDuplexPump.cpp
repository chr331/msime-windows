#include "WorkerDuplexPump.h"

#include <cstring>
#include <exception>
#include <utility>

namespace
{
DWORD FrameBytes()
{
    return static_cast<DWORD>(FanyImeVoiceFinal::kPacketBytes);
}

DWORD AckBytes()
{
    return static_cast<DWORD>(FanyImeVoiceFinal::kAckBytes);
}
} // namespace

class WorkerDuplexPump::RunExitGuard final
{
  public:
    RunExitGuard(WorkerDuplexPump *owner, ReadOperation *read, WriteOperation *write) noexcept
        : owner_(owner), read_(read), write_(write)
    {
    }

    ~RunExitGuard()
    {
        owner_->CompleteRun(*read_, *write_);
    }

    RunExitGuard(const RunExitGuard &) = delete;
    RunExitGuard &operator=(const RunExitGuard &) = delete;

  private:
    WorkerDuplexPump *owner_;
    ReadOperation *read_;
    WriteOperation *write_;
};

WorkerDuplexPump::WorkerDuplexPump(HANDLE connectedPipe, std::uint64_t connectionGeneration, HANDLE externalStopEvent,
                                   Callbacks callbacks, std::size_t ackQueueCapacity, ObservationHooks observationHooks)
    : pipe_(connectedPipe), connection_generation_(connectionGeneration), external_stop_event_(externalStopEvent),
      ack_queue_capacity_(ackQueueCapacity), callbacks_(std::move(callbacks)),
      observation_hooks_(std::move(observationHooks))
{
    if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE || connection_generation_ == 0 ||
        external_stop_event_ == nullptr || ack_queue_capacity_ == 0)
    {
        closing_.store(true, std::memory_order_release);
        return;
    }

    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_event_ == nullptr || wake_event_ == external_stop_event_)
    {
        if (wake_event_ != nullptr)
            CloseHandle(wake_event_);
        wake_event_ = nullptr;
        closing_.store(true, std::memory_order_release);
        return;
    }
    valid_.store(true, std::memory_order_release);
}

WorkerDuplexPump::~WorkerDuplexPump()
{
    Stop();

    // Destroying the pump from Run's callback would make this wait target the calling thread.
    // Treat that call-stack violation as a hard failure instead of silently deadlocking.
    if (run_thread_id_.load(std::memory_order_acquire) == GetCurrentThreadId())
        std::terminate();

    // Linearize destruction against Run. A Run that wins the Idle->Running exchange is waited
    // for below; a Run that starts later sees Finished and returns before touching the pipe.
    const Lifecycle lifecycle = lifecycle_.exchange(Lifecycle::Finished, std::memory_order_acq_rel);
    if (lifecycle == Lifecycle::Running)
    {
        std::unique_lock<std::mutex> lock(run_mutex_);
        run_exited_.wait(lock, [this]() { return run_finished_; });
    }

    if (wake_event_ != nullptr)
    {
        CloseHandle(wake_event_);
        wake_event_ = nullptr;
    }
}

WorkerDuplexPump::PendingSnapshot WorkerDuplexPump::Pending() const noexcept
{
    PendingSnapshot pending;
    pending.readPending = read_pending_.load(std::memory_order_acquire);
    pending.writePending = write_pending_.load(std::memory_order_acquire);
    pending.closing = closing_.load(std::memory_order_acquire);
    return pending;
}

bool WorkerDuplexPump::EnqueueAck(std::uint64_t connectionGeneration, const WorkerDuplexAck &ack)
{
    if (!IsValid() || connectionGeneration != connection_generation_ || closing_.load(std::memory_order_acquire))
    {
        return false;
    }

    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (closing_.load(std::memory_order_relaxed))
            return false;
        if (ack_queue_.size() >= ack_queue_capacity_)
        {
            // Reject the new record and terminate the connection. The owner must observe this
            // false result and retire the pipe instead of continuing without an Applied ACK.
            closing_.store(true, std::memory_order_release);
            overflow = true;
        }
        else
        {
            ack_queue_.push_back(ack);
        }
    }

    if (SetEvent(wake_event_) == FALSE)
    {
        closing_.store(true, std::memory_order_release);
        return false;
    }
    return !overflow;
}

void WorkerDuplexPump::Stop() noexcept
{
    closing_.store(true, std::memory_order_release);
    if (wake_event_ != nullptr)
        SetEvent(wake_event_);
}

void WorkerDuplexPump::Run()
{
    Lifecycle expected = Lifecycle::Idle;
    if (!lifecycle_.compare_exchange_strong(expected, Lifecycle::Running, std::memory_order_acq_rel))
        return;

    ReadOperation read;
    WriteOperation write;
    RunExitGuard exitGuard(this, &read, &write);

    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        run_thread_id_.store(GetCurrentThreadId(), std::memory_order_release);
    }

    if (!IsValid())
    {
        closing_.store(true, std::memory_order_release);
        return;
    }

    read.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (read.event == nullptr || write.event == nullptr)
    {
        closing_.store(true, std::memory_order_release);
        return;
    }

    try
    {
        while (!StopRequested())
        {
            bool failed = false;

            while (!read.active)
            {
                if (StopRequested())
                    break;
                const StartResult result = BeginRead(read);
                if (result == StartResult::Pending)
                    break;
                if (result == StartResult::Completed)
                    continue;
                closing_.store(true, std::memory_order_release);
                failed = true;
                break;
            }
            if (failed || closing_.load(std::memory_order_acquire))
                break;

            while (!write.active)
            {
                if (StopRequested())
                    break;
                const StartResult result = BeginWrite(write);
                if (result == StartResult::Pending)
                    break;
                if (result == StartResult::Completed)
                    continue;
                if (result == StartResult::NoWork)
                    break;
                closing_.store(true, std::memory_order_release);
                failed = true;
                break;
            }
            if (failed || closing_.load(std::memory_order_acquire))
                break;

            if (!read.active)
                continue;

            HANDLE handles[4] = {};
            DWORD handleCount = 0;
            DWORD readIndex = MAXDWORD;
            DWORD writeIndex = MAXDWORD;
            DWORD wakeIndex = MAXDWORD;
            DWORD stopIndex = MAXDWORD;

            readIndex = handleCount;
            handles[handleCount++] = read.event;
            if (write.active)
            {
                writeIndex = handleCount;
                handles[handleCount++] = write.event;
            }
            wakeIndex = handleCount;
            handles[handleCount++] = wake_event_;
            stopIndex = handleCount;
            handles[handleCount++] = external_stop_event_;

            const DWORD waitResult = WaitForMultipleObjects(handleCount, handles, FALSE, INFINITE);
            if (waitResult == WAIT_FAILED || waitResult == WAIT_ABANDONED)
            {
                closing_.store(true, std::memory_order_release);
                break;
            }
            if (waitResult < WAIT_OBJECT_0 || waitResult >= WAIT_OBJECT_0 + handleCount)
            {
                closing_.store(true, std::memory_order_release);
                break;
            }

            const DWORD signalIndex = waitResult - WAIT_OBJECT_0;
            if (signalIndex == readIndex)
            {
                if (!CompleteRead(read))
                {
                    closing_.store(true, std::memory_order_release);
                    break;
                }
            }
            else if (writeIndex != MAXDWORD && signalIndex == writeIndex)
            {
                if (!CompleteWrite(write))
                {
                    closing_.store(true, std::memory_order_release);
                    break;
                }
            }
            else if (signalIndex == stopIndex)
            {
                closing_.store(true, std::memory_order_release);
                break;
            }
            else if (signalIndex != wakeIndex)
            {
                closing_.store(true, std::memory_order_release);
                break;
            }
        }
    }
    catch (...)
    {
        // A callback or observation hook must not bypass the drain guard below.
        closing_.store(true, std::memory_order_release);
    }
}

WorkerDuplexPump::StartResult WorkerDuplexPump::BeginRead(ReadOperation &operation)
{
    if (operation.event == nullptr || !IsValid())
        return StartResult::Failed;

    ResetEvent(operation.event);
    operation.overlapped = {};
    operation.overlapped.hEvent = operation.event;

    DWORD bytesRead = 0;
    const BOOL result = ReadFile(pipe_, operation.buffer.data(), FrameBytes(), &bytesRead, &operation.overlapped);
    if (result != FALSE)
    {
        if (bytesRead != FanyImeVoiceFinal::kPacketBytes)
            return StartResult::Failed;
        return DispatchFrame(operation.buffer) ? StartResult::Completed : StartResult::Failed;
    }

    if (GetLastError() != ERROR_IO_PENDING)
        return StartResult::Failed;

    operation.active = true;
    read_pending_.store(true, std::memory_order_release);
    if (!ObservePending(observation_hooks_.onReadPending))
        return StartResult::Failed;
    return StartResult::Pending;
}

WorkerDuplexPump::StartResult WorkerDuplexPump::BeginWrite(WriteOperation &operation)
{
    if (operation.event == nullptr || !IsValid())
        return StartResult::Failed;

    WorkerDuplexAck ack{};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (closing_.load(std::memory_order_acquire))
            return StartResult::Failed;
        if (ack_queue_.empty())
            return StartResult::NoWork;
        ack = ack_queue_.front();
        ack_queue_.pop_front();
    }

    operation.buffer = ack;
    ResetEvent(operation.event);
    operation.overlapped = {};
    operation.overlapped.hEvent = operation.event;

    DWORD bytesWritten = 0;
    const BOOL result = WriteFile(pipe_, operation.buffer.data(), AckBytes(), &bytesWritten, &operation.overlapped);
    if (result != FALSE)
    {
        return bytesWritten == FanyImeVoiceFinal::kAckBytes ? StartResult::Completed : StartResult::Failed;
    }

    if (GetLastError() != ERROR_IO_PENDING)
        return StartResult::Failed;

    operation.active = true;
    write_pending_.store(true, std::memory_order_release);
    if (!ObservePending(observation_hooks_.onWritePending))
        return StartResult::Failed;
    return StartResult::Pending;
}

bool WorkerDuplexPump::CompleteRead(ReadOperation &operation)
{
    operation.active = false;
    read_pending_.store(false, std::memory_order_release);

    DWORD bytesRead = 0;
    const BOOL result = GetOverlappedResult(pipe_, &operation.overlapped, &bytesRead, FALSE);
    ResetEvent(operation.event);
    if (result == FALSE || bytesRead != FanyImeVoiceFinal::kPacketBytes)
        return false;
    return DispatchFrame(operation.buffer);
}

bool WorkerDuplexPump::CompleteWrite(WriteOperation &operation)
{
    operation.active = false;
    write_pending_.store(false, std::memory_order_release);

    DWORD bytesWritten = 0;
    const BOOL result = GetOverlappedResult(pipe_, &operation.overlapped, &bytesWritten, FALSE);
    ResetEvent(operation.event);
    return result != FALSE && bytesWritten == FanyImeVoiceFinal::kAckBytes;
}

bool WorkerDuplexPump::StopRequested() noexcept
{
    if (closing_.load(std::memory_order_acquire))
        return true;

    const DWORD waitResult = WaitForSingleObject(external_stop_event_, 0);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
    {
        closing_.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

bool WorkerDuplexPump::DispatchFrame(const WorkerDuplexFrame &frame) noexcept
{
    if (!callbacks_.onFrame)
        return true;
    try
    {
        callbacks_.onFrame(frame);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool WorkerDuplexPump::ObservePending(const std::function<void()> &hook) noexcept
{
    if (!hook)
        return true;
    try
    {
        hook();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void WorkerDuplexPump::CancelPending(ReadOperation &operation) noexcept
{
    if (operation.active)
        CancelIoEx(pipe_, &operation.overlapped);
}

void WorkerDuplexPump::CancelPending(WriteOperation &operation) noexcept
{
    if (operation.active)
        CancelIoEx(pipe_, &operation.overlapped);
}

void WorkerDuplexPump::DrainOperation(ReadOperation &operation) noexcept
{
    if (operation.active)
    {
        DWORD bytesRead = 0;
        GetOverlappedResult(pipe_, &operation.overlapped, &bytesRead, TRUE);
        operation.active = false;
    }
    read_pending_.store(false, std::memory_order_release);
    if (operation.event != nullptr)
    {
        ResetEvent(operation.event);
        CloseHandle(operation.event);
        operation.event = nullptr;
    }
}

void WorkerDuplexPump::DrainOperation(WriteOperation &operation) noexcept
{
    if (operation.active)
    {
        DWORD bytesWritten = 0;
        GetOverlappedResult(pipe_, &operation.overlapped, &bytesWritten, TRUE);
        operation.active = false;
    }
    write_pending_.store(false, std::memory_order_release);
    if (operation.event != nullptr)
    {
        ResetEvent(operation.event);
        CloseHandle(operation.event);
        operation.event = nullptr;
    }
}

void WorkerDuplexPump::CompleteRun(ReadOperation &read, WriteOperation &write) noexcept
{
    closing_.store(true, std::memory_order_release);

    // Run is the only initiator of I/O. Cancel both directions first, then wait for each
    // completion before releasing its event/buffer or announcing the disconnect.
    CancelPending(read);
    CancelPending(write);
    DrainOperation(read);
    DrainOperation(write);

    if (callbacks_.onDrained)
    {
        try
        {
            callbacks_.onDrained();
        }
        catch (...)
        {
        }
    }

    FinishRun();
}

void WorkerDuplexPump::FinishRun() noexcept
{
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        run_thread_id_.store(0, std::memory_order_release);
        run_finished_ = true;
        // Notify before releasing run_mutex_. This is the last member access on the Run path:
        // a destructor waiting on the same mutex may release the object as soon as the lock is
        // dropped, so notifying after unlock could touch a destroyed condition variable.
        run_exited_.notify_all();
    }
}
