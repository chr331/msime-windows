#include "ipc_negotiation.h"
#include "voice_final_delivery.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(expression)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            std::cerr << #expression << " at " << __LINE__ << '\n';                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

namespace
{
namespace Vf = FanyImeVoiceFinal;
using Packet = std::array<std::uint8_t, Vf::kPacketBytes>;

constexpr std::uint64_t Delivery = 0x1020304050607080ULL;
constexpr std::uint64_t Focus = 0x8877665544332211ULL;
constexpr std::uint64_t Epoch = 17;
constexpr std::uint64_t Deadline = 0x00000000FFFFFFFFULL;

std::u16string MakeText(std::size_t units)
{
    std::u16string text(units, u'\0');
    for (std::size_t i = 0; i < units; ++i)
        text[i] = static_cast<char16_t>(0x4E00 + (i % 0x2000));
    return text;
}

bool DecodeAll(const std::vector<Packet> &packets, std::vector<Vf::Frame> &frames)
{
    frames.clear();
    for (const Packet &packet : packets)
    {
        Vf::Frame frame;
        if (Vf::DecodePacket(packet.data(), packet.size(), frame) != Vf::DecodeError::None)
            return false;
        frames.push_back(frame);
    }
    return true;
}

bool AssembleAll(const std::vector<Packet> &packets, std::u16string &text)
{
    Vf::Assembler assembler;
    for (const Packet &packet : packets)
    {
        Vf::Frame frame;
        if (Vf::DecodePacket(packet.data(), packet.size(), frame) != Vf::DecodeError::None)
            return false;
        std::u16string chunk;
        const auto result = assembler.Feed(frame, chunk);
        if (result == Vf::Assembler::Result::Error)
            return false;
        if (result == Vf::Assembler::Result::Complete)
        {
            text = chunk;
            return true;
        }
    }
    return false;
}

bool Encode(const std::u16string &text, std::vector<Packet> &packets, Vf::Operation operation = Vf::Operation::Insert)
{
    return Vf::EncodeTextFrames(Delivery, Focus, Epoch, Deadline, operation, text, packets);
}
} // namespace

