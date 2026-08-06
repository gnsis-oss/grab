#include "fake/fake_overlay_delegate.hpp"
#include "grab/context.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_animation.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/presentation/space_graph.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::presentation::AnimationRect;
    using grab::kernel::presentation::evaluate_animation;
    using grab::kernel::presentation::evaluate_opacity;
    using grab::kernel::presentation::EvaluatedReveal;
    using grab::kernel::presentation::reveal_clip;

    constexpr std::chrono::milliseconds animationStart{ 100 };
    constexpr std::chrono::milliseconds standardDuration{ 1'000 };
    constexpr std::chrono::milliseconds shortDuration{ 250 };
    constexpr std::chrono::milliseconds longDuration{ 1'000 };
    constexpr std::int64_t              midpointDivisor = 2;
    constexpr std::chrono::milliseconds longMidpoint    = longDuration / midpointDivisor;
    constexpr std::chrono::milliseconds halfStandardDuration =
        standardDuration / midpointDivisor;
    constexpr std::chrono::milliseconds afterCompletion{ 400 };
    constexpr std::chrono::milliseconds negativeDuration{ -1 };
    constexpr std::size_t               sampleIntervals                 = 20U;
    constexpr double                    easingFrom                      = -2.0;
    constexpr double                    easingTo                        = 6.0;
    constexpr double                    scaleFrom                       = 0.5;
    constexpr double                    scaleTo                         = 2.0;
    constexpr double                    expectedLongMidpointTranslation = 12.0;
    constexpr double translationX = expectedLongMidpointTranslation * midpointDivisor;
    constexpr double translationY = -8.0;
    constexpr double expectedLongMidpointTranslationY = translationY / midpointDivisor;
    constexpr double zeroDurationScaleTo              = 3.0;
    constexpr double zeroDurationOpacityTo            = 0.25;
    constexpr double zeroDurationTranslationX         = 9.0;
    constexpr double zeroDurationTranslationY         = -7.0;
    constexpr double zeroDurationRevealTo             = 0.75;
    constexpr double terminalScaleFrom                = 0.25;
    constexpr double terminalScaleTo                  = 1.5;
    constexpr double terminalOpacityFrom              = 0.2;
    constexpr double terminalOpacityTo                = 0.8;
    constexpr double terminalTranslationX             = 14.0;
    constexpr double terminalTranslationY             = -3.0;
    constexpr double fadeOpacityFrom                  = 1.0;
    constexpr double fadeOpacityTo                    = 0.5;
    constexpr double expectedHalfFadeOpacity          = 0.5;
    constexpr double expectedHalfChannelOpacity       = 0.75;
    constexpr double expectedComposedOpacity =
        expectedHalfFadeOpacity * expectedHalfChannelOpacity;
    constexpr double        fractionBelowRange = -0.1;
    constexpr double        fractionAboveRange = 1.1;
    constexpr double        negativeScale      = -0.01;
    constexpr std::uint8_t  invalidEnumValue = std::numeric_limits<std::uint8_t>::max();
    constexpr std::uint32_t topologyGeneration = 1U;
    constexpr std::uint64_t mappingId          = 7U;
    constexpr std::string_view runtimeName{ "overlay-animation-test" };
    constexpr double           shapeX                   = 10.0;
    constexpr double           shapeY                   = 20.0;
    constexpr double           shapeWidth               = 30.0;
    constexpr double           shapeHeight              = 40.0;
    constexpr double           axisScaleX               = 2.0;
    constexpr double           axisScaleY               = 3.0;
    constexpr double           axisTranslateX           = 5.0;
    constexpr double           axisTranslateY           = -7.0;
    constexpr double           rotationXx               = 0.0;
    constexpr double           rotationXy               = -1.0;
    constexpr double           rotationYx               = 1.0;
    constexpr double           rotationYy               = 0.0;
    constexpr double           rotationTranslateX       = 100.0;
    constexpr std::size_t      initialDelegateCallCount = 1U;
    constexpr AnimationRect    revealBounds{
        .x      = 10.0,
        .y      = 20.0,
        .width  = 80.0,
        .height = 40.0,
    };
    constexpr double     revealFraction          = 0.25;
    constexpr double     revealedWidth           = 20.0;
    constexpr double     revealedHeight          = 10.0;
    constexpr double     maxEdgeRevealX          = 70.0;
    constexpr double     maxEdgeRevealY          = 50.0;
    constexpr double     persistentLifetimeAlpha = 1.0;

    constexpr std::array easingCurves{
        grab::overlay::Easing::Linear,
        grab::overlay::Easing::InQuad,
        grab::overlay::Easing::OutQuad,
        grab::overlay::Easing::InOutQuad,
        grab::overlay::Easing::InCubic,
        grab::overlay::Easing::OutCubic,
        grab::overlay::Easing::InOutCubic,
    };

    class AnimationRuntime final : public grab::spi::Runtime
    {
        public:

            [[nodiscard]]
            std::string_view
            name() const override
            {
                return runtimeName;
            }

            [[nodiscard]]
            std::uint32_t
            generation() const override
            {
                return topologyGeneration;
            }

            [[nodiscard]]
            grab::Result<void>
            start( [[maybe_unused]] const grab::OperationContext& context ) override
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            stop() override
            {
                return {};
            }

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override
            {
                return {};
            }

            [[nodiscard]]
            grab::spi::OverlayDelegate*
            overlay_delegate() override
            {
                return &delegate;
            }

            grab::testing::FakeOverlayDelegate delegate;
    };

    [[nodiscard]]
    grab::overlay::AnimationSpec
    scale_animation( double                    from,
                     double                    to,
                     std::chrono::milliseconds duration,
                     grab::overlay::Easing     easing = grab::overlay::Easing::Linear )
    {
        grab::overlay::ScaleChannel channel;
        channel.from     = from;
        channel.to       = to;
        channel.duration = duration;
        channel.easing   = easing;
        grab::overlay::AnimationSpec animation;
        animation.scale = channel;
        return animation;
    }

    [[nodiscard]]
    grab::overlay::Shape
    animated_rect( grab::CoordinateSpaceId      space,
                   grab::overlay::AnimationSpec animation )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = {
                       .x     = shapeX,
                       .y     = shapeY,
                       .w     = shapeWidth,
                       .h     = shapeHeight,
                       .space = space,
                       },
        };
        shape.fill.emplace();
        shape.animation = animation;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    animated_path( grab::CoordinateSpaceId      space,
                   grab::overlay::AnimationSpec animation )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Path{
            .commands = {
                         grab::overlay::MoveTo{
                    .point = { .x = shapeX, .y = shapeY, .space = space },
                }, grab::overlay::LineTo{
                    .point = {
                        .x     = shapeX + shapeWidth,
                        .y     = shapeY + shapeHeight,
                        .space = space,
                    },
                }, },
        };
        shape.stroke.emplace();
        shape.animation = animation;
        return shape;
    }

    [[nodiscard]]
    grab::Result<std::unique_ptr<grab::kernel::presentation::OverlayService>>
    animation_service( AnimationRuntime&               runtime,
                       const grab::detail::SpaceGraph& graph,
                       grab::CoordinateSpaceId         delegate_space )
    {
        return grab::kernel::presentation::OverlayService::create(
            runtime,
            graph,
            delegate_space,
            []
            {
                return animationStart;
            }
        );
    }

    [[nodiscard]]
    std::vector<grab::overlay::AnimationSpec>
    invalid_animations()
    {
        const auto infinity     = std::numeric_limits<double>::infinity();
        const auto not_a_number = std::numeric_limits<double>::quiet_NaN();
        std::vector<grab::overlay::AnimationSpec> result;

        auto non_finite_scale = scale_animation( infinity, scaleTo, standardDuration );
        result.push_back( non_finite_scale );

        auto negative_duration = scale_animation( scaleFrom, scaleTo, negativeDuration );
        result.push_back( negative_duration );

        auto negative_scale =
            scale_animation( scaleFrom, negativeScale, standardDuration );
        result.push_back( negative_scale );

        grab::overlay::AnimationSpec invalid_opacity_low;
        invalid_opacity_low.opacity.emplace();
        invalid_opacity_low.opacity->from = fractionBelowRange;
        result.push_back( invalid_opacity_low );

        grab::overlay::AnimationSpec invalid_opacity_high;
        invalid_opacity_high.opacity.emplace();
        invalid_opacity_high.opacity->to = fractionAboveRange;
        result.push_back( invalid_opacity_high );

        grab::overlay::AnimationSpec non_finite_translation;
        non_finite_translation.translate.emplace();
        non_finite_translation.translate->dy = not_a_number;
        result.push_back( non_finite_translation );

        grab::overlay::AnimationSpec invalid_reveal_low;
        invalid_reveal_low.reveal.emplace();
        invalid_reveal_low.reveal->from = fractionBelowRange;
        result.push_back( invalid_reveal_low );

        grab::overlay::AnimationSpec invalid_reveal_high;
        invalid_reveal_high.reveal.emplace();
        invalid_reveal_high.reveal->to = fractionAboveRange;
        result.push_back( invalid_reveal_high );

        auto invalid_easing =
            scale_animation( scaleFrom,
                             scaleTo,
                             standardDuration,
                             static_cast<grab::overlay::Easing>( invalidEnumValue ) );
        result.push_back( invalid_easing );

        grab::overlay::AnimationSpec invalid_axis;
        invalid_axis.reveal.emplace();
        invalid_axis.reveal->axis = static_cast<grab::overlay::Axis>( invalidEnumValue );
        result.push_back( invalid_axis );

        grab::overlay::AnimationSpec invalid_edge;
        invalid_edge.reveal.emplace();
        invalid_edge.reveal->from_edge =
            static_cast<grab::overlay::Edge>( invalidEnumValue );
        result.push_back( invalid_edge );

        return result;
    }

}    // namespace

