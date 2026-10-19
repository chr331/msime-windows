#pragma once

#include <cstdint>
#include <deque>

namespace VoiceInput
{
namespace ControlPolicy
{
enum class Command
{
    Start,
    Stop,
    Toggle,
    Lock,
    Cancel,
    Exit
};

struct Request
{
    Command command;
    // Non-zero only for a UI cancel. The UI advances the voice session before
    // queueing it, so the cookie fences cleanup from a newer recording.
    std::uint64_t session_cookie = 0;
};

struct EnqueueResult
{
    bool enqueued = false;
    bool abort_start = false;
};

inline bool IsSessionCancel(const Request &request)
{
    return request.command == Command::Cancel && request.session_cookie != 0;
}

// Only a plain Cancel participates in Stop/Cancel dedupe. A session-cookie
// cancel belongs to an older recording and must not swallow a later Stop or
// Cancel for the recording that is currently active.
inline bool IsPlainCancel(const Request &request)
{
    return request.command == Command::Cancel && request.session_cookie == 0;
}

// Serializes control intent independently of the worker thread. A session
// cancel is deliberately never merged: it must retain its UI-issued cookie even
// when a Stop is already queued. Ordinary Stop keeps the idle dedupe, while an
// explicit Cancel upgrades a queued Stop and remains reachable in the idle
// recognition state.
inline EnqueueResult Enqueue(std::deque<Request> &pending, Request request, bool recording, bool starting)
{
    if (IsSessionCancel(request))
    {
        pending.push_back(request);
        return {true, false};
    }

    if (request.command == Command::Start)
    {
        if (!pending.empty() && pending.back().command == Command::Start)
            return {};
        pending.push_back(request);
        return {true, false};
    }

    if (request.command == Command::Stop)
    {
        const bool abort_start = starting;
        if (!pending.empty() && pending.back().command == Command::Start)
            pending.pop_back();
        if (!recording && !starting)
            return {false, false};
        if (!pending.empty() && (pending.back().command == Command::Stop || IsPlainCancel(pending.back())))
            return {false, abort_start};
        pending.push_back(request);
        return {true, abort_start};
    }

    if (request.command == Command::Cancel)
    {
        const bool abort_start = starting;
        if (!pending.empty() && pending.back().command == Command::Start)
            pending.pop_back();
        if (!pending.empty() && IsPlainCancel(pending.back()))
            return {false, abort_start};
        if (!pending.empty() && pending.back().command == Command::Stop)
        {
            pending.back() = request;
            return {true, abort_start};
        }
        pending.push_back(request);
        return {true, abort_start};
    }

    pending.push_back(request);
    return {true, false};
}

struct CancelSnapshot
{
    std::uint64_t session_cookie = 0;
    std::uint64_t owner_session = 0;
};

struct CancelCurrent
{
    std::uint64_t session_cookie = 0;
    std::uint64_t owner_session = 0;
    bool owner_matches = false;
    bool route_current = false;
};

// The UI has already fenced the session. Cleanup is allowed only while that
// cookie is still current and the owner snapshot captured before the UIA query
// still names the same client/epoch/identity on a live route.
inline bool SessionCancelStillValid(const CancelSnapshot &snapshot, const CancelCurrent &current)
{
    return snapshot.session_cookie != 0 && current.session_cookie == snapshot.session_cookie &&
           current.owner_session == snapshot.owner_session && current.owner_matches && current.route_current;
}
} // namespace ControlPolicy
} // namespace VoiceInput
