#include "IPC/WorkerDuplexPump.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;

constexpr std::uint64_t kGeneration = 0x1122334455667788ull;
constexpr DWORD kWaitMilliseconds = 4000;

std::atomic<unsigned> g_pipeSequence{0};

#define CHECK_TRUE(expression)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s (GetLastError=%lu)\n", __FILE__, __LINE__, #expression,    \
                         static_cast<unsigned long>(GetLastError()));                                                  \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

struct DrainedState
{
    std::size_t count = 0;
    bool readPending = true;
    bool writePending = true;
};

struct Observation
{
    void OnReadPending()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++readPendingCount_;
        }
        changed_.notify_all();
    }

    void OnWritePending()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++writePendingCount_;
        }
        changed_.notify_all();
    }

    void OnFrame(const WorkerDuplexFrame &frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = frame;
            ++frameCount_;
        }
        changed_.notify_all();
    }

    void OnDrained(const WorkerDuplexPump *pump)
    {
        const WorkerDuplexPump::PendingSnapshot pending = pump->Pending();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++drained_.count;
            drained_.readPending = pending.readPending;
            drained_.writePending = pending.writePending;
        }
        changed_.notify_all();
    }

    bool WaitReadPending(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count]() { return readPendingCount_ >= count; });
    }

    bool WaitWritePending(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count]() { return writePendingCount_ >= count; });
    }

    bool WaitFrame(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count]() { return frameCount_ >= count; });
    }

    bool WaitDrained(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, count]() { return drained_.count >= count; });
    }

    std::size_t ReadPendingCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return readPendingCount_;
    }

    std::size_t WritePendingCount()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return writePendingCount_;
    }

    WorkerDuplexFrame Frame()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_;
    }

    DrainedState Drained()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return drained_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t readPendingCount_ = 0;
    std::size_t writePendingCount_ = 0;
    std::size_t frameCount_ = 0;
    WorkerDuplexFrame frame_{};
    DrainedState drained_{};
};

class PipePair
{
  public:
    PipePair() = default;
    ~PipePair()
    {
        Close();
    }

    PipePair(const PipePair &) = delete;
    PipePair &operator=(const PipePair &) = delete;

    bool Open(DWORD outputBufferBytes, DWORD inputBufferBytes)
    {
        Close();
        wchar_t name[160] = {};
        const unsigned sequence = g_pipeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        std::swprintf(name, sizeof(name) / sizeof(name[0]), L"\\\\.\\pipe\\msime-worker-duplex-%lu-%u",
                      GetCurrentProcessId(), sequence);

        server_ = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, outputBufferBytes,
                                   inputBufferBytes, 0, nullptr);
        if (server_ == INVALID_HANDLE_VALUE)
            return false;

        HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connectEvent == nullptr)
            return false;

        OVERLAPPED connectOverlapped{};
        connectOverlapped.hEvent = connectEvent;
        const BOOL connecting = ConnectNamedPipe(server_, &connectOverlapped);
        const DWORD connectError = connecting ? ERROR_SUCCESS : GetLastError();
        if (!connecting && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(connectEvent);
            Close();
            SetLastError(connectError);
            return false;
        }

        for (int attempt = 0; attempt < 32 && client_ == INVALID_HANDLE_VALUE; ++attempt)
        {
            client_ = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                                  nullptr);
            if (client_ == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY)
            {
                WaitNamedPipeW(name, 500);
                continue;
            }
        }
        if (client_ == INVALID_HANDLE_VALUE)
        {
            if (connectError == ERROR_IO_PENDING)
            {
                DWORD ignored = 0;
                CancelIoEx(server_, &connectOverlapped);
                GetOverlappedResult(server_, &connectOverlapped, &ignored, TRUE);
            }
            CloseHandle(connectEvent);
            Close();
            return false;
        }

        if (connectError == ERROR_IO_PENDING)
        {
            const DWORD waited = WaitForSingleObject(connectEvent, kWaitMilliseconds);
            if (waited != WAIT_OBJECT_0)
            {
                DWORD ignored = 0;
                CancelIoEx(server_, &connectOverlapped);
                GetOverlappedResult(server_, &connectOverlapped, &ignored, TRUE);
                CloseHandle(connectEvent);
                Close();
                SetLastError(waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
                return false;
            }

            DWORD ignored = 0;
            const BOOL completed = GetOverlappedResult(server_, &connectOverlapped, &ignored, FALSE);
            const DWORD completedError = completed ? ERROR_SUCCESS : GetLastError();
            if (!completed && completedError != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(connectEvent);
                Close();
                SetLastError(completedError);
                return false;
            }
        }

        CloseHandle(connectEvent);

        DWORD readMode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(client_, &readMode, nullptr, nullptr))
        {
            Close();
            return false;
        }
        return true;
    }

    void CloseClient()
    {
        if (client_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(client_);
            client_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE server() const noexcept
    {
        return server_;
    }
    HANDLE client() const noexcept
    {
        return client_;
    }

  private:
    void Close()
    {
        CloseClient();
        if (server_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(server_);
            server_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE server_ = INVALID_HANDLE_VALUE;
    HANDLE client_ = INVALID_HANDLE_VALUE;
};

bool ReadMessage(HANDLE pipe, void *buffer, DWORD expectedBytes, DWORD timeoutMilliseconds)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    DWORD transferred = 0;
    const BOOL started = ReadFile(pipe, buffer, expectedBytes, &transferred, &overlapped);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && error != ERROR_IO_PENDING)
    {
        CloseHandle(event);
        SetLastError(error);
        return false;
    }

    if (!started)
    {
        const DWORD waited = WaitForSingleObject(event, timeoutMilliseconds);
        if (waited != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            CloseHandle(event);
            SetLastError(waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
            return false;
        }
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
        {
            const DWORD completedError = GetLastError();
            CloseHandle(event);
            SetLastError(completedError);
            return false;
        }
    }

    CloseHandle(event);
    if (transferred != expectedBytes)
    {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

bool WriteMessage(HANDLE pipe, const void *buffer, DWORD bytes, DWORD timeoutMilliseconds)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    DWORD transferred = 0;
    const BOOL started = WriteFile(pipe, buffer, bytes, &transferred, &overlapped);
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && error != ERROR_IO_PENDING)
    {
        CloseHandle(event);
        SetLastError(error);
        return false;
    }

    if (!started)
    {
        const DWORD waited = WaitForSingleObject(event, timeoutMilliseconds);
        if (waited != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
            CloseHandle(event);
            SetLastError(waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
            return false;
        }
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
        {
            const DWORD completedError = GetLastError();
            CloseHandle(event);
            SetLastError(completedError);
            return false;
        }
    }

    CloseHandle(event);
    if (transferred != bytes)
    {
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    return true;
}

class PumpThread
{
  public:
    explicit PumpThread(WorkerDuplexPump &pump) : pump_(pump)
    {
        thread_ = std::thread([this]() {
            pump_.Run();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                returned_ = true;
            }
            returnedChanged_.notify_all();
        });
    }

    ~PumpThread()
    {
        pump_.Stop();
        if (thread_.joinable())
            thread_.join();
    }

    PumpThread(const PumpThread &) = delete;
    PumpThread &operator=(const PumpThread &) = delete;

    bool WaitReturned(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return returnedChanged_.wait_for(lock, timeout, [this]() { return returned_; });
    }

    void Join()
    {
        if (thread_.joinable())
            thread_.join();
    }

    void StopAndJoin(std::chrono::milliseconds timeout)
    {
        pump_.Stop();
        CHECK_TRUE(WaitReturned(timeout));
        Join();
    }

  private:
    WorkerDuplexPump &pump_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable returnedChanged_;
    bool returned_ = false;
};

WorkerDuplexFrame MakeFrame(std::uint8_t seed)
{
    WorkerDuplexFrame frame{};
    for (std::size_t index = 0; index < frame.size(); ++index)
        frame[index] = static_cast<std::uint8_t>((seed + index * 17u) & 0xffu);
    return frame;
}

WorkerDuplexAck MakeAck(std::uint8_t seed)
{
    WorkerDuplexAck ack{};
    for (std::size_t index = 0; index < ack.size(); ++index)
        ack[index] = static_cast<std::uint8_t>((seed + index * 5u) & 0xffu);
    return ack;
}

bool ForcePendingWrite(WorkerDuplexPump &pump, Observation &observation, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    unsigned sequence = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const WorkerDuplexAck ack = MakeAck(static_cast<std::uint8_t>(sequence++));
        CHECK_TRUE(pump.EnqueueAck(kGeneration, ack));
        if (observation.WaitWritePending(1, 30ms))
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void CheckDrained(Observation &observation)
{
    CHECK_TRUE(observation.WaitDrained(1, 1000ms));
    const DrainedState drained = observation.Drained();
    CHECK_TRUE(drained.count == 1);
    CHECK_TRUE(!drained.readPending);
    CHECK_TRUE(!drained.writePending);
}

void TestReadPendingThenAckReachesPeer()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(4096, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onFrame = [&observation](const WorkerDuplexFrame &frame) { observation.OnFrame(frame); };
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };
    hooks.onWritePending = [&observation]() { observation.OnWritePending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 8, std::move(hooks));
    pumpPointer = &pump;
    CHECK_TRUE(pump.IsValid());

    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));
    CHECK_TRUE(pump.Pending().readPending);

    const WorkerDuplexAck ack = MakeAck(0x31);
    CHECK_TRUE(pump.EnqueueAck(kGeneration, ack));

    WorkerDuplexAck received{};
    CHECK_TRUE(ReadMessage(pair.client(), received.data(), static_cast<DWORD>(received.size()), kWaitMilliseconds));
    CHECK_TRUE(received == ack);
    CHECK_TRUE(!pump.Pending().closing);

    runner.StopAndJoin(std::chrono::milliseconds(kWaitMilliseconds));
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestStaleGenerationIsRejectedWithoutClosing()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(4096, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 8, std::move(hooks));
    pumpPointer = &pump;
    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));

    const WorkerDuplexAck staleAck = MakeAck(0x71);
    CHECK_TRUE(!pump.EnqueueAck(kGeneration + 1, staleAck));
    CHECK_TRUE(!pump.Pending().closing);

    const WorkerDuplexAck freshAck = MakeAck(0x72);
    CHECK_TRUE(pump.EnqueueAck(kGeneration, freshAck));
    WorkerDuplexAck received{};
    CHECK_TRUE(ReadMessage(pair.client(), received.data(), static_cast<DWORD>(received.size()), kWaitMilliseconds));
    CHECK_TRUE(received == freshAck);

    runner.StopAndJoin(std::chrono::milliseconds(kWaitMilliseconds));
    CHECK_TRUE(!pump.EnqueueAck(kGeneration, freshAck));
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestWritePendingDoesNotBlockRead()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(512, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onFrame = [&observation](const WorkerDuplexFrame &frame) { observation.OnFrame(frame); };
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };
    hooks.onWritePending = [&observation]() { observation.OnWritePending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 512, std::move(hooks));
    pumpPointer = &pump;
    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));

    CHECK_TRUE(ForcePendingWrite(pump, observation, 5s));
    CHECK_TRUE(pump.Pending().writePending);
    CHECK_TRUE(pump.Pending().readPending);

    const WorkerDuplexFrame frame = MakeFrame(0x19);
    CHECK_TRUE(WriteMessage(pair.client(), frame.data(), static_cast<DWORD>(frame.size()), kWaitMilliseconds));
    CHECK_TRUE(observation.WaitFrame(1, std::chrono::milliseconds(kWaitMilliseconds)));
    CHECK_TRUE(observation.Frame() == frame);
    CHECK_TRUE(observation.WaitReadPending(2, std::chrono::milliseconds(kWaitMilliseconds)));
    CHECK_TRUE(pump.Pending().readPending);
    CHECK_TRUE(pump.Pending().writePending);
    CHECK_TRUE(observation.Drained().count == 0);

    // The external stop event is the only shutdown request on this path. Run must cancel and
    // drain both outstanding directions before announcing onDrained.
    CHECK_TRUE(SetEvent(stopEvent) != FALSE);
    CHECK_TRUE(runner.WaitReturned(std::chrono::milliseconds(kWaitMilliseconds)));
    runner.Join();
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestQueueFullFailsAndCloses()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(512, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };
    hooks.onWritePending = [&observation]() { observation.OnWritePending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 16, std::move(hooks));
    pumpPointer = &pump;
    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));
    CHECK_TRUE(ForcePendingWrite(pump, observation, 5s));

    bool rejected = false;
    const auto rejectionStart = std::chrono::steady_clock::now();
    for (int index = 0; index < 64 && !rejected; ++index)
    {
        const WorkerDuplexAck ack = MakeAck(static_cast<std::uint8_t>(0x80 + index));
        rejected = !pump.EnqueueAck(kGeneration, ack);
    }
    const auto rejectionElapsed = std::chrono::steady_clock::now() - rejectionStart;
    CHECK_TRUE(rejected);
    CHECK_TRUE(rejectionElapsed < 500ms);
    CHECK_TRUE(pump.Pending().closing);
    CHECK_TRUE(!pump.EnqueueAck(kGeneration, MakeAck(0xfe)));

    CHECK_TRUE(runner.WaitReturned(std::chrono::milliseconds(kWaitMilliseconds)));
    runner.Join();
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestMessageSizeTerminates(DWORD messageBytes)
{
    PipePair pair;
    CHECK_TRUE(pair.Open(4096, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 8, std::move(hooks));
    pumpPointer = &pump;
    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));

    const std::vector<std::uint8_t> malformed(messageBytes, 0x5a);
    CHECK_TRUE(WriteMessage(pair.client(), malformed.data(), messageBytes, kWaitMilliseconds));
    CHECK_TRUE(runner.WaitReturned(std::chrono::milliseconds(kWaitMilliseconds)));
    runner.Join();
    CHECK_TRUE(pump.Pending().closing);
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestDestructorWhileRunPending()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(4096, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };

    auto pump = std::make_unique<WorkerDuplexPump>(pair.server(), kGeneration, stopEvent, std::move(callbacks), 8,
                                                   std::move(hooks));
    pumpPointer = pump.get();
    std::thread runner([runPump = pump.get()]() { runPump->Run(); });
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));

    // The destructor must request stop, wait for Run to drain both OVERLAPPED operations, and only
    // then release the pump's events/buffers. onDrained still observes a live object here.
    pump.reset();
    runner.join();
    CheckDrained(observation);
    CloseHandle(stopEvent);
}

