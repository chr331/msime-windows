#include "voice_final_pending.h"

#include <array>
#include <limits>
#include <utility>

namespace VoiceFinalServer
{
Pending::Pending(std::uint64_t connection_generation, std::uint64_t delivery_id, std::uint64_t focus_token,
                 std::uint64_t deadline_tick)
    : connection_generation(connection_generation), delivery_id(delivery_id), focus_token(focus_token),
      deadline_tick(deadline_tick)
{
}

bool Pending::MarkMayHaveWritten()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_)
        return false;
    any_written_ = true;
    return true;
}

bool Pending::any_written() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return any_written_;
}

AckResult Pending::Fallback(bool was_written) const
{
    return was_written ? AckResult::UnknownOrPartial : AckResult::RejectedNoWrite;
}

bool Pending::CompleteLocked(AckResult result)
{
    if (completed_)
        return false;
    result_ = IsAckResult(result) ? result : AckResult::UnknownOrPartial;
    if (result_ == AckResult::RejectedNoWrite && any_written_)
        result_ = AckResult::UnknownOrPartial;
    completed_ = true;
    cv_.notify_all();
    return true;
}

bool Pending::Complete(AckResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return CompleteLocked(result);
}

bool Pending::AcceptAck(AckResult result)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_ || !IsAckResult(result))
        return false;
    result_ = result;
    completed_ = true;
    cv_.notify_all();
    return true;
}

AckResult Pending::Wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return completed_; });
    return result_;
}

AckResult Pending::WaitFor(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return completed_; }))
    {
        CompleteLocked(Fallback(any_written_));
    }
    return result_;
}

AckResult Pending::result() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

bool Pending::completed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
}

bool Pending::PreparePreviewRevision(std::uint64_t revision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_ || final_sealed_ || revision == 0 || revision != last_preview_revision_ + 1)
        return false;
    last_preview_revision_ = revision;
    return true;
}

bool Pending::SealFinalRevision(std::uint64_t revision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_ || final_sealed_ || revision != last_preview_revision_)
        return false;
    final_sealed_ = true;
    return true;
}

std::uint64_t Pending::revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_preview_revision_;
}

PendingRegistry::PendingRegistry(std::uint64_t connection_generation) : connection_generation_(connection_generation)
{
}

std::uint64_t PendingRegistry::connection_generation() const
{
    return connection_generation_;
}

std::shared_ptr<Pending> PendingRegistry::Register(std::uint64_t delivery_id, std::uint64_t focus_token,
                                                   std::uint64_t deadline_tick)
{
    if (delivery_id == 0 || focus_token == 0 || deadline_tick == 0)
        return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || pending_.size() >= kMaxPending)
        return {};
    const Key key{delivery_id, focus_token};
    if (pending_.find(key) != pending_.end())
        return {};
    auto pending = std::make_shared<Pending>(connection_generation_, delivery_id, focus_token, deadline_tick);
    pending_.emplace(key, pending);
    return pending;
}

bool PendingRegistry::AcceptAck(const FanyImeVoiceFinal::Ack &ack)
{
    if (ack.deliveryId == 0 || ack.focusToken == 0 || ack.workerGeneration == 0 ||
        !FanyImeVoiceFinal::IsAckResult(ack.result))
    {
        return false;
    }

    std::shared_ptr<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return false;
        const auto found = pending_.find(Key{ack.deliveryId, ack.focusToken});
        if (found == pending_.end() || found->second->completed())
            return false;
        if (worker_generation_ == 0)
            worker_generation_ = ack.workerGeneration;
        if (worker_generation_ != ack.workerGeneration)
            return false;
        pending = std::move(found->second);
        pending_.erase(found);
    }
    return pending->AcceptAck(ack.result);
}

void PendingRegistry::Retire(std::uint64_t delivery_id, std::uint64_t focus_token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(Key{delivery_id, focus_token});
}

void PendingRegistry::CompleteConnection(AckResult fallback)
{
    std::unordered_map<Key, std::shared_ptr<Pending>, KeyHash> retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return;
        closed_ = true;
        retired.swap(pending_);
    }
    for (auto &entry : retired)
        entry.second->Complete(fallback);
}

bool PendingRegistry::closed() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t PendingRegistry::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

AckReadStatus ReadVoiceAckMessages(HANDLE pipe, PendingRegistry &registry, std::size_t &messages_read)
{
    if (!pipe || pipe == INVALID_HANDLE_VALUE)
        return AckReadStatus::Disconnected;

    AckReadStatus status = AckReadStatus::NoData;
    for (;;)
    {
        DWORD available = 0;
        DWORD next_message = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, &next_message))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE)
                return AckReadStatus::Disconnected;
            return AckReadStatus::ProtocolError;
        }
        if (available == 0)
            return status;
        if (next_message != FanyImeVoiceFinal::kAckBytes)
            return AckReadStatus::ProtocolError;

        std::array<std::uint8_t, FanyImeVoiceFinal::kAckBytes> bytes{};
        DWORD read = 0;
        if (!ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_DATA)
                return status;
            if (error == ERROR_MORE_DATA)
                return AckReadStatus::ProtocolError;
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE)
                return AckReadStatus::Disconnected;
            return AckReadStatus::ProtocolError;
        }
        if (read != bytes.size())
            return AckReadStatus::ProtocolError;

        FanyImeVoiceFinal::Ack ack;
        if (FanyImeVoiceFinal::DecodeAck(bytes.data(), bytes.size(), ack) != FanyImeVoiceFinal::DecodeError::None)
            return AckReadStatus::ProtocolError;
        registry.AcceptAck(ack);
        ++messages_read;
        status = AckReadStatus::Progress;
    }
}

std::uint64_t DefaultClock()
{
    return GetTickCount64();
}
} // namespace VoiceFinalServer
