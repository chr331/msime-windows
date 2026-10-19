#include "ipc/pipe_endpoint.h"
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

void Require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

int main()
{
    HANDLE client = INVALID_HANDLE_VALUE;
    try
    {
        const auto name = L"\\\\.\\pipe\\msime-endpoint-lock-" + std::to_wstring(GetCurrentProcessId());
        const HANDLE server =
            CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
                             1, 4096, 4096, 0, nullptr);
        Require(server != INVALID_HANDLE_VALUE, "create message pipe");
        FanyImeIpc::PipeEndpoint endpoint(server);
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(client != INVALID_HANDLE_VALUE, "connect message pipe");
        ConnectNamedPipe(server, nullptr);
        std::mutex route_mutex;
        bool current = true;
        unsigned calls = 0;
        std::array<unsigned char, 404> frame{};
        DWORD bytes = 0, error = 0;
        Require(endpoint.WithCurrentRoute(
                    route_mutex, [&] { return current; },
                    [&] {
                        ++calls;
                        return endpoint.WriteLocked(frame.data(), DWORD(frame.size()), bytes, error);
                    }),
                "ordinary current-route frame");
        std::array<unsigned char, 404> received{};
        Require(ReadFile(client, received.data(), DWORD(received.size()), &bytes, nullptr) && bytes == frame.size(),
                "one complete ordinary frame");
        // Fill the real PIPE_NOWAIT output buffer without a reader.
        for (unsigned i = 0; i < 10000; ++i)
        {
            if (!endpoint.Write(frame.data(), DWORD(frame.size()), bytes, error))
                break;
            Require(i != 9999, "bounded pipe backpressure");
        }
        const auto deadline = GetTickCount64() + 1500;
        auto batch = std::async(std::launch::async, [&] {
            return endpoint.WriteBatch(std::vector<std::array<unsigned char, 404>>{frame}, deadline);
        });
        Sleep(30);
        Require(batch.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "real batch is blocked by full pipe");
        std::promise<void> queued;
        auto ordinary = std::async(std::launch::async, [&] {
            queued.set_value();
            return endpoint.WithCurrentRoute(
                route_mutex, [&] { return current; },
                [&] {
                    ++calls;
                    return endpoint.WriteLocked(frame.data(), DWORD(frame.size()), bytes, error);
                });
        });
        queued.get_future().wait();
        Require(ordinary.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
                "ordinary writer waits for batch");
        auto cancel = std::async(std::launch::async, [&] {
            std::lock_guard lock(route_mutex);
            current = false; // Route lookup / UI cancellation can advance while batch is blocked.
        });
        const bool responsive = cancel.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
        endpoint.Shutdown();
        const auto result = batch.get();
        cancel.get();
        const bool stale_write = ordinary.get();
        Require(responsive, "route lock remains available during endpoint backpressure");
        Require(!result.complete, "shutdown ends incomplete batch");
        Require(!stale_write && calls == 1, "route revalidation suppresses queued stale writer");
        CloseHandle(client);
        client = INVALID_HANDLE_VALUE;
        std::cout << "pipe endpoint backpressure, route responsiveness, stale writer: passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        if (client != INVALID_HANDLE_VALUE)
            CloseHandle(client);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
