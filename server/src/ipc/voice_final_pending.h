#pragma once

#include "engine/contracts/voice_final_delivery.h"

#include <Windows.h>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

// Server-side completion state for binary voice-final deliveries. This unit is independent of the
// global pipe registry: the IPC layer captures one registry per worker-pipe connection, and the
// reverse-pipe monitor feeds ACKs back by connection generation. Waiting never holds a routing,
// endpoint or send lock.
namespace VoiceFinalServer
{
using AckResult = FanyImeVoiceFinal::AckResult;

struct Pending
{
    Pending(std::uint64_t connection_generation, std::uint64_t delivery_id, std::uint64_t focus_token,
            std::uint64_t deadline_tick);

    const std::uint64_t connection_generation;
    const std::uint64_t delivery_id;
    const std::uint64_t focus_token;
    const std::uint64_t deadline_tick;

    bool MarkMayHaveWritten();
    bool any_written() const;
    // Completes exactly once. Only Applied is success. Timeout/cancel/disconnect callers pass the
    // conservative fallback; no byte attempted is converted to RejectedNoWrite.
    bool Complete(AckResult result);
    bool AcceptAck(AckResult result);
    AckResult Wait();
    AckResult WaitFor(std::chrono::milliseconds timeout);
    AckResult result() const;
    bool completed() const;
    // The service serializes one owner's preview/final batches. These checks
    // keep the wire revision monotonic and make final seal the owner before any
    // frame is written.
    bool PreparePreviewRevision(std::uint64_t revision);
    bool SealFinalRevision(std::uint64_t revision);
    std::uint64_t revision() const;

  private:
    AckResult Fallback(bool was_written) const;
    bool CompleteLocked(AckResult result);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    AckResult result_ = AckResult::UnknownOrPartial;
    bool completed_ = false;
    bool any_written_ = false;
    bool final_sealed_ = false;
    std::uint64_t last_preview_revision_ = 0;
};

class PendingRegistry
{
  public:
    static constexpr std::size_t kMaxPending = 256;

    explicit PendingRegistry(std::uint64_t connection_generation);

    std::uint64_t connection_generation() const;
    // Registers before the first final frame is written. A duplicate owner/key, a closed registry,
    // or a full registry returns null; no frame may be sent in that case.
    std::shared_ptr<Pending> Register(std::uint64_t delivery_id, std::uint64_t focus_token,
                                      std::uint64_t deadline_tick);
    // Returns true only when the ACK matched this connection, the first non-zero DLL worker
    // generation, and a live owner. Duplicate/late/wrong-focus ACKs return false.
    bool AcceptAck(const FanyImeVoiceFinal::Ack &ack);
    void Retire(std::uint64_t delivery_id, std::uint64_t focus_token);
    // Retires every pending entry; late ACKs can no longer find them.
    void CompleteConnection(AckResult fallback = AckResult::RejectedNoWrite);
    bool closed() const;
    std::size_t size() const;

  private:
    struct Key
    {
        std::uint64_t delivery_id = 0;
        std::uint64_t focus_token = 0;

        bool operator==(const Key &other) const
        {
            return delivery_id == other.delivery_id && focus_token == other.focus_token;
        }
    };
    struct KeyHash
    {
        std::size_t operator()(const Key &key) const
        {
            const std::uint64_t mixed = key.delivery_id * 0x9E3779B185EBCA87ull ^ key.focus_token;
            return static_cast<std::size_t>(mixed ^ (mixed >> 32));
        }
    };

    const std::uint64_t connection_generation_;
    mutable std::mutex mutex_;
    std::unordered_map<Key, std::shared_ptr<Pending>, KeyHash> pending_;
    std::uint64_t worker_generation_ = 0;
    bool closed_ = false;
};

enum class AckReadStatus
{
    NoData,
    Progress,
    ProtocolError,
    Disconnected,
};

// Consumes every complete 48-byte ACK currently queued on a message-mode PIPE_NOWAIT worker
// connection. PeekNamedPipe inspects the next message rather than treating total bytes available
// as one frame, so two queued ACKs are read one at a time.
AckReadStatus ReadVoiceAckMessages(HANDLE pipe, PendingRegistry &registry, std::size_t &messages_read);

std::uint64_t DefaultClock();
} // namespace VoiceFinalServer
