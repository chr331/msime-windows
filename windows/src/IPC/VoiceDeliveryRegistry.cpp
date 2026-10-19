#include "VoiceDeliveryRegistry.h"

using namespace FanyImeVoiceFinal;

bool VoiceDeliveryRegistry::Notify()
{
    // Never call into the message queue, transport, or an edit task with the registry locked.
    return !notify_ || notify_();
}

bool VoiceDeliveryRegistry::Receive(const Frame &frame, std::uint64_t generation, std::uint64_t initialEpoch,
                                    Sender sender)
{
    if (!generation || !ValidateFrame(frame))
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::make_pair(generation, frame.deliveryId);
        auto it = owners_.find(key);
        if (it == owners_.end())
        {
            // Keep completed IDs until disconnect; capacity closes the connection instead of
            // forgetting an ID and allowing a replay into the document.
            std::size_t count = 0;
            for (const auto &entry : owners_)
                if (entry.first.first == generation)
                    ++count;
            if (count >= 64 || !frame.first || frame.operation == Operation::Cancel)
                return false;
            it =
                owners_.emplace(key, std::make_shared<Owner>(generation, frame, initialEpoch, std::move(sender))).first;
        }
        auto owner = it->second;
        if (owner->focus != frame.focusToken)
            return false;
        if (frame.operation == Operation::Cancel)
        {
            if (frame.compositionEpoch != owner->revision)
                return false;
            owner->cancelled.store(true);
            owner->assembler.Reset();
            owner->assembling = false;
            work_.push_back({owner, Operation::Cancel, frame.compositionEpoch, frame.deadlineTick});
        }
        else
        {
            if (owner->cancelled || owner->failed)
                return false;
            if (frame.first)
            {
                if (owner->assembling || owner->sealed)
                    return false;
                if (frame.operation == Operation::Preview)
                {
                    if (frame.compositionEpoch != owner->revision + 1)
                        return false;
                    owner->revision = frame.compositionEpoch;
                }
                else
                {
                    if (frame.compositionEpoch != owner->revision ||
                        (frame.operation == Operation::Insert && owner->revision != 0) ||
                        (frame.operation == Operation::CommitComposition && owner->revision == 0))
                        return false;
                    owner->sealed.store(true);
                }
                owner->assembler.Reset();
                owner->assembling = true;
                owner->assemblingOperation = frame.operation;
            }
            if (!owner->assembling || owner->assemblingOperation != frame.operation)
                return false;
            std::u16string text;
            const auto result = owner->assembler.Feed(frame, text);
            if (result == Assembler::Result::Error)
            {
                owner->assembler.Reset();
                owner->failed = true;
                return false;
            }
            if (result == Assembler::Result::Complete)
            {
                owner->assembling = false;
                if (work_.size() >= 128)
                    return false;
                work_.push_back({owner, frame.operation, frame.compositionEpoch, frame.deadlineTick, std::move(text)});
            }
        }
    }
    return Notify();
}

void VoiceDeliveryRegistry::Retire(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = owners_.begin(); it != owners_.end();)
        {
            if (!generation || it->first.first == generation)
            {
                auto owner = it->second;
                owner->cancelled = true;
                work_.push_back({owner});
                if (generation)
                    it = owners_.erase(it);
                else
                    ++it;
            }
            else
                ++it;
        }
    }
    Notify();
}

void VoiceDeliveryRegistry::Complete(Work work, AckResult result)
{
    if (work.operation != Operation::Preview && work.operation != Operation::Cancel)
    {
        if (work.owner->acknowledged.load())
            return;
        if (result == AckResult::UnknownOrPartial)
            work.owner->finalUnknown = true;
        work.owner->Acknowledge(result);
    }
    else if (work.operation == Operation::Preview && result != AckResult::Applied &&
             !(result == AckResult::RejectedNoWrite && work.revision > 1 && work.owner->sealed))
        work.owner->failed = true;
    work.text.clear();
    work.completion = true;
    work.result = result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work_.push_back(std::move(work));
    }
    Notify();
}

std::deque<VoiceDeliveryRegistry::Work> VoiceDeliveryRegistry::Take()
{
    std::deque<Work> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.swap(work_);
    return result;
}
