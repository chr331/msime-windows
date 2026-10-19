#include "ipc/voice_final_pending.h"
#include "voice-input/voice_delivery_owner.h"

#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace VoiceFinalServer;
using namespace FanyImeVoiceFinal;

namespace
{
void Require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

struct PipePair
{
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
    PipePair()
    {
        static unsigned sequence = 0;
        const auto name = L"\\\\.\\pipe\\msime-voice-ack-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(++sequence);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                  PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT, 1, 4096, 4096, 0, nullptr);
        Require(server != INVALID_HANDLE_VALUE, "create real message pipe");
        client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(client != INVALID_HANDLE_VALUE, "connect real message pipe");
        ConnectNamedPipe(server, nullptr);
    }
    ~PipePair()
    {
        if (client != INVALID_HANDLE_VALUE)
            CloseHandle(client);
        if (server != INVALID_HANDLE_VALUE)
            CloseHandle(server);
    }
    template <class T> void Write(const T &bytes)
    {
        DWORD written = 0;
        Require(WriteFile(client, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size(),
                "write real message");
    }
};

std::array<std::uint8_t, kAckBytes> AckBytes(AckResult result, std::uint64_t id, std::uint64_t focus,
                                             std::uint64_t generation)
{
    std::array<std::uint8_t, kAckBytes> bytes{};
    Require(EncodeAck({result, id, focus, generation}, bytes), "encode ACK");
    return bytes;
}

bool auto_ack = true;
AckResult next_ack = AckResult::Applied;
std::vector<std::pair<Operation, std::uint64_t>> sent;
std::promise<void> final_sent;
} // namespace

// Only the pipe adapter is replaced for the owner tests. The owner and pending
// registry are production code; real message framing is exercised separately.
VoiceFinalIpc::SendStatus VoiceFinalIpc::SendPreview(const Channel &channel, std::uint64_t revision,
                                                     const std::u16string &text)
{
    Require(!text.empty(), "preview text");
    if (!channel.pending->PreparePreviewRevision(revision))
        return SendStatus::NoWrite;
    channel.pending->MarkMayHaveWritten();
    sent.push_back({Operation::Preview, revision});
    return SendStatus::Complete;
}

VoiceFinalIpc::SendStatus VoiceFinalIpc::SendFinal(const Channel &channel, std::uint64_t revision, Operation operation,
                                                   const std::u16string &text)
{
    Require(text.size() > 4000, "complete final text, no truncation");
    if (!channel.pending->SealFinalRevision(revision))
        return SendStatus::NoWrite;
    channel.pending->MarkMayHaveWritten();
    sent.push_back({operation, revision});
    if (auto_ack)
        channel.registry->AcceptAck({next_ack, channel.delivery_id, channel.focus_token, 77});
    else
        final_sent.set_value();
    return SendStatus::Complete;
}

bool VoiceFinalIpc::SendCancel(const Channel &channel)
{
    Require(channel.pending->completed(), "cancel wakes before wire cleanup");
    sent.push_back({Operation::Cancel, channel.pending->revision()});
    return true;
}

void VoiceFinalIpc::Abandon(const Channel &channel)
{
    channel.pending->Complete(AckResult::RejectedNoWrite);
    channel.registry->Retire(channel.delivery_id, channel.focus_token);
}

int main()
{
    try
    {
        PendingRegistry registry(11);
        auto first = registry.Register(1, 2, DefaultClock() + 1000);
        first->MarkMayHaveWritten();
        Require(!registry.AcceptAck({AckResult::Applied, 999, 2, 666}), "unmatched ACK ignored");
        Require(!registry.AcceptAck({AckResult::Applied, 1, 3, 666}), "wrong focus cannot bind generation");
        Require(registry.AcceptAck({AckResult::RejectedNoWrite, 1, 2, 77}), "DLL NoWrite is authoritative");
        Require(first->Wait() == AckResult::RejectedNoWrite, "wire bytes are not document writes");
        Require(!registry.AcceptAck({AckResult::Applied, 1, 2, 77}), "duplicate ignored");
        auto second = registry.Register(2, 2, DefaultClock() + 1000);
        Require(!registry.AcceptAck({AckResult::Applied, 2, 2, 78}), "wrong generation ignored");
        Require(registry.AcceptAck({AckResult::Applied, 2, 2, 77}), "matching generation accepted");
        for (unsigned id = 3; id < 303; ++id)
        {
            auto pending = registry.Register(id, 2, DefaultClock() + 1000);
            Require(pending != nullptr, "completed/cancelled entries do not exhaust registry");
            pending->Complete(AckResult::RejectedNoWrite);
            registry.Retire(id, 2);
        }
        auto uncertain = registry.Register(400, 2, DefaultClock() + 1000);
        uncertain->MarkMayHaveWritten();
        registry.CompleteConnection();
        Require(uncertain->Wait() == AckResult::UnknownOrPartial, "disconnect after attempt is uncertain");
        Require(!registry.Register(401, 2, 1), "closed connection cannot register");

        PipePair pipe;
        PendingRegistry wire(12);
        auto a = wire.Register(1, 4, DefaultClock() + 1000);
        auto b = wire.Register(2, 4, DefaultClock() + 1000);
        pipe.Write(AckBytes(AckResult::Applied, 1, 4, 88));
        pipe.Write(AckBytes(AckResult::RejectedNoWrite, 2, 4, 88));
        std::size_t count = 0;
        Require(ReadVoiceAckMessages(pipe.server, wire, count) == AckReadStatus::Progress && count == 2,
                "two queued ACK messages are read independently");
        Require(a->result() == AckResult::Applied && b->result() == AckResult::RejectedNoWrite,
                "real-pipe ACKs correlate");
        std::array<std::uint8_t, 96> concatenated{};
        const auto ack = AckBytes(AckResult::Applied, 3, 4, 88);
        std::copy(ack.begin(), ack.end(), concatenated.begin());
        std::copy(ack.begin(), ack.end(), concatenated.begin() + kAckBytes);
        pipe.Write(concatenated);
        Require(ReadVoiceAckMessages(pipe.server, wire, count) == AckReadStatus::ProtocolError,
                "two ACKs in one message rejected");
        PipePair short_pipe;
        std::array<std::uint8_t, 47> short_ack{};
        short_pipe.Write(short_ack);
        Require(ReadVoiceAckMessages(short_pipe.server, wire, count) == AckReadStatus::ProtocolError,
                "short message rejected");

        VoiceFinalIpc::Channel channel;
        channel.registry = std::make_shared<PendingRegistry>(13);
        channel.pending = channel.registry->Register(1, 4, DefaultClock() + 1000);
        channel.delivery_id = 1;
        channel.focus_token = 4;
        VoiceInput::VoiceDeliveryOwner owner(channel);
        Require(owner.Preview(L"first"), "first preview");
        Require(!owner.Preview(L"first"), "duplicate preview skipped");
        Require(owner.Preview(L"second"), "second preview");
        Require(owner.Finish(std::wstring(4097, L'x')) == AckResult::Applied, "only final ACK means success");
        Require(!owner.Preview(L"late"), "final seals preview");
        Require(sent.back() == std::make_pair(Operation::CommitComposition, std::uint64_t(2)),
                "commit carries exact preview revision");

        channel.pending = channel.registry->Register(2, 4, DefaultClock() + 1000);
        channel.delivery_id = 2;
        VoiceInput::VoiceDeliveryOwner rejected(channel);
        next_ack = AckResult::RejectedNoWrite;
        Require(rejected.Finish(std::wstring(4097, L'y')) == AckResult::RejectedNoWrite,
                "owner preserves explicit NoWrite");
        rejected.Cleanup();
        Require(sent.back() == std::make_pair(Operation::Cancel, std::uint64_t(0)), "insert cleanup exact revision");

        channel.pending = channel.registry->Register(3, 4, DefaultClock() + 1000);
        channel.delivery_id = 3;
        VoiceInput::VoiceDeliveryOwner cancelled(channel);
        Require(cancelled.Preview(L"pending"), "cancelled owner preview");
        auto_ack = false;
        auto written = final_sent.get_future();
        auto result = std::async(std::launch::async, [&] { return cancelled.Finish(std::wstring(4097, L'z')); });
        written.wait();
        cancelled.Cancel();
        Require(result.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready,
                "cancel wakes final wait without endpoint/owner lock");
        Require(result.get() == AckResult::UnknownOrPartial, "cancel after write never claims NoWrite");
        cancelled.Cleanup();
        Require(sent.back() == std::make_pair(Operation::Cancel, std::uint64_t(1)), "cleanup uses preview revision");
        std::cout << "Pending registry, real message pipes and recording owner integration passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
