#pragma once

#include "WorkerDuplexPump.h"
#include <deque>
#include <map>
#include <memory>

// Transport-side ownership contains no COM pointers. The UI serializes edit sessions per
// owner; sealing is published immediately, even while its message queue is blocked.
class VoiceDeliveryRegistry final
{
  public:
    using Sender = std::function<bool(const WorkerDuplexAck &)>;
    struct Owner
    {
        const std::uint64_t generation, id, focus, initialEpoch;
        std::atomic<bool> sealed{false}, cancelled{false}, failed{false}, finalApplying{false}, finalUnknown{false};
        std::atomic<bool> acknowledged{false};
        Sender sender;
        std::uint64_t revision = 0; // registry lock only
        FanyImeVoiceFinal::Assembler assembler;
        bool assembling = false;
        FanyImeVoiceFinal::Operation assemblingOperation = FanyImeVoiceFinal::Operation::Insert;
        Owner(std::uint64_t gen, const FanyImeVoiceFinal::Frame &frame, std::uint64_t epoch, Sender send)
            : generation(gen), id(frame.deliveryId), focus(frame.focusToken), initialEpoch(epoch),
              sender(std::move(send))
        {
        }
        void Acknowledge(FanyImeVoiceFinal::AckResult result)
        {
            if (acknowledged.exchange(true))
                return;
            WorkerDuplexAck bytes{};
            if (FanyImeVoiceFinal::EncodeAck({result, id, focus, generation}, bytes) && sender)
                sender(bytes);
        }
    };
    struct Work
    {
        std::shared_ptr<Owner> owner;
        FanyImeVoiceFinal::Operation operation = FanyImeVoiceFinal::Operation::Cancel;
        std::uint64_t revision = 0, deadline = 0;
        std::u16string text;
        bool completion = false;
        FanyImeVoiceFinal::AckResult result = FanyImeVoiceFinal::AckResult::RejectedNoWrite;
    };

    explicit VoiceDeliveryRegistry(std::function<bool()> notify) : notify_(std::move(notify))
    {
    }
    bool Receive(const FanyImeVoiceFinal::Frame &frame, std::uint64_t generation, std::uint64_t initialEpoch,
                 Sender sender);
    void Retire(std::uint64_t generation = 0);
    void Complete(Work work, FanyImeVoiceFinal::AckResult result);
    std::deque<Work> Take();

  private:
    bool Notify();
    std::function<bool()> notify_;
    std::mutex mutex_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<Owner>> owners_;
    std::deque<Work> work_;
};
