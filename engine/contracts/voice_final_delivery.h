#pragma once

// Binary final voice delivery over the existing 404-byte worker pipe packet.
//
// The four-byte opcode is followed by exactly 400 bytes of fixed-width payload.
// Every field is written with an explicit-width memcpy: this protocol carries
// binary UTF-16 data, so it must never be parsed with wcslen or a NUL scan.
#include "windows_ipc.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace FanyImeVoiceFinal
{
constexpr std::size_t kPacketBytes = sizeof(FanyImeNamedpipeDataToTsfWorkerThread);
constexpr std::size_t kPayloadBytes = 400;
constexpr std::size_t kHeaderBytes = 64;
// Offset of the UTF-16 payload inside the 400-byte payload, after the opcode.
constexpr std::size_t kTextOffset = kHeaderBytes;
constexpr std::size_t kMaxPayloadUnits = (kPayloadBytes - kHeaderBytes) / sizeof(std::uint16_t);
constexpr std::uint32_t kMaxTextUnits = 65536;
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMagic = 0x31444656; // "VFD1"
constexpr std::uint32_t kOpcode = FanyImeWorkerReplyType::VoiceFinal;

static_assert(kPacketBytes == 404, "the worker pipe ABI is fixed at 404 bytes");
static_assert(kPayloadBytes == FanyImePipeLimits::CandidateTextCapacity * sizeof(FanyImeWireChar),
              "the final-delivery payload owns the complete worker data area");
static_assert(kTextOffset + kMaxPayloadUnits * sizeof(std::uint16_t) == kPayloadBytes,
              "the payload area must end exactly at the wire packet boundary");
static_assert(kOpcode == 40, "voice final is the first appended worker opcode");

constexpr std::uint16_t kFlagFirst = 1u << 0;
constexpr std::uint16_t kFlagLast = 1u << 1;
constexpr std::uint16_t kFlagMask = kFlagFirst | kFlagLast;

// Wire offsets in the 400-byte payload (the opcode lives at packet offset 0).
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kHeaderBytesOffset = 6;
constexpr std::size_t kFlagsOffset = 8;
constexpr std::size_t kOperationOffset = 10;
constexpr std::size_t kTotalUnitsOffset = 12;
constexpr std::size_t kOffsetUnitsOffset = 16;
constexpr std::size_t kPayloadUnitsOffset = 20;
constexpr std::size_t kReserved0Offset = 22;
constexpr std::size_t kDeliveryIdOffset = 24;
constexpr std::size_t kFocusTokenOffset = 32;
// The voice owner's wire preview revision. This is not the DLL's local TSF composition epoch:
// the Server assigns one deliveryId before the first Preview, every Preview increments this
// revision, and the final frame carries the last revision.
constexpr std::size_t kCompositionEpochOffset = 40;
constexpr std::size_t kDeadlineTickOffset = 48;
constexpr std::size_t kReserved1Offset = 56;
constexpr std::size_t kReserved2Offset = 60;
// Source-compatible name used by the first contract draft.
constexpr std::size_t kPayloadOffset = kTextOffset;

// ACK direction: one fixed binary record read by the Server from the reverse
// side of the worker pipe. It intentionally has no candidate-string layout.
constexpr std::size_t kAckBytes = 48;
constexpr std::uint32_t kAckMagic = 0x4B414656; // "VFAK"
constexpr std::uint16_t kAckVersion = 1;
constexpr std::size_t kAckMagicOffset = 0;
constexpr std::size_t kAckVersionOffset = 4;
constexpr std::size_t kAckSizeOffset = 6;
constexpr std::size_t kAckResultOffset = 8;
constexpr std::size_t kAckReserved0Offset = 10;
constexpr std::size_t kAckReserved1Offset = 12;
constexpr std::size_t kAckDeliveryIdOffset = 16;
constexpr std::size_t kAckFocusTokenOffset = 24;
constexpr std::size_t kAckWorkerGenerationOffset = 32;
constexpr std::size_t kAckReserved2Offset = 40;

enum class Operation : std::uint16_t
{
    Insert = 1,
    CommitComposition = 2,
    Cancel = 3,
    // Preview shares the complete UTF-16 frame/assembler path with Insert and carries the voice
    // owner's wire preview revision in compositionEpoch. It never produces a final Applied
    // acknowledgement. Adding the value does not change the packet ABI or protocol version.
    Preview = 4,
};

enum class AckResult : std::uint16_t
{
    Applied = 1,
    RejectedNoWrite = 2,
    UnknownOrPartial = 3,
};

enum class DecodeError : std::uint8_t
{
    None = 0,
    WrongPacketSize,
    WrongOpcode,
    BadMagic,
    BadVersion,
    BadHeaderSize,
    BadFlags,
    BadOperation,
    ZeroIdentity,
    BadReserved,
    TextTooLarge,
    BadOffset,
    PayloadTooLarge,
    BadCount,
    BadLastFrame,
    BadCancelFrame,
    BadAckMagic,
    BadAckVersion,
    BadAckSize,
    BadAckResult,
    BadAckReserved,
};

struct Frame
{
    bool first = false;
    bool last = false;
    Operation operation = Operation::Insert;
    std::uint32_t totalUnits = 0;
    std::uint32_t offsetUnits = 0;
    std::uint16_t payloadUnits = 0;
    std::uint64_t deliveryId = 0;
    std::uint64_t focusToken = 0;
    std::uint64_t compositionEpoch = 0;
    std::uint64_t deadlineTick = 0;
    std::vector<std::uint16_t> payload;
};

struct FrameSpec
{
    bool first = false;
    bool last = false;
    Operation operation = Operation::Insert;
    std::uint32_t totalUnits = 0;
    std::uint32_t offsetUnits = 0;
    std::uint64_t deliveryId = 0;
    std::uint64_t focusToken = 0;
    std::uint64_t compositionEpoch = 0;
    std::uint64_t deadlineTick = 0;
};

struct Ack
{
    AckResult result = AckResult::RejectedNoWrite;
    std::uint64_t deliveryId = 0;
    std::uint64_t focusToken = 0;
    std::uint64_t workerGeneration = 0;
};

inline void StoreU16(std::uint8_t *p, std::uint16_t value)
{
    std::memcpy(p, &value, sizeof(value));
}

inline void StoreU32(std::uint8_t *p, std::uint32_t value)
{
    std::memcpy(p, &value, sizeof(value));
}

inline void StoreU64(std::uint8_t *p, std::uint64_t value)
{
    std::memcpy(p, &value, sizeof(value));
}

inline std::uint16_t LoadU16(const std::uint8_t *p)
{
    std::uint16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline std::uint32_t LoadU32(const std::uint8_t *p)
{
    std::uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline std::uint64_t LoadU64(const std::uint8_t *p)
{
    std::uint64_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

inline bool AllZero(const std::uint8_t *p, std::size_t bytes)
{
    for (std::size_t i = 0; i < bytes; ++i)
    {
        if (p[i] != 0)
            return false;
    }
    return true;
}

inline bool IsOperation(Operation operation)
{
    return operation == Operation::Insert || operation == Operation::CommitComposition ||
           operation == Operation::Cancel || operation == Operation::Preview;
}

inline bool IsAckResult(AckResult result)
{
    return result == AckResult::Applied || result == AckResult::RejectedNoWrite ||
           result == AckResult::UnknownOrPartial;
}

inline bool IsValidUtf16(const std::u16string &text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const std::uint16_t unit = static_cast<std::uint16_t>(text[i]);
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (i + 1 >= text.size())
                return false;
            const std::uint16_t low = static_cast<std::uint16_t>(text[i + 1]);
            if (low < 0xDC00 || low > 0xDFFF)
                return false;
            ++i;
        }
        else if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            return false;
        }
    }
    return true;
}

inline bool ValidFrameShape(Operation operation, bool first, bool last, std::uint32_t totalUnits,
                            std::uint32_t offsetUnits, std::size_t payloadUnits)
{
    if (!IsOperation(operation) || totalUnits > kMaxTextUnits || payloadUnits > kMaxPayloadUnits)
        return false;
    if (offsetUnits > totalUnits)
        return false;
    const std::uint64_t end = static_cast<std::uint64_t>(offsetUnits) + payloadUnits;
    if (end > totalUnits || (first != (offsetUnits == 0)))
        return false;
    if (operation == Operation::Cancel)
    {
        return first && last && totalUnits == 0 && offsetUnits == 0 && payloadUnits == 0;
    }
    if (last != (end == totalUnits))
        return false;
    // Only a complete empty text may carry zero units.
    return totalUnits == 0 || payloadUnits != 0;
}

inline bool ValidateFrame(const Frame &frame)
{
    if (frame.deliveryId == 0 || frame.focusToken == 0 || frame.deadlineTick == 0)
        return false;
    return frame.payloadUnits == frame.payload.size() &&
           ValidFrameShape(frame.operation, frame.first, frame.last, frame.totalUnits, frame.offsetUnits,
                           frame.payload.size());
}

// Encode one frame into the exact 404-byte worker packet shape.
inline bool EncodeFrame(const FrameSpec &spec, const std::uint16_t *payload, std::size_t payloadUnits,
                        std::array<std::uint8_t, kPacketBytes> &packet)
{
    if (spec.deliveryId == 0 || spec.focusToken == 0 || spec.deadlineTick == 0)
        return false;
    if (payloadUnits != 0 && payload == nullptr)
        return false;
    if (!ValidFrameShape(spec.operation, spec.first, spec.last, spec.totalUnits, spec.offsetUnits, payloadUnits))
        return false;

    packet.fill(0);
    StoreU32(packet.data(), kOpcode);
    std::uint8_t *const payload_begin = packet.data() + sizeof(std::uint32_t);
    StoreU32(payload_begin + kMagicOffset, kMagic);
    StoreU16(payload_begin + kVersionOffset, kVersion);
    StoreU16(payload_begin + kHeaderBytesOffset, static_cast<std::uint16_t>(kHeaderBytes));
    std::uint16_t flags = 0;
    if (spec.first)
        flags |= kFlagFirst;
    if (spec.last)
        flags |= kFlagLast;
    StoreU16(payload_begin + kFlagsOffset, flags);
    StoreU16(payload_begin + kOperationOffset, static_cast<std::uint16_t>(spec.operation));
    StoreU32(payload_begin + kTotalUnitsOffset, spec.totalUnits);
    StoreU32(payload_begin + kOffsetUnitsOffset, spec.offsetUnits);
    StoreU16(payload_begin + kPayloadUnitsOffset, static_cast<std::uint16_t>(payloadUnits));
    // reserved0 at 22 is left zero.
    StoreU64(payload_begin + kDeliveryIdOffset, spec.deliveryId);
    StoreU64(payload_begin + kFocusTokenOffset, spec.focusToken);
    StoreU64(payload_begin + kCompositionEpochOffset, spec.compositionEpoch);
    StoreU64(payload_begin + kDeadlineTickOffset, spec.deadlineTick);
    // reserved1/reserved2 at 56/60 are left zero.
    if (payloadUnits != 0)
        std::memcpy(payload_begin + kTextOffset, payload, payloadUnits * sizeof(std::uint16_t));
    return true;
}

inline bool EncodeTextFrames(std::uint64_t deliveryId, std::uint64_t focusToken, std::uint64_t compositionEpoch,
                             std::uint64_t deadlineTick, Operation operation, const std::u16string &text,
                             std::vector<std::array<std::uint8_t, kPacketBytes>> &frames)
{
    frames.clear();
    if (deliveryId == 0 || focusToken == 0 || deadlineTick == 0 || operation == Operation::Cancel ||
        !IsOperation(operation) || text.size() > kMaxTextUnits)
    {
        return false;
    }
    std::vector<std::uint16_t> units;
    units.reserve(text.size());
    for (char16_t unit : text)
        units.push_back(static_cast<std::uint16_t>(unit));

    std::uint32_t offset = 0;
    const std::uint32_t total = static_cast<std::uint32_t>(units.size());
    do
    {
        const std::uint32_t remaining = total - offset;
        const std::uint32_t count = remaining < static_cast<std::uint32_t>(kMaxPayloadUnits)
                                        ? remaining
                                        : static_cast<std::uint32_t>(kMaxPayloadUnits);
        FrameSpec spec;
        spec.deliveryId = deliveryId;
        spec.focusToken = focusToken;
        spec.compositionEpoch = compositionEpoch;
        spec.deadlineTick = deadlineTick;
        spec.operation = operation;
        spec.totalUnits = total;
        spec.offsetUnits = offset;
        spec.first = offset == 0;
        spec.last = offset + count == total;
        std::array<std::uint8_t, kPacketBytes> packet{};
        if (!EncodeFrame(spec, count == 0 ? nullptr : units.data() + offset, count, packet))
        {
            frames.clear();
            return false;
        }
        frames.push_back(packet);
        offset += count;
    } while (offset < total);
    return true;
}

inline bool EncodeCancelFrame(std::uint64_t deliveryId, std::uint64_t focusToken, std::uint64_t compositionEpoch,
                              std::uint64_t deadlineTick, std::array<std::uint8_t, kPacketBytes> &packet)
{
    FrameSpec spec;
    spec.first = true;
    spec.last = true;
    spec.operation = Operation::Cancel;
    spec.deliveryId = deliveryId;
    spec.focusToken = focusToken;
    spec.compositionEpoch = compositionEpoch;
    spec.deadlineTick = deadlineTick;
    return EncodeFrame(spec, nullptr, 0, packet);
}

inline DecodeError DecodePacket(const std::uint8_t *packet, std::size_t packetBytes, Frame &frame)
{
    frame = Frame{};
    if (packet == nullptr || packetBytes != kPacketBytes)
        return DecodeError::WrongPacketSize;
    if (LoadU32(packet) != kOpcode)
        return DecodeError::WrongOpcode;
    const std::uint8_t *const payload_begin = packet + sizeof(std::uint32_t);
    if (LoadU32(payload_begin + kMagicOffset) != kMagic)
        return DecodeError::BadMagic;
    if (LoadU16(payload_begin + kVersionOffset) != kVersion)
        return DecodeError::BadVersion;
    if (LoadU16(payload_begin + kHeaderBytesOffset) != kHeaderBytes)
        return DecodeError::BadHeaderSize;
    const std::uint16_t flags = LoadU16(payload_begin + kFlagsOffset);
    if ((flags & ~kFlagMask) != 0)
        return DecodeError::BadFlags;
    const auto operation = static_cast<Operation>(LoadU16(payload_begin + kOperationOffset));
    if (!IsOperation(operation))
        return DecodeError::BadOperation;
    const std::uint64_t deliveryId = LoadU64(payload_begin + kDeliveryIdOffset);
    const std::uint64_t focusToken = LoadU64(payload_begin + kFocusTokenOffset);
    const std::uint64_t compositionEpoch = LoadU64(payload_begin + kCompositionEpochOffset);
    const std::uint64_t deadlineTick = LoadU64(payload_begin + kDeadlineTickOffset);
    if (deliveryId == 0 || focusToken == 0 || deadlineTick == 0)
        return DecodeError::ZeroIdentity;
    if (LoadU16(payload_begin + kReserved0Offset) != 0 || LoadU32(payload_begin + kReserved1Offset) != 0 ||
        LoadU32(payload_begin + kReserved2Offset) != 0)
    {
        return DecodeError::BadReserved;
    }

    const std::uint32_t totalUnits = LoadU32(payload_begin + kTotalUnitsOffset);
    const std::uint32_t offsetUnits = LoadU32(payload_begin + kOffsetUnitsOffset);
    const std::uint16_t payloadUnits = LoadU16(payload_begin + kPayloadUnitsOffset);
    if (totalUnits > kMaxTextUnits)
        return DecodeError::TextTooLarge;
    if (offsetUnits > totalUnits)
        return DecodeError::BadOffset;
    if (payloadUnits > kMaxPayloadUnits)
        return DecodeError::PayloadTooLarge;
    const std::uint64_t end = static_cast<std::uint64_t>(offsetUnits) + payloadUnits;
    if (end > totalUnits)
        return DecodeError::BadCount;

    const bool first = (flags & kFlagFirst) != 0;
    const bool last = (flags & kFlagLast) != 0;
    if (!ValidFrameShape(operation, first, last, totalUnits, offsetUnits, payloadUnits))
    {
        if (operation == Operation::Cancel)
            return DecodeError::BadCancelFrame;
        if (first != (offsetUnits == 0))
            return DecodeError::BadFlags;
        if (last != (end == totalUnits))
            return DecodeError::BadLastFrame;
        return DecodeError::BadCount;
    }
    // Unused bytes are reserved and must stay zero for this version.
    const std::size_t used = kTextOffset + static_cast<std::size_t>(payloadUnits) * sizeof(std::uint16_t);
    if (!AllZero(payload_begin + used, kPayloadBytes - used))
        return DecodeError::BadReserved;

    frame.first = first;
    frame.last = last;
    frame.operation = operation;
    frame.totalUnits = totalUnits;
    frame.offsetUnits = offsetUnits;
    frame.payloadUnits = payloadUnits;
    frame.deliveryId = deliveryId;
    frame.focusToken = focusToken;
    frame.compositionEpoch = compositionEpoch;
    frame.deadlineTick = deadlineTick;
    frame.payload.resize(payloadUnits);
    if (payloadUnits != 0)
        std::memcpy(frame.payload.data(), payload_begin + kTextOffset, payloadUnits * sizeof(std::uint16_t));
    return DecodeError::None;
}

inline DecodeError DecodePacket(const FanyImeNamedpipeDataToTsfWorkerThread &packet, Frame &frame)
{
    std::array<std::uint8_t, kPacketBytes> bytes{};
    std::memcpy(bytes.data(), &packet, sizeof(bytes));
    return DecodePacket(bytes.data(), bytes.size(), frame);
}

class Assembler
{
  public:
    enum class Result
    {
        NeedMore,
        Complete,
        Error,
    };

    Result Feed(const Frame &frame, std::u16string &complete)
    {
        complete.clear();
        if (completed_ || !ValidateFrame(frame) || frame.operation == Operation::Cancel)
            return Result::Error;
        if (!started_)
        {
            if (!frame.first || frame.offsetUnits != 0)
                return Result::Error;
            started_ = true;
            meta_ = frame;
            text_.clear();
        }
        else
        {
            if (frame.first || frame.offsetUnits != text_.size() || frame.deliveryId != meta_.deliveryId ||
                frame.focusToken != meta_.focusToken || frame.compositionEpoch != meta_.compositionEpoch ||
                frame.deadlineTick != meta_.deadlineTick || frame.operation != meta_.operation ||
                frame.totalUnits != meta_.totalUnits)
            {
                return Result::Error;
            }
        }
        if (text_.size() + frame.payload.size() > frame.totalUnits)
            return Result::Error;
        for (std::uint16_t unit : frame.payload)
            text_.push_back(static_cast<char16_t>(unit));
        if (frame.last)
        {
            if (text_.size() != frame.totalUnits || !IsValidUtf16(text_))
                return Result::Error;
            completed_ = true;
            complete = text_;
            return Result::Complete;
        }
        return Result::NeedMore;
    }

    void Reset()
    {
        started_ = false;
        completed_ = false;
        meta_ = Frame{};
        text_.clear();
    }

    bool started() const
    {
        return started_;
    }

    bool completed() const
    {
        return completed_;
    }

    const std::u16string &text() const
    {
        return text_;
    }

  private:
    bool started_ = false;
    bool completed_ = false;
    Frame meta_{};
    std::u16string text_;
};

inline bool EncodeAck(const Ack &ack, std::array<std::uint8_t, kAckBytes> &bytes)
{
    bytes.fill(0);
    if (!IsAckResult(ack.result) || ack.deliveryId == 0 || ack.focusToken == 0)
        return false;
    StoreU32(bytes.data() + kAckMagicOffset, kAckMagic);
    StoreU16(bytes.data() + kAckVersionOffset, kAckVersion);
    StoreU16(bytes.data() + kAckSizeOffset, static_cast<std::uint16_t>(kAckBytes));
    StoreU16(bytes.data() + kAckResultOffset, static_cast<std::uint16_t>(ack.result));
    StoreU64(bytes.data() + kAckDeliveryIdOffset, ack.deliveryId);
    StoreU64(bytes.data() + kAckFocusTokenOffset, ack.focusToken);
    StoreU64(bytes.data() + kAckWorkerGenerationOffset, ack.workerGeneration);
    // reserved0/reserved1/reserved2 stay zero.
    return true;
}

inline DecodeError DecodeAck(const std::uint8_t *bytes, std::size_t size, Ack &ack)
{
    ack = Ack{};
    if (bytes == nullptr || size != kAckBytes)
        return DecodeError::BadAckSize;
    if (LoadU32(bytes + kAckMagicOffset) != kAckMagic)
        return DecodeError::BadAckMagic;
    if (LoadU16(bytes + kAckVersionOffset) != kAckVersion)
        return DecodeError::BadAckVersion;
    if (LoadU16(bytes + kAckSizeOffset) != kAckBytes)
        return DecodeError::BadAckSize;
    if (LoadU16(bytes + kAckReserved0Offset) != 0 || LoadU32(bytes + kAckReserved1Offset) != 0 ||
        LoadU64(bytes + kAckReserved2Offset) != 0)
    {
        return DecodeError::BadAckReserved;
    }
    const auto result = static_cast<AckResult>(LoadU16(bytes + kAckResultOffset));
    if (!IsAckResult(result))
        return DecodeError::BadAckResult;
    ack.result = result;
    ack.deliveryId = LoadU64(bytes + kAckDeliveryIdOffset);
    ack.focusToken = LoadU64(bytes + kAckFocusTokenOffset);
    ack.workerGeneration = LoadU64(bytes + kAckWorkerGenerationOffset);
    if (ack.deliveryId == 0 || ack.focusToken == 0)
        return DecodeError::ZeroIdentity;
    return DecodeError::None;
}
} // namespace FanyImeVoiceFinal
