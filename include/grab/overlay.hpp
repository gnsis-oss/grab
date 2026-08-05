#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace grab::overlay
{

    struct Color
    {
            std::uint8_t r = 0U;
            std::uint8_t g = 0U;
            std::uint8_t b = 0U;
            std::uint8_t a = std::numeric_limits<std::uint8_t>::max();
    };

    inline constexpr Color defaultOverlayColor{
        .r = 237U,
        .g = 206U,
        .b = 89U,
        .a = 255U,
    };

    struct StrokeStyle
    {
            Color color{};
            float width_px = 1.0F;
    };

    struct FillStyle
    {
            Color color{};
    };

    struct Persistent
    {
    };

    struct Ttl
    {
            std::chrono::milliseconds duration{};
    };

    struct Fade
    {
            std::chrono::milliseconds duration{};
    };

    using LifetimePolicy = std::variant<Persistent, Ttl, Fade>;

    enum class Easing : std::uint8_t
    {
        Linear,
        InQuad,
        OutQuad,
        InOutQuad,
        InCubic,
        OutCubic,
        InOutCubic,
    };

    enum class Axis : std::uint8_t
    {
        X,
        Y,
    };

    enum class Edge : std::uint8_t
    {
        Min,
        Max,
    };

    struct Channel
    {
            Easing                    easing = Easing::Linear;
            std::chrono::milliseconds duration{};
    };

    struct ScaleChannel : Channel
    {
            double from = 1.0;
            double to   = 1.0;
    };

    struct OpacityChannel : Channel
    {
            double from = 1.0;
            double to   = 1.0;
    };

    struct TranslateChannel : Channel
    {
            double dx = 0.0;
            double dy = 0.0;
    };

    struct RevealChannel : Channel
    {
            Axis   axis      = Axis::X;
            Edge   from_edge = Edge::Min;
            double from      = 0.0;
            double to        = 1.0;
    };

    struct AnimationSpec
    {
            std::optional<ScaleChannel>     scale     = std::nullopt;
            std::optional<OpacityChannel>   opacity   = std::nullopt;
            std::optional<TranslateChannel> translate = std::nullopt;
            std::optional<RevealChannel>    reveal    = std::nullopt;
    };

    enum class Band : std::uint8_t
    {
        Annotation,
        Trail,
    };

    struct MoveTo
    {
            SpacePoint point{};
    };

    struct LineTo
    {
            SpacePoint point{};
    };

    struct BezierTo
    {
            std::vector<SpacePoint> control;
    };

    struct ClosePath
    {
    };

    using PathCommand = std::variant<MoveTo, LineTo, BezierTo, ClosePath>;

    struct Path
    {
            std::vector<PathCommand> commands;
            bool                     closed = false;
    };

    struct Rect
    {
            SpaceRect bounds{};
    };

    struct Ellipse
    {
            SpacePoint center{};
            double     radius_x = 0.0;
            double     radius_y = 0.0;
    };

    struct Polygon
    {
            std::vector<SpacePoint> points;
    };

    using Geometry = std::variant<Path, Rect, Ellipse, Polygon>;

    struct Shape
    {
            Geometry                     geometry;
            std::optional<StrokeStyle>   stroke;
            std::optional<FillStyle>     fill;
            LifetimePolicy               lifetime{ Persistent{} };
            Band                         band      = Band::Annotation;
            std::int32_t                 z         = 0;
            std::optional<AnimationSpec> animation = std::nullopt;
    };

    struct SceneEpoch
    {
            std::uint64_t value = 0U;
            friend auto
            operator<=>( const SceneEpoch&,
                         const SceneEpoch& ) = default;
    };

    struct Revision
    {
            std::uint64_t value = 0U;
            friend auto
            operator<=>( const Revision&,
                         const Revision& ) = default;
    };

    struct ShapeId
    {
            SceneEpoch    epoch{};
            std::uint32_t slot = 0U;
            friend auto
            operator<=>( const ShapeId&,
                         const ShapeId& ) = default;
    };

    struct ShapeRecord
    {
            ShapeId                   id{};
            Shape                     shape{};
            std::chrono::milliseconds started_at{};
    };

    struct Upsert
    {
            ShapeRecord record{};
    };

    struct Remove
    {
            ShapeId id{};
    };

    struct Clear
    {
            SceneEpoch new_epoch{};
    };

    struct SceneDelta
    {
            SceneEpoch                          epoch{};
            Revision                            revision{};
            std::variant<Upsert, Remove, Clear> change;
    };

    struct SceneSnapshot
    {
            SceneEpoch               epoch{};
            Revision                 through_revision{};
            std::vector<ShapeRecord> shapes;
    };

}    // namespace grab::overlay

namespace grab
{

    class EditSession;

    class Overlay
    {
        public:

            ~Overlay();

            Overlay( const Overlay& ) = delete;
            Overlay&
            operator=( const Overlay& ) = delete;
            Overlay( Overlay&& )        = delete;
            Overlay&
            operator=( Overlay&& ) = delete;

            [[nodiscard]]
            Result<overlay::ShapeId>
            add( overlay::Shape shape );

            // Adds many shapes for the price of one call.
            //
            // Every mutating call here is a synchronous round trip to the
            // session's reactor thread, serviced on its frame clock — measured
            // at ~32 ms, and paid by the CALLER, who is blocked for all of it.
            // The cost is per call and nearly independent of what the call
            // carries, so anything animated (a cursor trail, a path being drawn,
            // a sweep of highlights) adding one shape at a time is capped near
            // thirty shapes per second AND stalls its own producer each time.
            //
            // Prefer this wherever more than one shape is known at once. It is
            // all-or-nothing: if any shape fails preflight, none are added.
            [[nodiscard]]
            Result<std::vector<overlay::ShapeId>>
            add_many( std::span<overlay::Shape> shapes );

            [[nodiscard]]
            Result<void>
            update( overlay::ShapeId id,
                    overlay::Shape   shape );

            [[nodiscard]]
            Result<void>
            remove( overlay::ShapeId id );

            void
            clear();

            [[nodiscard]]
            Result<void>
            flush();

            // The coordinate space the overlay surface renders in (the
            // display-global space in Phase 1). Draw into this space to avoid
            // any transform.
            [[nodiscard]]
            Result<CoordinateSpaceId>
            space();

            // Makes the overlay consume pointer input over its whole surface
            // instead of passing it through to whatever is underneath.
            //
            // An overlay is click-through by default, which is right for
            // annotation (a trail, a highlight) and wrong for a modal tool that
            // draws from the pointer. Such a tool learns about input from the
            // observation stream, which the server delivers regardless of who
            // owns the pointer — so without capture the same press ALSO reaches
            // the window below and, on a desktop, starts its rubber-band
            // selection alongside yours.
            //
            // Arm this when the tool becomes active, NOT when the button goes
            // down: by then the press has already been delivered elsewhere, and
            // grabbing afterwards only strands whatever it started.
            //
            // THE CALLER OWNS THE CAPTURE. A pointer grab that outlives its
            // owner freezes the user's desktop, so pair this with
            // release_pointer() on every exit path including the error ones.
            [[nodiscard]]
            Result<void>
            capture_pointer();

            // Idempotent and safe when nothing is captured.
            [[nodiscard]]
            Result<void>
            release_pointer();

        private:

            friend class EditSession;
            friend class Session;

            Overlay();

            void
            detach() noexcept;

            class Impl;
            std::shared_ptr<Impl> impl_;
    };

}    // namespace grab