int main()
{
    // Keep the existing worker-pipe ABI. The new opcode sits in msg_type and the
    // complete binary header/text occupies the 400-byte data array unchanged.
    CHECK(sizeof(FanyImeNamedpipeDataToTsfWorkerThread) == 404);
    CHECK(sizeof(FanyImeWireChar) * FanyImePipeLimits::CandidateTextCapacity == 400);
    CHECK(offsetof(FanyImeNamedpipeDataToTsfWorkerThread, msg_type) == 0);
    CHECK(offsetof(FanyImeNamedpipeDataToTsfWorkerThread, data) == 4);
    CHECK(FanyImeWorkerReplyType::HorizontalArrowSelectionChanged == 39);
    CHECK(FanyImeWorkerReplyType::VoiceFinal == 40);
    CHECK(FanyImeWorkerReplyType::MaxKnown == 40);
    CHECK(Vf::kPacketBytes == 404);
    CHECK(Vf::kPayloadBytes == 400);
    CHECK(Vf::kMaxTextUnits == 65536);

    // The capability is optional and is deliberately not advertised by the
    // default client or server configuration in this contract-only change.
    constexpr std::uint32_t ackCapability = FanyImeProtocol::VoiceDeliveryAck;
    CHECK(ackCapability == (1u << 9));
    CHECK((FanyImeProtocol::RequiredCapabilities & ackCapability) == 0);
    CHECK((FanyImeProtocol::Capabilities & ackCapability) == 0);
    CHECK((FanyImeProtocol::Hello(7, 1).modifiers_down & ackCapability) == 0);

    FanyImeNamedpipeData legacy{};
    legacy.event_type = FanyImePipeEventType::ClientHello;
    legacy.client_id = 7;
    const auto legacyResult = FanyImeProtocol::Negotiate(legacy, FanyImeProtocol::Capabilities | ackCapability);
    CHECK(legacyResult.accepted && legacyResult.legacy);
    CHECK((legacyResult.capabilities & ackCapability) == 0);

    const auto advertised = FanyImeProtocol::Capabilities | ackCapability;
    const auto ackHello = FanyImeProtocol::Hello(7, 2, advertised);
    const auto oldServer = FanyImeProtocol::Negotiate(ackHello);
    CHECK(oldServer.accepted && !oldServer.legacy);
    CHECK((oldServer.capabilities & ackCapability) == 0);
    const auto newServer = FanyImeProtocol::Negotiate(ackHello, advertised);
    CHECK(newServer.accepted && !newServer.legacy);
    CHECK((newServer.capabilities & ackCapability) != 0);
    const auto ackReply = FanyImeProtocol::Reply(ackHello, newServer);
    CHECK(FanyImeProtocol::AcceptReply(ackReply, 2));
    CHECK((FanyImeProtocol::ReplyCapabilities(ackReply) & ackCapability) != 0);
    CHECK((FanyImeProtocol::Negotiate(FanyImeProtocol::Hello(7, 3), advertised).capabilities & ackCapability) == 0);

    // Text longer than both historical inline limits is carried once, in full,
    // and reassembled without truncation or replay.
    const std::u16string text200 = MakeText(200);
    std::vector<Packet> frames200;
    CHECK(Encode(text200, frames200));
    CHECK(frames200.size() == 2);
    std::u16string assembled;
    CHECK(AssembleAll(frames200, assembled));
    CHECK(assembled == text200);

    const std::u16string text2049 = MakeText(2049);
    std::vector<Packet> frames2049;
    CHECK(Encode(text2049, frames2049));
    CHECK(frames2049.size() > 1);
    assembled.clear();
    CHECK(AssembleAll(frames2049, assembled));
    CHECK(assembled.size() == 2049 && assembled == text2049);

    const std::u16string maxText = MakeText(Vf::kMaxTextUnits);
    std::vector<Packet> maxFrames;
    CHECK(Encode(maxText, maxFrames));
    assembled.clear();
    CHECK(AssembleAll(maxFrames, assembled));
    CHECK(assembled.size() == Vf::kMaxTextUnits && assembled == maxText);

    std::vector<Packet> tooLarge;
    CHECK(!Encode(MakeText(Vf::kMaxTextUnits + 1), tooLarge));

    std::vector<Packet> emptyFrames;
    CHECK(Encode(std::u16string(), emptyFrames));
    CHECK(emptyFrames.size() == 1);
    assembled = u"stale";
    CHECK(AssembleAll(emptyFrames, assembled));
    CHECK(assembled.empty());

    // The four-byte opcode is not a candidate string. Embedded NULs are data and
    // the decoder never scans for a terminator.
    std::u16string withNuls = MakeText(300);
    withNuls[9] = 0;
    withNuls[167] = 0;
    withNuls[299] = 0;
    std::vector<Packet> nulFrames;
    CHECK(Encode(withNuls, nulFrames));
    assembled.clear();
    CHECK(AssembleAll(nulFrames, assembled));
    CHECK(assembled.size() == withNuls.size() && assembled == withNuls);

    std::vector<Packet> commitFrames;
    CHECK(Encode(MakeText(20), commitFrames, Vf::Operation::CommitComposition));
    Vf::Frame commitFrame;
    CHECK(Vf::DecodePacket(commitFrames.front().data(), commitFrames.front().size(), commitFrame) ==
          Vf::DecodeError::None);
    CHECK(commitFrame.operation == Vf::Operation::CommitComposition);

    // Preview uses the same one-shot complete text framing as Insert. It round-trips through the
    // assembler, including full text longer than the historical limits and a surrogate pair that
    // straddles a frame boundary. Preview never uses Cancel framing and has no Applied ACK path.
    CHECK(Vf::IsOperation(Vf::Operation::Preview));
    CHECK(!Vf::IsOperation(static_cast<Vf::Operation>(99)));
    CHECK(static_cast<std::uint16_t>(Vf::Operation::Preview) == 4);
    {
        const std::u16string previewText = MakeText(20);
        std::vector<Packet> previewFrames;
        CHECK(Encode(previewText, previewFrames, Vf::Operation::Preview));
        Vf::Frame decodedPreview;
        CHECK(Vf::DecodePacket(previewFrames.front().data(), previewFrames.front().size(), decodedPreview) ==
              Vf::DecodeError::None);
        CHECK(decodedPreview.operation == Vf::Operation::Preview);
        std::u16string previewAssembled;
        CHECK(AssembleAll(previewFrames, previewAssembled));
        CHECK(previewAssembled == previewText);
    }
    {
        std::u16string previewLong = MakeText(2049);
        previewLong[Vf::kMaxPayloadUnits - 1] = static_cast<char16_t>(0xD83D);
        previewLong[Vf::kMaxPayloadUnits] = static_cast<char16_t>(0xDE00);
        std::vector<Packet> previewLongFrames;
        CHECK(Encode(previewLong, previewLongFrames, Vf::Operation::Preview));
        CHECK(previewLongFrames.size() > 1);
        std::u16string previewAssembled;
        CHECK(AssembleAll(previewLongFrames, previewAssembled));
        CHECK(previewAssembled.size() == 2049 && previewAssembled == previewLong);
    }

    // A surrogate pair may straddle a frame boundary. The assembler only judges
    // UTF-16 validity after the final frame has arrived.
    std::u16string splitSurrogate = MakeText(400);
    splitSurrogate[Vf::kMaxPayloadUnits - 1] = static_cast<char16_t>(0xD83D);
    splitSurrogate[Vf::kMaxPayloadUnits] = static_cast<char16_t>(0xDE00);
    std::vector<Packet> surrogateFrames;
    CHECK(Encode(splitSurrogate, surrogateFrames));
    std::vector<Vf::Frame> decodedSurrogate;
    CHECK(DecodeAll(surrogateFrames, decodedSurrogate));
    CHECK(decodedSurrogate.size() >= 2);
    CHECK(decodedSurrogate[0].payloadUnits == Vf::kMaxPayloadUnits);
    CHECK(decodedSurrogate[0].payload.back() == 0xD83D);
    CHECK(decodedSurrogate[1].payload.front() == 0xDE00);
    assembled.clear();
    CHECK(AssembleAll(surrogateFrames, assembled));
    CHECK(assembled == splitSurrogate);

    std::u16string loneHigh = MakeText(Vf::kMaxPayloadUnits - 1);
    loneHigh.push_back(static_cast<char16_t>(0xD83D));
    std::vector<Packet> loneHighFrames;
    CHECK(Encode(loneHigh, loneHighFrames));
    Vf::Frame loneHighFrame;
    CHECK(Vf::DecodePacket(loneHighFrames.front().data(), loneHighFrames.front().size(), loneHighFrame) ==
          Vf::DecodeError::None);
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(loneHighFrame, complete) == Vf::Assembler::Result::Error);
        CHECK(complete.empty());
    }

    const std::u16string baseText = MakeText(400);
    std::vector<Packet> baseFrames;
    CHECK(Encode(baseText, baseFrames));
    std::vector<Vf::Frame> baseDecoded;
    CHECK(DecodeAll(baseFrames, baseDecoded));
    CHECK(baseDecoded.size() == 3);

    // Preview and final frames cannot be mixed inside one assembled delivery. The operation is
    // part of the assembler's lock-step metadata in both directions.
    {
        std::vector<Packet> previewBaseFrames;
        CHECK(Encode(baseText, previewBaseFrames, Vf::Operation::Preview));
        std::vector<Vf::Frame> previewBaseDecoded;
        CHECK(DecodeAll(previewBaseFrames, previewBaseDecoded));
        CHECK(previewBaseDecoded.size() == baseDecoded.size());
        {
            Vf::Assembler assembler;
            std::u16string complete;
            CHECK(assembler.Feed(previewBaseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
            CHECK(assembler.Feed(baseDecoded[1], complete) == Vf::Assembler::Result::Error);
            CHECK(complete.empty());
        }
        {
            Vf::Assembler assembler;
            std::u16string complete;
            CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
            CHECK(assembler.Feed(previewBaseDecoded[1], complete) == Vf::Assembler::Result::Error);
            CHECK(complete.empty());
        }
    }

    // Raw codec damage: every check runs before a frame can reach the assembler.
    Vf::Frame scratch;
    {
        Packet damaged = baseFrames[0];
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size() - 1, scratch) == Vf::DecodeError::WrongPacketSize);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU32(damaged.data(), Vf::kOpcode + 1);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::WrongOpcode);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU32(damaged.data() + 4 + Vf::kMagicOffset, Vf::kMagic ^ 1u);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadMagic);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kVersionOffset, Vf::kVersion + 1);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadVersion);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kHeaderBytesOffset, static_cast<std::uint16_t>(Vf::kHeaderBytes - 1));
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadHeaderSize);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kFlagsOffset, static_cast<std::uint16_t>(1u << 7));
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadFlags);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kOperationOffset, 99);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadOperation);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU64(damaged.data() + 4 + Vf::kDeliveryIdOffset, 0);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::ZeroIdentity);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kReserved0Offset, 1);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadReserved);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU32(damaged.data() + 4 + Vf::kReserved1Offset, 1);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadReserved);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU32(damaged.data() + 4 + Vf::kTotalUnitsOffset, Vf::kMaxTextUnits + 1);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::TextTooLarge);
    }
    {
        Packet damaged = baseFrames[0];
        // Offset declared past the total length.
        Vf::StoreU32(damaged.data() + 4 + Vf::kOffsetUnitsOffset, 401);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadOffset);
    }
    {
        Packet damaged = baseFrames[0];
        Vf::StoreU16(damaged.data() + 4 + Vf::kPayloadUnitsOffset,
                     static_cast<std::uint16_t>(Vf::kMaxPayloadUnits + 1));
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::PayloadTooLarge);
    }
    {
        Packet damaged = baseFrames[0];
        // 390 + 168 spills past the declared 400-unit total.
        Vf::StoreU32(damaged.data() + 4 + Vf::kOffsetUnitsOffset, 390);
        Vf::StoreU16(damaged.data() + 4 + Vf::kFlagsOffset, 0);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadCount);
    }
    {
        Packet damaged = baseFrames[1];
        // The middle frame claims to be final before the declared total arrives.
        Vf::StoreU16(damaged.data() + 4 + Vf::kFlagsOffset, Vf::kFlagLast);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadLastFrame);
    }
    {
        Packet damaged = baseFrames[1];
        Vf::StoreU16(damaged.data() + 4 + Vf::kFlagsOffset, Vf::kFlagFirst);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadFlags);
    }

    std::vector<Packet> shortFrames;
    CHECK(Encode(MakeText(10), shortFrames));
    {
        Packet damaged = shortFrames.front();
        damaged[4 + Vf::kTextOffset + 10 * sizeof(std::uint16_t)] = 1;
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadReserved);
    }

    // Cancel is a single, exact delivery identity. It cannot accidentally target
    // another in-flight delivery and it is never accepted by the text assembler.
    Packet cancelA{};
    Packet cancelB{};
    CHECK(Vf::EncodeCancelFrame(Delivery, Focus, Epoch, Deadline, cancelA));
    CHECK(Vf::EncodeCancelFrame(Delivery + 1, Focus, Epoch, Deadline, cancelB));
    Vf::Frame decodedCancelA;
    Vf::Frame decodedCancelB;
    CHECK(Vf::DecodePacket(cancelA.data(), cancelA.size(), decodedCancelA) == Vf::DecodeError::None);
    CHECK(Vf::DecodePacket(cancelB.data(), cancelB.size(), decodedCancelB) == Vf::DecodeError::None);
    CHECK(decodedCancelA.operation == Vf::Operation::Cancel);
    CHECK(decodedCancelA.deliveryId == Delivery);
    CHECK(decodedCancelB.deliveryId == Delivery + 1);
    CHECK(decodedCancelA.deliveryId != decodedCancelB.deliveryId);
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(decodedCancelA, complete) == Vf::Assembler::Result::Error);
    }
    {
        Packet damaged = cancelA;
        Vf::StoreU16(damaged.data() + 4 + Vf::kFlagsOffset, Vf::kFlagFirst);
        CHECK(Vf::DecodePacket(damaged.data(), damaged.size(), scratch) == Vf::DecodeError::BadCancelFrame);
    }

    // Assembler state must reject out-of-order, duplicate, skipped and changed
    // frames before it emits any complete text.
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[2], complete) == Vf::Assembler::Result::Error);
    }
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[1], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[1], complete) == Vf::Assembler::Result::Error);
    }
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[2], complete) == Vf::Assembler::Result::Error);
    }
    {
        Packet changedPacket = baseFrames[1];
        Vf::StoreU64(changedPacket.data() + 4 + Vf::kFocusTokenOffset, Focus + 1);
        Vf::Frame changed;
        CHECK(Vf::DecodePacket(changedPacket.data(), changedPacket.size(), changed) == Vf::DecodeError::None);
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(changed, complete) == Vf::Assembler::Result::Error);
    }
    {
        Packet changedPacket = baseFrames[1];
        Vf::StoreU32(changedPacket.data() + 4 + Vf::kTotalUnitsOffset, 401);
        Vf::Frame changed;
        CHECK(Vf::DecodePacket(changedPacket.data(), changedPacket.size(), changed) == Vf::DecodeError::None);
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(changed, complete) == Vf::Assembler::Result::Error);
    }
    {
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(baseDecoded[0], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[1], complete) == Vf::Assembler::Result::NeedMore);
        CHECK(assembler.Feed(baseDecoded[2], complete) == Vf::Assembler::Result::Complete);
        CHECK(complete == baseText);
        CHECK(assembler.Feed(baseDecoded[2], complete) == Vf::Assembler::Result::Error);
        CHECK(complete.empty());
    }
    {
        Vf::Frame overLimit;
        overLimit.first = true;
        overLimit.last = true;
        overLimit.totalUnits = Vf::kMaxTextUnits + 1;
        overLimit.payloadUnits = 1;
        overLimit.deliveryId = Delivery;
        overLimit.focusToken = Focus;
        overLimit.deadlineTick = Deadline;
        overLimit.payload.push_back(L'x');
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(overLimit, complete) == Vf::Assembler::Result::Error);
    }
    {
        Vf::Frame overflow;
        overflow.first = true;
        overflow.totalUnits = Vf::kMaxTextUnits;
        overflow.offsetUnits = Vf::kMaxTextUnits - 1;
        overflow.payloadUnits = 2;
        overflow.deliveryId = Delivery;
        overflow.focusToken = Focus;
        overflow.deadlineTick = Deadline;
        overflow.payload = {L'x', L'y'};
        Vf::Assembler assembler;
        std::u16string complete;
        CHECK(assembler.Feed(overflow, complete) == Vf::Assembler::Result::Error);
    }

    // A fixed ACK record needs no text layout. All three terminal states carry
    // both identities; malformed state, header, reserved and identity fields fail.
    for (const Vf::AckResult result :
         {Vf::AckResult::Applied, Vf::AckResult::RejectedNoWrite, Vf::AckResult::UnknownOrPartial})
    {
        Vf::Ack input;
        input.result = result;
        input.deliveryId = Delivery;
        input.focusToken = Focus;
        input.workerGeneration = 9;
        std::array<std::uint8_t, Vf::kAckBytes> bytes{};
        CHECK(Vf::EncodeAck(input, bytes));
        Vf::Ack output;
        CHECK(Vf::DecodeAck(bytes.data(), bytes.size(), output) == Vf::DecodeError::None);
        CHECK(output.result == result);
        CHECK(output.deliveryId == Delivery);
        CHECK(output.focusToken == Focus);
        CHECK(output.workerGeneration == 9);
    }
    {
        Vf::Ack bad;
        bad.result = static_cast<Vf::AckResult>(99);
        bad.deliveryId = Delivery;
        bad.focusToken = Focus;
        std::array<std::uint8_t, Vf::kAckBytes> bytes{};
        CHECK(!Vf::EncodeAck(bad, bytes));
        bad.result = Vf::AckResult::Applied;
        bad.deliveryId = 0;
        CHECK(!Vf::EncodeAck(bad, bytes));

        Vf::Ack good;
        good.result = Vf::AckResult::Applied;
        good.deliveryId = Delivery;
        good.focusToken = Focus;
        good.workerGeneration = 9;
        CHECK(Vf::EncodeAck(good, bytes));
        Vf::Ack output;
        CHECK(Vf::DecodeAck(bytes.data(), bytes.size() - 1, output) == Vf::DecodeError::BadAckSize);
        std::array<std::uint8_t, Vf::kAckBytes> damaged = bytes;
        Vf::StoreU32(damaged.data() + Vf::kAckMagicOffset, Vf::kAckMagic ^ 1u);
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::BadAckMagic);
        damaged = bytes;
        Vf::StoreU16(damaged.data() + Vf::kAckVersionOffset, Vf::kAckVersion + 1);
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::BadAckVersion);
        damaged = bytes;
        Vf::StoreU16(damaged.data() + Vf::kAckSizeOffset, static_cast<std::uint16_t>(Vf::kAckBytes - 1));
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::BadAckSize);
        damaged = bytes;
        Vf::StoreU16(damaged.data() + Vf::kAckResultOffset, 99);
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::BadAckResult);
        damaged = bytes;
        Vf::StoreU16(damaged.data() + Vf::kAckReserved0Offset, 1);
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::BadAckReserved);
        damaged = bytes;
        Vf::StoreU64(damaged.data() + Vf::kAckDeliveryIdOffset, 0);
        CHECK(Vf::DecodeAck(damaged.data(), damaged.size(), output) == Vf::DecodeError::ZeroIdentity);
    }

    // Decode the same bytes through the production 404-byte worker object too.
    FanyImeNamedpipeDataToTsfWorkerThread wire{};
    std::memcpy(&wire, baseFrames.front().data(), sizeof(wire));
    Vf::Frame wireFrame;
    CHECK(Vf::DecodePacket(wire, wireFrame) == Vf::DecodeError::None);
    CHECK(wire.msg_type == FanyImeWorkerReplyType::VoiceFinal);
    CHECK(wireFrame.deliveryId == Delivery && wireFrame.offsetUnits == 0);

    std::cout << "Voice final delivery contract passed\n";
    return 0;
}
