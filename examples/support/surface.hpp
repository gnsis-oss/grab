#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ladder::view
{

    // A grab-free screen rectangle. Keeping geometry out of grab lets every
    // crawl backend (network or visual) share the same Target vocabulary; the
    // grab-backed surface converts grab::SpaceRect <-> ViewRect at its edge.
    struct ViewRect
    {
            static constexpr double center_divisor = 2.0;

            double                  x_{};
            double                  y_{};
            double                  w_{};
            double                  h_{};

            [[nodiscard]]
            constexpr double
            center_x() const noexcept
            {
                return x_ + ( w_ / center_divisor );
            }

            [[nodiscard]]
            constexpr double
            center_y() const noexcept
            {
                return y_ + ( h_ / center_divisor );
            }

            [[nodiscard]]
            constexpr double
            area() const noexcept
            {
                return w_ * h_;
            }

            // One inclusive-boundary containment rule so targeting and the
            // safety fence (§10.3) agree on what "inside" means.
            [[nodiscard]]
            constexpr bool
            contains( double px,
                      double py ) const noexcept
            {
                const bool within_x = px >= x_ && px <= ( x_ + w_ );
                const bool within_y = py >= y_ && py <= ( y_ + h_ );
                return within_x && within_y;
            }
    };

    // A navigation target discovered on a page (plan §3). The href is optional
    // evidence — a11y usually exposes it, sometimes not — never the addressing
    // mechanism; node_id_ is the a11y identity used to re-resolve geometry,
    // which is stale the moment it is read on a live page.
    struct Target
    {
            ViewRect                   rect_;
            std::string                label_;
            std::optional<std::string> href_;
            std::uint64_t              node_id_{};
            std::uint16_t              depth_{};
    };

    // A harvested link: label, target, and screen rect for later overlay.
    struct Link
    {
            std::string label_;
            std::string href_;
            ViewRect    rect_;
    };

    // The result of harvesting one settled page (§6). truncated_ records a
    // clipped harvest so a node cap is never silently lossy (§6.1); confidence_
    // downgrades rather than fails when tiers disagree (§6.2).
    struct Harvest
    {
            enum class Confidence : std::uint8_t
            {
                High,
                Degraded,
            };

            std::string       uri_;
            std::string       title_;
            std::string       body_;
            std::vector<Link> links_;
            std::size_t       node_count_{};
            bool              truncated_{};
            Confidence        confidence_{ Confidence::High };
    };

    // The Surface concept that sat here in the spider original is deliberately
    // NOT ported: it existed to let a crawler brain drive a network page and a
    // real browser through one seam, and grab's examples have no network path.

}    // namespace ladder::view