TEST( OverlayAnimation,
      EasingCurvesStartAtFromEndAtToAndRemainMonotonic )
{
    for( const auto easing : easingCurves )
    {
        auto animation =
            scale_animation( easingFrom, easingTo, standardDuration, easing );

        const auto start =
            evaluate_animation( animation, std::chrono::milliseconds::zero() );
        const auto end = evaluate_animation( animation, standardDuration );
        EXPECT_DOUBLE_EQ( start.scale, easingFrom );
        EXPECT_DOUBLE_EQ( end.scale, easingTo );

        auto previous = start.scale;
        for( std::size_t step{}; step <= sampleIntervals; ++step )
        {
            const auto elapsed = standardDuration * step / sampleIntervals;
            const auto current = evaluate_animation( animation, elapsed ).scale;
            EXPECT_GE( current, previous );
            previous = current;
        }
    }
}

TEST( OverlayAnimation,
      UnequalChannelDurationsClampShorterChannelsWhileLongerChannelsRun )
{
    auto animation = scale_animation( scaleFrom, scaleTo, shortDuration );
    animation.translate.emplace();
    animation.translate->duration = longDuration;
    animation.translate->dx       = translationX;
    animation.translate->dy       = translationY;

    const auto midway             = evaluate_animation( animation, longMidpoint );

    EXPECT_DOUBLE_EQ( midway.scale, scaleTo );
    EXPECT_DOUBLE_EQ( midway.translate_x, expectedLongMidpointTranslation );
    EXPECT_DOUBLE_EQ( midway.translate_y, expectedLongMidpointTranslationY );
    EXPECT_EQ( midway.duration, longDuration );
    EXPECT_FALSE( midway.complete );

    const auto complete = evaluate_animation( animation, longDuration );
    EXPECT_DOUBLE_EQ( complete.scale, scaleTo );
    EXPECT_DOUBLE_EQ( complete.translate_x, translationX );
    EXPECT_DOUBLE_EQ( complete.translate_y, translationY );
    EXPECT_TRUE( complete.complete );
}

