#include "IPC/VoiceDeliveryRegistry.h"
#undef NDEBUG
#include <cassert>
#include <iostream>

using namespace FanyImeVoiceFinal;

static Frame FrameFor(Operation operation, std::uint64_t id, std::uint64_t revision)
{
    std::vector<std::array<std::uint8_t, kPacketBytes>> packets;
    if (operation == Operation::Cancel)
    {
        std::array<std::uint8_t, kPacketBytes> packet{};
        assert(EncodeCancelFrame(id, 7, revision, 100000, packet));
        packets.push_back(packet);
    }
    else
        assert(EncodeTextFrames(id, 7, revision, 100000, operation, u"hello", packets));
    Frame result;
    assert(DecodePacket(packets.front().data(), packets.front().size(), result) == DecodeError::None);
    return result;
}

int main()
{
    int notifications = 0, acknowledgements = 0;
    VoiceDeliveryRegistry *raw = nullptr;
    VoiceDeliveryRegistry registry([&] {
        ++notifications;
        if (raw)
            raw->Take();
        return true;
    });
    // A notifier is allowed to synchronously take work: no registry lock crosses it.
    raw = &registry;
    auto sender = [&](const WorkerDuplexAck &bytes) {
        Ack ack;
        assert(DecodeAck(bytes.data(), bytes.size(), ack) == DecodeError::None);
        assert(ack.workerGeneration == 3 && ack.focusToken == 7);
        ++acknowledgements;
        registry.Take();
        return true;
    };
    assert(registry.Receive(FrameFor(Operation::Preview, 1, 1), 3, 11, sender));
    assert(notifications == 1);
    raw = nullptr;
    assert(registry.Receive(FrameFor(Operation::Preview, 1, 2), 3, 11, sender));
    auto work = registry.Take();
    auto owner = work.front().owner;
    assert(work.size() == 1 && owner->revision == 2 && !owner->sealed);
    assert(registry.Receive(FrameFor(Operation::CommitComposition, 1, 2), 3, 11, sender));
    auto final = registry.Take().front();
    assert(owner->sealed && final.revision == 2 && final.owner == owner);
    assert(!registry.Receive(FrameFor(Operation::Preview, 1, 3), 3, 11, sender));
    registry.Complete(work.front(), AckResult::RejectedNoWrite);
    assert(!owner->failed); // sealed queued update is skipped without poisoning the final
    registry.Complete(final, AckResult::Applied);
    registry.Complete(final, AckResult::UnknownOrPartial);
    assert(acknowledgements == 1 && owner->acknowledged);
    registry.Take();
    assert(!registry.Receive(FrameFor(Operation::Insert, 1, 0), 3, 11, sender));
    // Focus reset retains the seen ID until its transport is actually retired.
    registry.Retire();
    assert(owner->cancelled);
    assert(!registry.Receive(FrameFor(Operation::Preview, 1, 1), 3, 11, sender));
    registry.Retire(3);
    assert(registry.Receive(FrameFor(Operation::Insert, 1, 0), 4, 12, {}));
    registry.Take();
    // A final without a preview must be Insert, with revision zero.
    assert(!registry.Receive(FrameFor(Operation::CommitComposition, 2, 0), 4, 12, {}));
    assert(!registry.Receive(FrameFor(Operation::Preview, 3, 2), 4, 12, {}));
    assert(registry.Receive(FrameFor(Operation::Preview, 4, 1), 4, 12, {}));
    auto preview = registry.Take().back();
    assert(registry.Receive(FrameFor(Operation::Cancel, 4, 1), 4, 12, {}));
    assert(preview.owner->cancelled);
    assert(!registry.Receive(FrameFor(Operation::CommitComposition, 4, 1), 4, 12, {}));
    registry.Retire(4);
    registry.Take();
    // Completed IDs are bounded, never evicted while the same connection is alive.
    for (std::uint64_t id = 100; id < 164; ++id)
        assert(registry.Receive(FrameFor(Operation::Insert, id, 0), 9, 12, {}));
    assert(!registry.Receive(FrameFor(Operation::Insert, 164, 0), 9, 12, {}));
    std::cout
        << "voice delivery registry: ownership, sealing, cancellation, once ACK, retirement and capacity passed\n";
}
