#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │  Present policy for the X11 overlay.                                     │
// │                                                                          │
// │  The overlay presents strictly incrementally: many small damage rects    │
// │  per second (trail segments, fades, ttl expiries). On a hybrid-GPU       │
// │  "reverse PRIME" desktop the glass is a SinkOutput provider's            │
// │  dirty-region COPY of the rendered framebuffer, and small late rects     │
// │  are exactly what such copies drop or lag. The operator then sees        │
// │  STALE overlay content — goal boxes where things were, trails that       │
// │  never clear — while every readback, which stops at the source           │
// │  framebuffer, measures perfect alignment.                                │
// │                                                                          │
// │  The countermeasure is to present the FULL surface on any frame that     │
// │  had damage at all: the server-side dirty region is then the whole       │
// │  screen and any sink copy that lands self-heals every stale region.      │
// │  Idle frames still present nothing.                                      │
// │                                                                          │
// │  Selection: a PRIME sink is a topology fact (a SinkOutput provider       │
// │  alongside a SourceOutput provider, RandR 1.4); GRAB_OVERLAY_PRESENT     │
// │  overrides the detection in either direction. The parsing and the        │
// │  decision are pure so they are testable without a display.               │
// └──────────────────────────────────────────────────────────────────────────┘

#include <cstdint>
#include <string_view>

namespace grab::drivers::desktop::x11
{

    enum class PresentPolicy : std::uint8_t
    {
        Auto,           // full when a PRIME sink is present, else incremental
        Full,           // always present the whole surface on damaged frames
        Incremental,    // always present only the damage rects
    };

    // `GRAB_OVERLAY_PRESENT` value -> policy. Unknown or empty text is Auto:
    // an unrecognised override must not silently disable the safe default on
    // the machines that need it.
    [[nodiscard]]
    constexpr PresentPolicy
    parse_present_policy( std::string_view value ) noexcept
    {
        if( value == "full" )
        {
            return PresentPolicy::Full;
        }
        if( value == "incremental" )
        {
            return PresentPolicy::Incremental;
        }
        return PresentPolicy::Auto;
    }

    [[nodiscard]]
    constexpr bool
    full_present_selected( PresentPolicy policy,
                           bool          prime_sink_present ) noexcept
    {
        switch( policy )
        {
            case PresentPolicy::Full :
                return true;
            case PresentPolicy::Incremental :
                return false;
            case PresentPolicy::Auto :
                break;
        }
        return prime_sink_present;
    }

}    // namespace grab::drivers::desktop::x11
