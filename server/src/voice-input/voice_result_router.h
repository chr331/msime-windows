#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace VoiceInput
{
enum class RetainedResultSurface
{
    Card,
    OverlayRecovery,
    Unavailable,
};

enum class RetainedResultPresentation
{
    Shown,
    AlreadyShown,
    // The owning surface intentionally refused this generation, for example because the user
    // dismissed the same session or the call is stale. It must never be bypassed by another surface.
    Rejected,
    // The surface has no usable window.
    Unavailable,
    // A window exists but the UI-thread notification could not be posted.
    DeliveryFailed,
};

inline bool RetainedResultWasAccepted(RetainedResultPresentation presentation)
{
    return presentation == RetainedResultPresentation::Shown ||
           presentation == RetainedResultPresentation::AlreadyShown;
}

inline bool RetainedResultSurfaceLost(RetainedResultPresentation presentation)
{
    return presentation == RetainedResultPresentation::Unavailable ||
           presentation == RetainedResultPresentation::DeliveryFailed;
}

inline RetainedResultSurface SelectRetainedResultSurface(bool card_available, bool overlay_available)
{
    if (card_available)
        return RetainedResultSurface::Card;
    if (overlay_available)
        return RetainedResultSurface::OverlayRecovery;
    return RetainedResultSurface::Unavailable;
}

// Single production route for a final transcript that must stay available to the user. The card is
// authoritative when it is available: a model-level rejection (stale session, dismissed result,
// duplicate text) is final and never re-opens the result on the overlay. Only a lost card surface
// (missing window or failed UI notification) falls back to the already initialized overlay recovery
// mode. The returned value is true only when a surface accepted the complete text.
inline bool RouteRetainedResult(
    std::uint64_t session, const std::wstring &text, bool card_available, bool overlay_available,
    const std::function<RetainedResultPresentation(std::uint64_t, const std::wstring &)> &show_card,
    const std::function<bool(std::uint64_t, const std::wstring &)> &show_overlay_recovery)
{
    if (session == 0 || text.empty())
        return false;

    if (SelectRetainedResultSurface(card_available, overlay_available) == RetainedResultSurface::Card)
    {
        const RetainedResultPresentation card_result =
            show_card ? show_card(session, text) : RetainedResultPresentation::Unavailable;
        if (RetainedResultWasAccepted(card_result))
            return true;
        if (!RetainedResultSurfaceLost(card_result))
            return false;
    }
    return overlay_available && show_overlay_recovery && show_overlay_recovery(session, text);
}
} // namespace VoiceInput
