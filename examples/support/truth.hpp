#pragma once

#include "support/surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <grab/result.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ladder::view
{

    // ── Ground truth for the visual-crawl proving ground ────────────────────
    //
    // A visual crawl driven against a page nobody authored can only ever be
    // *inspected*: an operator looks at an overlay and judges whether it boxes
    // the right thing. That judgement does not scale, does not run unattended,
    // and — the expensive part — cannot distinguish "the crawler read the page
    // correctly" from "the crawler read a different page plausibly".
    //
    // So the proving ground is authored instead of found. Every element on the
    // synthetic site is emitted at an absolute, explicitly-sized position, and
    // the generator writes the rect it emitted into a truth file alongside the
    // HTML. The rect is therefore known *before* any browser is involved, and
    // every downstream claim — the a11y tree's geometry, the overlay's landing
    // point, the identity of the page a click arrived at — becomes an assertion
    // against an authored value rather than an eyeball.
    //
    // Two properties make the authored rects trustworthy:
    //
    //   * Nothing depends on text metrics. Targets are emitted as absolutely
    //     positioned *blocks* with explicit width/height and border-box sizing,
    //     so a target's border box is exactly what the generator wrote no
    //     matter which fonts the host happens to have installed. An inline <a>
    //     sized by its glyphs would make the truth file a guess.
    //
    //   * The page→screen mapping is measured, never assumed. Fiducials of
    //     known page position and known exact colour are painted onto every
    //     page; the harness finds them in a screenshot and solves for the
    //     mapping (see solve_page_to_screen). That yields a second, wholly
    //     independent path from page space to screen space — one through
    //     pixels, one through AT-SPI — and disagreement between them is a bug
    //     report rather than a matter of opinion.
    //
    // This header is pure vocabulary plus the fitting/comparison arithmetic:
    // no grab, no I/O, no browser. It compiles with the visual-crawl mode off.

    // What kind of element a target is. The crawler must not merely find *a*
    // node at the right coordinates — it must resolve the right ROLE, or an
    // armed run will happily "click a link" that is really a checkbox.
    enum class TruthKind : std::uint8_t
    {
        Link,
        Button,
        Checkbox,
        TextInput,
        ComboBox,
        Tab,
        Heading,
        Image,
        ListItem,
        TableCell,
    };

    // Why a target is interesting. These are the shapes that produce
    // plausible-but-wrong harvests, so the proving ground plants them
    // deliberately and the verifier reports its score broken down by flag —
    // a harness that only passes on the easy targets has not proven anything.
    namespace truth_flag
    {

        inline constexpr std::uint32_t none = 0U;
        // Requires scrolling to bring into the viewport.
        inline constexpr std::uint32_t below_fold = 1U << 0U;
        // Small enough that a few pixels of coordinate error miss it entirely.
        inline constexpr std::uint32_t tiny = 1U << 1U;
        // Covered by the sticky header at some scroll offsets.
        inline constexpr std::uint32_t occluded = 1U << 2U;
        // Its visible text repeats elsewhere on the same page.
        inline constexpr std::uint32_t duplicate_text = 1U << 3U;
        // Same visible text as another target but a DIFFERENT href: selecting
        // by label alone silently harvests the wrong page.
        inline constexpr std::uint32_t same_text_new_href = 1U << 4U;
        // Lives inside a table cell (deeper, busier a11y subtree).
        inline constexpr std::uint32_t in_table = 1U << 5U;
        // Navigates within the page rather than to a new document.
        inline constexpr std::uint32_t fragment_only = 1U << 6U;

    }    // namespace truth_flag

    // One authored element: what it is, what it says, where it goes, and the
    // exact page-space rect the generator emitted for it.
    struct TruthTarget
    {
            std::string   id_;
            std::string   text_;
            // Empty for non-navigating kinds (buttons, inputs, headings).
            std::string   href_;
            TruthKind     kind_{ TruthKind::Link };
            ViewRect      page_rect_{};
            std::uint32_t flags_{ truth_flag::none };
            // Position among this page's targets in document order. The
            // verifier matches extracted nodes to authored ones by href plus
            // this ordinal — never by geometry, which would make the geometry
            // check circular, and never by text alone, which the
            // same_text_new_href targets exist to punish.
            std::size_t   ordinal_{};

            [[nodiscard]]
            constexpr bool
            has_flag( std::uint32_t flag ) const noexcept
            {
                return ( flags_ & flag ) != 0U;
            }
    };

    // A solid patch of an exact colour at a known page position. Painted at the
    // page corners and, as a ruler column, every ruler_pitch pixels down the
    // left margin so that *some* fiducials are on screen at any scroll offset.
    struct TruthFiducial
    {
            double       page_x_{};
            double       page_y_{};
            double       size_{};
            std::uint8_t r_{};
            std::uint8_t g_{};
            std::uint8_t b_{};
    };

    struct TruthPage
    {
            std::string                id_;
            std::string                file_;
            std::string                title_;
            double                     content_w_{};
            double                     page_h_{};
            std::vector<TruthFiducial> fiducials_;
            std::vector<TruthTarget>   targets_;
    };

    struct TruthSite
    {
            std::vector<TruthPage> pages_;

            [[nodiscard]]
            const TruthPage*
            find( std::string_view page_id ) const noexcept
            {
                for( const auto& page : pages_ )
                {
                    if( page.id_ == page_id )
                    {
                        return &page;
                    }
                }
                return nullptr;
            }
    };

    // ── Page space → screen space ───────────────────────────────────────────

    // The mapping from authored page coordinates to screen pixels. Uniform
    // scale plus translation is the complete model: browser zoom and display
    // scale are both uniform, and the browser never rotates or shears content.
    //
    // Scrolling needs no separate term — a vertical scroll of the document is
    // exactly a change in ty_, so a fit taken from whatever fiducials happen to
    // be on screen already accounts for the current scroll offset.
    struct PageToScreen
    {
            double scale_{ 1.0 };
            double tx_{};
            double ty_{};

            [[nodiscard]]
            constexpr ViewRect
            apply( const ViewRect& page ) const noexcept
            {
                return ViewRect{
                    .x_ = ( page.x_ * scale_ ) + tx_,
                    .y_ = ( page.y_ * scale_ ) + ty_,
                    .w_ = page.w_ * scale_,
                    .h_ = page.h_ * scale_
                };
            }
    };

    // One fiducial located in a screenshot: where it was authored, and where it
    // was actually found.
    struct FiducialSighting
    {
            double page_x_{};
            double page_y_{};
            double screen_x_{};
            double screen_y_{};
    };

    // A fitted mapping plus the evidence for trusting it.
    struct AffineFit
    {
            PageToScreen map_{};
            // RMS distance, in screen pixels, between where the fit predicts
            // each sighting and where it was actually found. A capture whose
            // residual is large is not describable by any uniform-scale
            // mapping, which means the sightings are wrong (a mis-detected
            // colour, a partially occluded fiducial) and every rect derived
            // from it must be discarded rather than reported.
            double       residual_px_{};
            std::size_t  used_{};
    };

    // Least-squares fit of screen = scale * page + t over the sightings.
    //
    // Centring both point sets removes the translation, leaving a single scalar
    // scale whose least-squares solution is a ratio of dot products; the
    // translation then follows from the two centroids. Both axes are fitted
    // jointly, which is what lets a purely vertical ruler column (no spread in
    // x at all) still determine the scale — the y spread carries it.
    [[nodiscard]]
    inline grab::Result<AffineFit>
    solve_page_to_screen( std::span<const FiducialSighting> sightings ) noexcept
    {
        // Two distinct points are the minimum that can separate scale from
        // translation; one point admits every scale.
        constexpr std::size_t minimum_sightings = 2U;
        // Below this the centred page coordinates carry no spread on either
        // axis, so the scale is unconstrained however many points there are.
        constexpr double      minimum_spread = 1E-9;

        if( sightings.size() < minimum_sightings )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "fewer than two fiducial sightings" );
        }

        const auto count = static_cast<double>( sightings.size() );
        double     page_x_sum{};
        double     page_y_sum{};
        double     screen_x_sum{};
        double     screen_y_sum{};
        for( const auto& sighting : sightings )
        {
            page_x_sum   += sighting.page_x_;
            page_y_sum   += sighting.page_y_;
            screen_x_sum += sighting.screen_x_;
            screen_y_sum += sighting.screen_y_;
        }
        const double page_x_mean   = page_x_sum / count;
        const double page_y_mean   = page_y_sum / count;
        const double screen_x_mean = screen_x_sum / count;
        const double screen_y_mean = screen_y_sum / count;

        double       cross         = 0.0;
        double       spread        = 0.0;
        for( const auto& sighting : sightings )
        {
            const double page_dx    = sighting.page_x_ - page_x_mean;
            const double page_dy    = sighting.page_y_ - page_y_mean;
            const double screen_dx  = sighting.screen_x_ - screen_x_mean;
            const double screen_dy  = sighting.screen_y_ - screen_y_mean;
            cross                  += ( page_dx * screen_dx ) + ( page_dy * screen_dy );
            spread                 += ( page_dx * page_dx ) + ( page_dy * page_dy );
        }
        if( spread < minimum_spread )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "sightings carry no spatial spread" );
        }

        AffineFit fit{};
        fit.map_.scale_      = cross / spread;
        fit.map_.tx_         = screen_x_mean - ( fit.map_.scale_ * page_x_mean );
        fit.map_.ty_         = screen_y_mean - ( fit.map_.scale_ * page_y_mean );
        fit.used_            = sightings.size();

        double squared_error = 0.0;
        for( const auto& sighting : sightings )
        {
            const double predicted_x =
                ( fit.map_.scale_ * sighting.page_x_ ) + fit.map_.tx_;
            const double predicted_y =
                ( fit.map_.scale_ * sighting.page_y_ ) + fit.map_.ty_;
            const double error_x  = predicted_x - sighting.screen_x_;
            const double error_y  = predicted_y - sighting.screen_y_;
            squared_error        += ( error_x * error_x ) + ( error_y * error_y );
        }
        // Per-point distance, so the gate reads in pixels regardless of how
        // many fiducials the capture happened to contain.
        fit.residual_px_ = std::sqrt( squared_error / count );
        return fit;
    }

    // ── Rect comparison ─────────────────────────────────────────────────────

    // How far an observed rect is from the authored one, and whether that is
    // within tolerance. Both a per-edge displacement and an overlap ratio are
    // reported: the displacements say *which way* a mapping is wrong (a uniform
    // dy is a content-origin error, a dw that grows with x is a scale error),
    // while the IoU is the single number that answers "did it land on it".
    struct RectVerdict
    {
            double dx_{};
            double dy_{};
            double dw_{};
            double dh_{};
            double iou_{};
            bool   ok_{};
    };

    [[nodiscard]]
    inline double
    intersection_over_union( const ViewRect& lhs,
                             const ViewRect& rhs ) noexcept
    {
        const double left   = std::max( lhs.x_, rhs.x_ );
        const double top    = std::max( lhs.y_, rhs.y_ );
        const double right  = std::min( lhs.x_ + lhs.w_, rhs.x_ + rhs.w_ );
        const double bottom = std::min( lhs.y_ + lhs.h_, rhs.y_ + rhs.h_ );
        if( right <= left || bottom <= top )
        {
            return 0.0;
        }
        const double overlap    = ( right - left ) * ( bottom - top );
        const double union_area = lhs.area() + rhs.area() - overlap;
        return union_area > 0.0 ? overlap / union_area : 0.0;
    }

    // A rect passes on BOTH criteria, not either. The edge tolerance alone
    // would pass a rect that is the right size but offset by its own width on a
    // large target; the IoU alone would pass a small target swallowed by a much
    // larger observed rect. Requiring both is what makes "landed on it" mean
    // what an operator would mean by it.
    [[nodiscard]]
    inline RectVerdict
    compare_rects( const ViewRect& expected,
                   const ViewRect& observed,
                   double          tolerance_px,
                   double          minimum_iou ) noexcept
    {
        RectVerdict verdict{};
        verdict.dx_       = observed.x_ - expected.x_;
        verdict.dy_       = observed.y_ - expected.y_;
        verdict.dw_       = observed.w_ - expected.w_;
        verdict.dh_       = observed.h_ - expected.h_;
        verdict.iou_      = intersection_over_union( expected, observed );

        const bool within = std::abs( verdict.dx_ ) <=
                            tolerance_px &&
                            std::abs( verdict.dy_ ) <=
                            tolerance_px &&
                            std::abs( verdict.dw_ ) <=
                            tolerance_px &&
                            std::abs( verdict.dh_ ) <= tolerance_px;
        verdict.ok_       = within && verdict.iou_ >= minimum_iou;
        return verdict;
    }

}    // namespace ladder::view
