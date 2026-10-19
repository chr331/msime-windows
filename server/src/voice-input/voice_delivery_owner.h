#pragma once

#include "ipc/ipc.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace VoiceInput
{
// One recording owns one binary delivery. The service never holds its session
// mutex while calling Preview, Finish or Cleanup. Cancellation wakes the ACK
// waiter immediately; wire cleanup serializes only with this owner's batches.
class VoiceDeliveryOwner
{
  public:
    explicit VoiceDeliveryOwner(VoiceFinalIpc::Channel channel) : channel_(std::move(channel))
    {
    }

    bool Preview(const std::wstring &text)
    {
        std::lock_guard<std::mutex> lock(wire_mutex_);
        if (cancelled_ || sealed_ || text.empty() || text == last_preview_)
            return false;
        auto channel = FreshChannel();
        if (VoiceFinalIpc::SendPreview(channel, revision_ + 1, Utf16(text)) != VoiceFinalIpc::SendStatus::Complete)
            return false;
        ++revision_;
        last_preview_ = text;
        return true;
    }

    VoiceFinalServer::AckResult Finish(const std::wstring &text)
    {
        {
            std::lock_guard<std::mutex> lock(wire_mutex_);
            if (cancelled_ || sealed_)
                return VoiceFinalServer::AckResult::UnknownOrPartial;
            sealed_ = true;
            auto channel = FreshChannel();
            const auto operation =
                revision_ ? FanyImeVoiceFinal::Operation::CommitComposition : FanyImeVoiceFinal::Operation::Insert;
            if (VoiceFinalIpc::SendFinal(channel, revision_, operation, Utf16(text)) !=
                VoiceFinalIpc::SendStatus::Complete)
                channel_.pending->Complete(VoiceFinalServer::AckResult::RejectedNoWrite);
        }
        // No routing, session, owner-wire or endpoint lock is held during ACK wait.
        const auto result = channel_.pending->WaitFor(std::chrono::milliseconds(1500));
        VoiceFinalIpc::Abandon(channel_);
        return result;
    }

    void Cancel()
    {
        cancelled_ = true;
        VoiceFinalIpc::Abandon(channel_);
    }

    void Cleanup()
    {
        Cancel();
        std::lock_guard<std::mutex> lock(wire_mutex_);
        if (channel_.pending->any_written())
            VoiceFinalIpc::SendCancel(FreshChannel());
    }

  private:
    static std::u16string Utf16(const std::wstring &text)
    {
        static_assert(sizeof(wchar_t) == sizeof(char16_t));
        return std::u16string(text.begin(), text.end());
    }
    VoiceFinalIpc::Channel FreshChannel() const
    {
        auto channel = channel_;
        channel.deadline_tick = GetTickCount64() + 1500;
        return channel;
    }

    const VoiceFinalIpc::Channel channel_;
    std::mutex wire_mutex_;
    std::atomic<bool> cancelled_{false};
    bool sealed_ = false;
    std::uint64_t revision_ = 0;
    std::wstring last_preview_;
};
} // namespace VoiceInput
