#pragma once

#include "support/stage/assert.hpp"
#include "support/truth.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ladder::view::stage
{

    // ── What a rung authors, and what it then observed ───────────────────────
    //
    // A scene is generated, never hand-written, for the same reason the proving
    // ground is: the truth and the markup have to come from one place, or the
    // truth is a guess about what a browser did with the markup.

    // Authored page geometry. Stated rather than assumed, because rungs 9, 13
    // and 14 deliberately exceed one screenful and the rest deliberately do not.
    struct ViewportSpec
    {
            std::int32_t viewport_w_ = 0;
            std::int32_t viewport_h_ = 0;
            // >= viewport_h_. Equal means the page cannot scroll, which for most
            // rungs is a property worth pinning rather than a coincidence.
            std::int32_t document_h_ = 0;
    };

    struct ScenePage
    {
            std::string name_;      // "a" — becomes a.html
            std::string html_;      // generated, byte-for-byte deterministic
            std::string marker_;    // unique <title> substring and body marker
    };

    // The kind of thing an observation is OF. This is the axis that makes the
    // two-independent-paths rule mechanical instead of a convention: a11y_* and
    // pixel_* can corroborate each other, and nothing corroborates itself.
    enum class Observe : std::uint8_t
    {
        A11yName,
        A11yValue,
        A11yState,
        A11yBounds,
        PixelColour,
        WindowTitle,
        ClipboardText,
        FitTranslation,    // the fiducial fit's ty — i.e. the scroll offset
        PageId,            // the page's own fiducial colour
        CursorPosition,
        ButtonClick,       // observed primary-button press/release pair with positions
        HoldDuration,
        Capability,
    };

    [[nodiscard]]
    constexpr std::string_view
    observer_of( Observe observe ) noexcept
    {
        switch( observe )
        {
            case Observe::A11yName :
            case Observe::A11yValue :
            case Observe::A11yState :
            case Observe::A11yBounds :
                return "a11y";
            case Observe::PixelColour :
            case Observe::PageId :
            case Observe::FitTranslation :
                return "pixel";
            case Observe::WindowTitle :
                return "window";
            case Observe::ClipboardText :
                return "clipboard";
            case Observe::CursorPosition :
                return "device";
            case Observe::ButtonClick :
                return "button";
            case Observe::HoldDuration :
                return "clock";
            case Observe::Capability :
                return "grab";
        }
        return "unknown";
    }

    // A claim, DECLARED BEFORE THE ACT.
    //
    // Declaring first is the point. A rung that decides what to check after
    // seeing what happened is writing its own exam, and an act that quietly
    // does less than intended then scores full marks.
    struct Expectation
    {
            std::string name_;
            Observe     observe_{};
            std::string subject_;    // authored target id, e.g. "btn_main"
            std::string value_;      // expected, rendered as text
            // Comparison is exact text unless one of these is set.
            double      tolerance_ = 0.0;    // |actual - expected| <= tolerance_
            double      low_       = 0.0;    // range form: low_ <= actual <= high_
            double      high_      = 0.0;
            bool        ranged_    = false;
    };

    struct Observation
    {
            Observe     observe_{};
            std::string subject_;
            std::string value_;
    };

    using Observations = std::vector<Observation>;

    struct Scene
    {
            std::string              id_;
            std::vector<ScenePage>   pages_;
            TruthSite                truth_;
            std::vector<Expectation> expect_;
            std::vector<std::string> frames_;    // declared frame list
            ViewportSpec             viewport_{};
    };

    // ── Scoring: pure, and therefore testable without a browser ──────────────
    //
    // evaluate() takes what was expected and what was seen and returns the
    // scorecard. It touches no display, no grab handle and no clock, so the
    // whole scoring layer is covered by the default (grab-OFF) build — which is
    // also what keeps spider `cp -r` graduatable.

    [[nodiscard]]
    inline const Observation*
    find_observation( const Observations& seen,
                      const Expectation&  want ) noexcept
    {
        for( const Observation& observation : seen )
        {
            if( observation.observe_ ==
                want.observe_ &&
                observation.subject_ == want.subject_ )
            {
                return &observation;
            }
        }
        return nullptr;
    }

    [[nodiscard]]
    inline bool
    matches( const Expectation& want,
             std::string_view   actual )
    {
        if( !want.ranged_ && want.tolerance_ == 0.0 )
        {
            return want.value_ == actual;
        }

        // Numeric comparison. A value that will not parse fails rather than
        // being silently treated as zero, which would make "" match 0.0.
        const std::string actual_text{ actual };
        char*             end = nullptr;
        // NOLINTNEXTLINE(cert-err34-c)
        const double      parsed = std::strtod( actual_text.c_str(), &end );
        if( end == nullptr || end == actual_text.c_str() || *end != '\0' )
        {
            return false;
        }
        if( want.ranged_ )
        {
            return parsed >= want.low_ && parsed <= want.high_;
        }

        char*        want_end = nullptr;
        // NOLINTNEXTLINE(cert-err34-c)
        const double target = std::strtod( want.value_.c_str(), &want_end );
        if( want_end == nullptr || *want_end != '\0' )
        {
            return false;
        }
        const double difference = parsed - target;
        return ( difference < 0.0 ? -difference : difference ) <= want.tolerance_;
    }

    [[nodiscard]]
    inline std::string
    describe( const Expectation& want )
    {
        if( want.ranged_ )
        {
            return std::to_string( want.low_ ) + " .. " + std::to_string( want.high_ );
        }
        if( want.tolerance_ != 0.0 )
        {
            return want.value_ + " +/- " + std::to_string( want.tolerance_ );
        }
        return want.value_;
    }

    [[nodiscard]]
    inline Scorecard
    evaluate( const Scene&        scene,
              const Observations& seen )
    {
        Scorecard card{ scene.id_ };
        for( const Expectation& want : scene.expect_ )
        {
            const Observation* observation = find_observation( seen, want );

            // A declared expectation with NO observation is a failure, not an
            // omission. Otherwise an act that never ran the step would score the
            // same as one that ran it correctly.
            const std::string  actual = observation != nullptr
                                          ? observation->value_
                                          : std::string{ "(not observed)" };
            const bool         pass = observation != nullptr && matches( want, actual );

            card.note( Assertion{
                .observer_ = std::string{ observer_of( want.observe_ ) },
                .name_     = want.name_,
                .expected_ = describe( want ),
                .actual_   = actual,
                .pass_     = pass
            } );
        }
        return card;
    }

}    // namespace ladder::view::stage