TEST( OverlayAnimation,
      ZeroDurationChannelsYieldTheirToValuesImmediately )
{
    auto animation = scale_animation( scaleFrom,
                                      zeroDurationScaleTo,
                                      std::chrono::milliseconds::zero() );
    animation.opacity.emplace();
    animation.opacity->from = fadeOpacityFrom;
    animation.opacity->to   = zeroDurationOpacityTo;
    animation.translate.emplace();
    animation.translate->dx = zeroDurationTranslationX;
    animation.translate->dy = zeroDurationTranslationY;
    animation.reveal.emplace();
    animation.reveal->from = fractionBelowRange + revealFraction;
    animation.reveal->to   = zeroDurationRevealTo;

    const auto evaluated =
        evaluate_animation( animation, std::chrono::milliseconds::zero() );

    EXPECT_DOUBLE_EQ( evaluated.scale, zeroDurationScaleTo );
    EXPECT_DOUBLE_EQ( evaluated.opacity, zeroDurationOpacityTo );
    EXPECT_DOUBLE_EQ( evaluated.translate_x, zeroDurationTranslationX );
    EXPECT_DOUBLE_EQ( evaluated.translate_y, zeroDurationTranslationY );
    ASSERT_TRUE( evaluated.reveal.has_value() );
    EXPECT_DOUBLE_EQ( evaluated.reveal->fraction, zeroDurationRevealTo );
    EXPECT_TRUE( evaluated.complete );
}

