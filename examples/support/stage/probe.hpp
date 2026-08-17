#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ladder::view::stage
{

    // ── What the display can do, and why not when it cannot ──────────────────
    //
    // Every rung probes what it needs BEFORE using it. The alternative is what
    // the previous harness did: attach an overlay, discard the failure, draw
    // into nothing, and report a clean run. A capability that cannot be
    // exercised is a FAILURE, never a skip.
    //
    // The reason string is grab's own, never a paraphrase. Three separate
    // investigations on this project went to the wrong layer because a refusal
    // named its outcome ("frontier empty", "SETTLE: TIMEOUT", "not reachable
    // inside the fence") and hid its cause.

    enum class Capability : std::uint8_t
    {
        Display,           // an X connection opens at all
        WindowManager,     // _NET_SUPPORTING_WM_CHECK has an owner
        Compositor,        // _NET_WM_CM_S<n> has an owner
        Overlay,           // Session::overlay() resolves
        Reactor,           // Session::start_observation() runs the frame clock
        XTest,             // a synthetic move round-trips through Input::position
        CaptureRegion,     // Screen::region() returns a frame
        CaptureWindow,     // Screen::window_by_class() returns a frame
        Watch,             // Session::watch() yields a subscription
        AtspiBus,          // the accessibility bus is reachable
        AtspiTree,         // a document node enumerates with > 0 nodes
        KeyboardChords,    // a Ctrl chord round-trips
        Clipboard,         // a selection read round-trips
        // A positioned primary-button press/release round-trips back through
        // grab's event stream. "Live" means an event pair was OBSERVED, not
        // that a subscription was created: a subscription that never delivers
        // is exactly the silent failure this whole file exists to refuse.
        ButtonEvents,
    };

    [[nodiscard]]
    constexpr std::string_view
    name_of( Capability capability ) noexcept
    {
        switch( capability )
        {
            case Capability::Display :
                return "display";
            case Capability::WindowManager :
                return "window_manager";
            case Capability::Compositor :
                return "compositor";
            case Capability::Overlay :
                return "overlay";
            case Capability::Reactor :
                return "reactor";
            case Capability::XTest :
                return "xtest";
            case Capability::CaptureRegion :
                return "capture_region";
            case Capability::CaptureWindow :
                return "capture_window";
            case Capability::Watch :
                return "watch";
            case Capability::AtspiBus :
                return "atspi_bus";
            case Capability::AtspiTree :
                return "atspi_tree";
            case Capability::KeyboardChords :
                return "keyboard_chords";
            case Capability::Clipboard :
                return "clipboard";
            case Capability::ButtonEvents :
                return "button_events";
        }
        return "unknown";
    }

    // A capability's verdict. `reason_` is meaningful only when !live_, and it
    // carries the underlying library's message verbatim.
    struct Verdict
    {
            Capability  capability_{};
            bool        live_ = false;
            std::string reason_;
    };

    // Every probed capability, and which of them a rung actually requires.
    //
    // Separating "probed" from "required" is what lets rung 1 report the whole
    // environment while still passing on a display that lacks, say, a clipboard
    // no rung on this machine will use. Diagnostic completeness is not the same
    // as success.
    class Report
    {
        public:

            void
            note( Verdict verdict )
            {
                verdicts_.push_back( std::move( verdict ) );
            }

            [[nodiscard]]
            const std::vector<Verdict>&
            verdicts() const noexcept
            {
                return verdicts_;
            }

            [[nodiscard]]
            bool
            live( Capability capability ) const noexcept
            {
                for( const Verdict& verdict : verdicts_ )
                {
                    if( verdict.capability_ == capability )
                    {
                        return verdict.live_;
                    }
                }
                return false;
            }

            [[nodiscard]]
            std::string
            reason( Capability capability ) const
            {
                for( const Verdict& verdict : verdicts_ )
                {
                    if( verdict.capability_ == capability )
                    {
                        return verdict.reason_;
                    }
                }
                return "not probed";
            }

            // True when every capability in `required` reported live.
            [[nodiscard]]
            bool
            satisfies( const std::vector<Capability>& required ) const noexcept
            {
                for( const Capability capability : required )
                {
                    if( !live( capability ) )
                    {
                        return false;
                    }
                }
                return true;
            }

        private:

            std::vector<Verdict> verdicts_;
    };

}    // namespace ladder::view::stage
