#pragma once

// One-shot duplex transport for the connected worker pipe. Run is the only thread that touches
// OVERLAPPED operations: it keeps one exact-size message read and one exact-size ACK write in
// flight independently. EnqueueAck only appends to a bounded queue and signals a wake event.
#include <windows.h>

#include "../../../engine/contracts/voice_final_delivery.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

using WorkerDuplexFrame = std::array<std::uint8_t, FanyImeVoiceFinal::kPacketBytes>;
using WorkerDuplexAck = std::array<std::uint8_t, FanyImeVoiceFinal::kAckBytes>;

inline constexpr std::size_t kDefaultWorkerAckQueueCapacity = 64;

class WorkerDuplexPump final
{
  public:
    struct Callbacks
    {
        // Called on the Run thread with one complete 404-byte message-mode frame. The callback
        // runs without any pump queue lock held and must not destroy this object.
        std::function<void(const WorkerDuplexFrame &)> onFrame;
        // Called once after both pending I/O operations have been cancelled and drained. This is
        // the first point at which the owner may unpublish or close the non-owning pipe. The
        // callback must not destroy this object or wait for Run.
        std::function<void()> onDrained;
    };

    struct ObservationHooks
    {
        // Diagnostic hooks fire only after the real Win32 call returned ERROR_IO_PENDING.
        std::function<void()> onReadPending;
        std::function<void()> onWritePending;
    };

    struct PendingSnapshot
    {
        bool readPending = false;
        bool writePending = false;
        bool closing = false;
    };

    // The connected pipe and externalStopEvent are non-owning. The pipe must remain valid at least
    // until onDrained has been called; the external stop event must remain valid until Run returns.
    // The owner must keep the object alive until Run returns; destruction is the one allowed
    // concurrent retirement: it requests stop and waits for Run to finish before releasing events
    // and buffers. Callbacks must neither destroy the object nor wait for Run.
    // ackQueueCapacity is configurable only to make overflow and real pipe-buffer pressure
    // testable; production uses the default 64.
    WorkerDuplexPump(HANDLE connectedPipe, std::uint64_t connectionGeneration, HANDLE externalStopEvent,
                     Callbacks callbacks, std::size_t ackQueueCapacity = kDefaultWorkerAckQueueCapacity,
                     ObservationHooks observationHooks = {});
    ~WorkerDuplexPump();

    WorkerDuplexPump(const WorkerDuplexPump &) = delete;
    WorkerDuplexPump &operator=(const WorkerDuplexPump &) = delete;
    WorkerDuplexPump(WorkerDuplexPump &&) = delete;
    WorkerDuplexPump &operator=(WorkerDuplexPump &&) = delete;

    bool IsValid() const noexcept
    {
        return valid_.load(std::memory_order_acquire);
    }

    PendingSnapshot Pending() const noexcept;

    // Short queue-only operation. It never calls WriteFile. A stale generation is rejected without
    // closing the pump. A full queue rejects the new ACK, marks the connection closing and wakes
    // Run so the owner can tear the pipe down; an Applied result is never silently dropped while
    // the connection remains advertised as healthy.
    bool EnqueueAck(std::uint64_t connectionGeneration, const WorkerDuplexAck &ack);

    // Safe to call repeatedly and from any thread other than Run's callback stack. It only marks
    // closing and signals the internal wake event; Run remains the sole CancelIoEx/drain owner.
    void Stop() noexcept;

    // Must be called exactly once, on the worker thread. Returns only after pending read/write
    // operations are drained and onDrained has been called. Do not call Run twice, destroy the
    // object from within either callback, or wait for Run from a callback. Destruction retires an
    // Idle pump so a Run that starts concurrently with destruction returns without touching pipe
    // or callback state.
    void Run();

  private:
    struct ReadOperation
    {
        OVERLAPPED overlapped{};
        HANDLE event = nullptr;
        WorkerDuplexFrame buffer{};
        bool active = false;
    };

    struct WriteOperation
    {
        OVERLAPPED overlapped{};
        HANDLE event = nullptr;
        WorkerDuplexAck buffer{};
        bool active = false;
    };

    enum class StartResult
    {
        Pending,
        Completed,
        NoWork,
        Failed,
    };

    enum class Lifecycle
    {
        Idle,
        Running,
        Finished,
    };

    class RunExitGuard;

    StartResult BeginRead(ReadOperation &operation);
    StartResult BeginWrite(WriteOperation &write);
    bool CompleteRead(ReadOperation &operation);
    bool CompleteWrite(WriteOperation &operation);
    bool StopRequested() noexcept;
    bool DispatchFrame(const WorkerDuplexFrame &frame) noexcept;
    bool ObservePending(const std::function<void()> &hook) noexcept;
    void CancelPending(ReadOperation &operation) noexcept;
    void CancelPending(WriteOperation &operation) noexcept;
    void DrainOperation(ReadOperation &operation) noexcept;
    void DrainOperation(WriteOperation &operation) noexcept;
    void CompleteRun(ReadOperation &read, WriteOperation &write) noexcept;
    void FinishRun() noexcept;

    HANDLE pipe_;
    const std::uint64_t connection_generation_;
    HANDLE external_stop_event_;
    const std::size_t ack_queue_capacity_;
    Callbacks callbacks_;
    ObservationHooks observation_hooks_;

    HANDLE wake_event_ = nullptr;
    std::atomic<bool> valid_{false};
    std::atomic<bool> closing_{false};
    std::atomic<Lifecycle> lifecycle_{Lifecycle::Idle};
    std::atomic<bool> read_pending_{false};
    std::atomic<bool> write_pending_{false};
    std::atomic<DWORD> run_thread_id_{0};

    std::mutex queue_mutex_;
    std::deque<WorkerDuplexAck> ack_queue_;

    std::mutex run_mutex_;
    std::condition_variable run_exited_;
    bool run_finished_ = false;
};
