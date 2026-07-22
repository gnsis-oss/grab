#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"
#include "grab/space.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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
            Geometry                   geometry;
            std::optional<StrokeStyle> stroke;
            std::optional<FillStyle>   fill;
            LifetimePolicy             lifetime{ Persistent{} };
            Band                       band = Band::Annotation;
            std::int32_t               z    = 0;
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

        private:

            friend class Session;

            Overlay();

            void
            detach() noexcept;

            class Impl;
            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab
