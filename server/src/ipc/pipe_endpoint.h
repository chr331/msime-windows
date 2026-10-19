#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "pipe_write_policy.h"

namespace FanyImeIpc
{
class PipeEndpoint
{
  public:
    explicit PipeEndpoint(HANDLE pipe) : pipe_(pipe)
    {
    }

    ~PipeEndpoint()
    {
        Shutdown();
        if (pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pipe_);
        }
    }

    PipeEndpoint(const PipeEndpoint &) = delete;
    PipeEndpoint &operator=(const PipeEndpoint &) = delete;

    HANDLE handle() const
    {
        return pipe_;
    }

    void Shutdown()
    {
        if (!shutting_down_.exchange(true) && pipe_ && pipe_ != INVALID_HANDLE_VALUE)
        {
            // Do not close here: a writer may still hold a shared reference. Disconnecting
            // wakes blocked pipe I/O, and the handle is closed exactly once by the destructor.
            DisconnectNamedPipe(pipe_);
        }
    }

    bool Write(const void *data, DWORD data_size, DWORD &bytes_written, DWORD &write_error)
    {
        std::lock_guard lock(write_mutex_);
        return WriteLocked(data, data_size, bytes_written, write_error);
    }

    // Worker writers acquire the endpoint before the route mutex. Batch backpressure
    // must never hold the global route lock while an ordinary notification waits.
    template <typename Validate, typename Write>
    bool WithCurrentRoute(std::mutex &route_mutex, Validate validate, Write write)
    {
        std::lock_guard endpoint_lock(write_mutex_);
        std::lock_guard route_lock(route_mutex);
        return validate() && write();
    }

    // Caller owns write_mutex_ through WithCurrentRoute.
    bool WriteLocked(const void *data, DWORD data_size, DWORD &bytes_written, DWORD &write_error)
    {
        bytes_written = 0;
        if (shutting_down_.load())
        {
            write_error = ERROR_PIPE_NOT_CONNECTED;
            return false;
        }

        const BOOL result = WriteFile(pipe_, data, data_size, &bytes_written, nullptr);
        if (!result)
        {
            write_error = GetLastError();
            return false;
        }
        if (!FanyImeIpc::IsCompleteMessageFrameWrite(true, bytes_written, data_size))
        {
            write_error = bytes_written == 0 ? ERROR_NO_DATA : ERROR_WRITE_FAULT;
            return false;
        }
        write_error = ERROR_SUCCESS;
        return true;
    }

    struct BatchResult
    {
        bool complete = false;
        bool any_bytes = false;
        std::size_t frames_written = 0;
        DWORD bytes_written = 0;
        DWORD error = ERROR_SUCCESS;
    };

    // Writes one complete message-mode batch while holding the endpoint write lock. A
    // PIPE_NOWAIT worker may accept a whole frame as TRUE/0 bytes when its buffer is
    // full; only that provably unwritten frame is retried, and only within its wire
    // deadline. A nonzero short write is uncertain and is never retried.
    template <typename Frame> BatchResult WriteBatch(const std::vector<Frame> &frames, std::uint64_t deadline_tick)
    {
        std::lock_guard lock(write_mutex_);
        BatchResult result;
        if (shutting_down_.load())
        {
            result.error = ERROR_PIPE_NOT_CONNECTED;
            return result;
        }
        for (const auto &frame : frames)
        {
            for (;;)
            {
                if (shutting_down_.load() || GetTickCount64() >= deadline_tick)
                {
                    result.error = shutting_down_.load() ? ERROR_PIPE_NOT_CONNECTED : ERROR_TIMEOUT;
                    return result;
                }
                DWORD written = 0;
                if (!WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr))
                {
                    result.bytes_written = written;
                    result.any_bytes = result.any_bytes || written != 0;
                    result.error = GetLastError();
                    return result;
                }
                if (written == frame.size())
                    break;
                result.bytes_written = written;
                if (written != 0)
                {
                    result.any_bytes = true;
                    result.error = ERROR_WRITE_FAULT;
                    return result;
                }
                if (GetTickCount64() >= deadline_tick)
                {
                    result.error = ERROR_TIMEOUT;
                    return result;
                }
                Sleep(1);
            }
            result.any_bytes = true;
            ++result.frames_written;
        }
        result.complete = true;
        return result;
    }

    HANDLE ReleaseHandle()
    {
        std::lock_guard lock(write_mutex_);
        shutting_down_.store(true);
        const HANDLE released = pipe_;
        pipe_ = INVALID_HANDLE_VALUE;
        return released;
    }

  private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::mutex write_mutex_;
    std::atomic_bool shutting_down_ = false;
};

} // namespace FanyImeIpc