TEST( OverlayAnimation,
      CompletedPersistentAnimationHoldsItsTerminalState )
{
    grab::overlay::AnimationSpec animation =
        scale_animation( terminalScaleFrom, terminalScaleTo, standardDuration );
    animation.opacity.emplace();
    animation.opacity->from     = terminalOpacityFrom;
    animation.opacity->to       = terminalOpacityTo;
    animation.opacity->duration = shortDuration;
    animation.translate.emplace();
    animation.translate->dx       = terminalTranslationX;
    animation.translate->dy       = terminalTranslationY;
    animation.translate->duration = longDuration;

    grab::overlay::ShapeRecord record;
    record.started_at      = animationStart;
    record.shape.lifetime  = grab::overlay::Persistent{};
    record.shape.animation = animation;
    const auto evaluated =
        evaluate_animation( record, animationStart + longDuration + afterCompletion );

    EXPECT_DOUBLE_EQ( evaluated.scale, terminalScaleTo );
    EXPECT_DOUBLE_EQ( evaluated.opacity, terminalOpacityTo );
    EXPECT_DOUBLE_EQ( evaluated.translate_x, terminalTranslationX );
    EXPECT_DOUBLE_EQ( evaluated.translate_y, terminalTranslationY );
    EXPECT_TRUE( evaluated.complete );
    EXPECT_DOUBLE_EQ(
        evaluate_opacity( record, animationStart + longDuration + afterCompletion ),
        terminalOpacityTo * persistentLifetimeAlpha
    );
}

TEST( OverlayAnimation,
      FadeAlphaAndOpacityChannelAlphaMultiply )
{
    grab::overlay::AnimationSpec animation;
    animation.opacity.emplace();
    animation.opacity->from     = fadeOpacityFrom;
    animation.opacity->to       = fadeOpacityTo;
    animation.opacity->duration = standardDuration;

    grab::overlay::ShapeRecord record;
    record.started_at      = animationStart;
    record.shape.lifetime  = grab::overlay::Fade{ .duration = standardDuration };
    record.shape.animation = animation;

    EXPECT_DOUBLE_EQ( evaluate_opacity( record, animationStart + halfStandardDuration ),
                      expectedComposedOpacity );
}

TEST( OverlayAnimation,
      ServiceValidationRejectsInvalidAnimationValuesAndEnums )
{
    grab::detail::SpaceGraph graph;
    const auto               delegate_space = graph.add_space( topologyGeneration );
    AnimationRuntime         runtime;
    auto service = animation_service( runtime, graph, delegate_space );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    for( auto animation : invalid_animations() )
    {
        const auto added =
            ( *service )->add( animated_rect( delegate_space, animation ) );
        ASSERT_FALSE( added.has_value() );
        EXPECT_EQ( added.error().code, grab::ErrorCode::InvalidArgument );
    }
    EXPECT_EQ( runtime.delegate.calls().size(), initialDelegateCallCount );
}

TEST( OverlayAnimation,
      RevealClipsFromEachRequestedEdgeAlongTheRequestedAxis )
{
    const auto x_min = reveal_clip( revealBounds,
                                    EvaluatedReveal{
                                        .axis      = grab::overlay::Axis::X,
                                        .from_edge = grab::overlay::Edge::Min,
                                        .fraction  = revealFraction,
                                    } );
    EXPECT_DOUBLE_EQ( x_min.x, revealBounds.x );
    EXPECT_DOUBLE_EQ( x_min.y, revealBounds.y );
    EXPECT_DOUBLE_EQ( x_min.width, revealedWidth );
    EXPECT_DOUBLE_EQ( x_min.height, revealBounds.height );

    const auto x_max = reveal_clip( revealBounds,
                                    EvaluatedReveal{
                                        .axis      = grab::overlay::Axis::X,
                                        .from_edge = grab::overlay::Edge::Max,
                                        .fraction  = revealFraction,
                                    } );
    EXPECT_DOUBLE_EQ( x_max.x, maxEdgeRevealX );
    EXPECT_DOUBLE_EQ( x_max.y, revealBounds.y );
    EXPECT_DOUBLE_EQ( x_max.width, revealedWidth );
    EXPECT_DOUBLE_EQ( x_max.height, revealBounds.height );

    const auto y_min = reveal_clip( revealBounds,
                                    EvaluatedReveal{
                                        .axis      = grab::overlay::Axis::Y,
                                        .from_edge = grab::overlay::Edge::Min,
                                        .fraction  = revealFraction,
                                    } );
    EXPECT_DOUBLE_EQ( y_min.x, revealBounds.x );
    EXPECT_DOUBLE_EQ( y_min.y, revealBounds.y );
    EXPECT_DOUBLE_EQ( y_min.width, revealBounds.width );
    EXPECT_DOUBLE_EQ( y_min.height, revealedHeight );

    const auto y_max = reveal_clip( revealBounds,
                                    EvaluatedReveal{
                                        .axis      = grab::overlay::Axis::Y,
                                        .from_edge = grab::overlay::Edge::Max,
                                        .fraction  = revealFraction,
                                    } );
    EXPECT_DOUBLE_EQ( y_max.x, revealBounds.x );
    EXPECT_DOUBLE_EQ( y_max.y, maxEdgeRevealY );
    EXPECT_DOUBLE_EQ( y_max.width, revealBounds.width );
    EXPECT_DOUBLE_EQ( y_max.height, revealedHeight );
}

TEST( OverlayAnimation,
      ServiceRejectsAnimatedShapesThatRequireTransformationOrLowering )
{
    grab::detail::SpaceGraph graph;
    const auto               transformed_space = graph.add_space( topologyGeneration );
    const auto               lowered_space     = graph.add_space( topologyGeneration );
    const auto               delegate_space    = graph.add_space( topologyGeneration );
    graph.add_transform( grab::TransformRecord{
        .source      = transformed_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = axisScaleX,
                  .tx = axisTranslateX,
                  .yy = axisScaleY,
                  .ty = axisTranslateY,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    graph.add_transform( grab::TransformRecord{
        .source      = lowered_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = rotationXx,
                  .xy = rotationXy,
                  .tx = rotationTranslateX,
                  .yx = rotationYx,
                  .yy = rotationYy,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    AnimationRuntime runtime;
    auto             service = animation_service( runtime, graph, delegate_space );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    const auto transformed =
        ( *service )
            ->add(
                animated_path( transformed_space,
                               scale_animation( scaleFrom, scaleTo, standardDuration ) )
            );
    ASSERT_FALSE( transformed.has_value() );
    EXPECT_EQ( transformed.error().code, grab::ErrorCode::InvalidArgument );

    const auto lowered =
        ( *service )
            ->add(
                animated_rect( lowered_space,
                               scale_animation( scaleFrom, scaleTo, standardDuration ) )
            );
    ASSERT_FALSE( lowered.has_value() );
    EXPECT_EQ( lowered.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_EQ( runtime.delegate.calls().size(), initialDelegateCallCount );
}