void TestPeerDisconnectDrains()
{
    PipePair pair;
    CHECK_TRUE(pair.Open(4096, 4096));

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_TRUE(stopEvent != nullptr);

    Observation observation;
    WorkerDuplexPump *pumpPointer = nullptr;
    WorkerDuplexPump::Callbacks callbacks;
    callbacks.onDrained = [&observation, &pumpPointer]() { observation.OnDrained(pumpPointer); };
    WorkerDuplexPump::ObservationHooks hooks;
    hooks.onReadPending = [&observation]() { observation.OnReadPending(); };

    WorkerDuplexPump pump(pair.server(), kGeneration, stopEvent, std::move(callbacks), 8, std::move(hooks));
    pumpPointer = &pump;
    PumpThread runner(pump);
    CHECK_TRUE(observation.WaitReadPending(1, std::chrono::milliseconds(kWaitMilliseconds)));

    pair.CloseClient();
    CHECK_TRUE(runner.WaitReturned(std::chrono::milliseconds(kWaitMilliseconds)));
    runner.Join();
    CHECK_TRUE(pump.Pending().closing);
    CheckDrained(observation);
    CloseHandle(stopEvent);
}
} // namespace

int main()
{
    TestReadPendingThenAckReachesPeer();
    TestStaleGenerationIsRejectedWithoutClosing();
    TestWritePendingDoesNotBlockRead();
    TestQueueFullFailsAndCloses();
    TestMessageSizeTerminates(3);
    TestMessageSizeTerminates(405);
    TestDestructorWhileRunPending();
    TestPeerDisconnectDrains();
    std::puts("worker_duplex_pump: all checks passed");
    return 0;
}
