#include "codec/png.hpp"
#include "drivers/desktop/x11/window_match.hpp"
#include "drivers/desktop/x11/window_tracker.hpp"
#include "drivers/desktop/x11/workflow.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/geometry/size.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "image/compare.hpp"
#include "kernel/action/polling_event_source.hpp"
#include "kernel/action/wait_engine.hpp"
#include "kernel/capture/tile_differ.hpp"
#include "kernel/scheduling/reactor.hpp"
#include "notify/notifier.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace grab::screen
{
    namespace
    {

        constexpr char             candidateSeparator           = ',';
        constexpr std::int32_t     defaultNotificationTimeoutMs = -1;
        constexpr std::uint32_t    oneCapture                   = 1U;
        constexpr std::uint64_t    noDiffPixels                 = 0U;
        constexpr std::size_t      subscriptionDepth            = 64U;
        constexpr auto             trackerPollInterval = std::chrono::milliseconds{ 50 };
        constexpr auto             watchPollWindow     = std::chrono::milliseconds{ 25 };
        constexpr std::uint16_t    watchTileExtent     = 32U;
        constexpr std::string_view defaultMissLabel    = "<empty>";
        constexpr std::string_view notificationApp     = "grab";
        constexpr std::string_view diffSummary         = "screens differ";
        constexpr std::string_view diffPixelsLabel     = "diff_pixels=";
        constexpr std::string_view matchRatioLabel     = " match_ratio=";

        class RunningReactor
        {
            public:

                RunningReactor() :
                    thread_(
                        [this]
                        {
                            result_ = reactor_.run();
                        }
                    )
                {
                }

                ~RunningReactor()
                {
                    stop_and_join();
                }

                RunningReactor( const RunningReactor& ) = delete;
                RunningReactor&
                operator=( const RunningReactor& ) = delete;
                RunningReactor( RunningReactor&& ) = delete;
                RunningReactor&
                operator=( RunningReactor&& ) = delete;

                [[nodiscard]]
                grab::core::Reactor&
                reactor() noexcept
                {
                    return reactor_;
                }

                void
                stop_and_join() noexcept
                {
                    reactor_.stop();
                    if( thread_.joinable() )
                    {
                        thread_.join();
                    }
                }

                [[nodiscard]]
                const grab::Result<void>&
                result() const noexcept
                {
                    return result_;
                }

            private:

                grab::core::Reactor reactor_;
                grab::Result<void>  result_;
                std::thread         thread_;
        };

        [[nodiscard]]
        std::string
        miss_label( const std::vector<std::string>& candidates )
        {
            if( candidates.empty() )
            {
                return std::string{ defaultMissLabel };
            }

            std::string label;
            for( const std::string& candidate : candidates )
            {
                if( label.empty() )
                {
                    label = candidate;
                    continue;
                }
                label.push_back( candidateSeparator );
                label.append( candidate );
            }
            return label;
        }

        [[nodiscard]]
        grab::Result<void>
        write_binary_file( const std::filesystem::path& path,
                           std::span<const std::byte>   bytes )
        {
            if( bytes.size() >
                static_cast<std::size_t>( std::numeric_limits<std::streamsize>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "file is too large to write: " + path.string() );
            }

            std::vector<char> output_bytes;
            output_bytes.reserve( bytes.size() );
            for( const std::byte value : bytes )
            {
                output_bytes.push_back(
                    static_cast<char>( std::to_integer<unsigned char>( value ) )
                );
            }

            std::ofstream stream{ path, std::ios::binary };
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to open output file: " + path.string() );
            }

            if( !output_bytes.empty() )
            {
                stream.write( output_bytes.data(),
                              static_cast<std::streamsize>( output_bytes.size() ) );
            }
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to write output file: " + path.string() );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<std::vector<std::byte>>
        read_binary_file( const std::filesystem::path& path )
        {
            std::ifstream stream{ path, std::ios::binary };
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to open input file: " + path.string() );
            }

            const std::vector<char> input_bytes{
                std::istreambuf_iterator<char>{ stream },
                std::istreambuf_iterator<char>{}
            };
            if( stream.bad() )
            {
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "failed to read input file: " + path.string() );
            }

            std::vector<std::byte> bytes;
            bytes.reserve( input_bytes.size() );
            std::ranges::transform(
                input_bytes,
                std::back_inserter( bytes ),
                []( char value )
                {
                    return static_cast<std::byte>( static_cast<unsigned char>( value ) );
                }
            );
            return bytes;
        }

        [[nodiscard]]
        std::optional<grab::WindowChange>
        matching_title_change( const grab::Event&              event,
                               const std::vector<std::string>& candidates )
        {
            if( event.kind != grab::EventKind::WindowTitleChanged )
            {
                return std::nullopt;
            }

            const auto* payload = std::get_if<grab::WindowChange>( &event.payload );
            if( payload ==
                nullptr ||
                !grab::screen::wm_class_matches_any( payload->app, candidates ) )
            {
                return std::nullopt;
            }

            return *payload;
        }

        [[nodiscard]]
        grab::notify::Notification
        diff_notification( const grab::image::DiffResult& diff )
        {
            return grab::notify::Notification{
                .app_name   = std::string{ notificationApp },
                .summary    = std::string{ diffSummary },
                .body       = std::string{ diffPixelsLabel } +
                              std::to_string( diff.diff_pixels ) +
                              std::string{ matchRatioLabel } +
                              std::to_string( diff.match_ratio ),
                .icon       = {},
                .timeout_ms = defaultNotificationTimeoutMs,
            };
        }

    }    // namespace

    grab::Result<BatchResult>
    batch_capture( grab::Screen&                 screen,
                   const std::vector<BatchItem>& items )
    {
        BatchResult result;
        for( const BatchItem& item : items )
        {
            auto image = screen.window_by_class( item.wm_class_candidates );
            if( !image.has_value() )
            {
                if( image.error().code == grab::ErrorCode::WindowNotFound )
                {
                    result.misses.push_back( miss_label( item.wm_class_candidates ) );
                    continue;
                }
                return std::unexpected( std::move( image.error() ) );
            }

            auto encoded = grab::codec::encode_png( *image );
            if( !encoded.has_value() )
            {
                return std::unexpected( std::move( encoded.error() ) );
            }

            auto written =
                write_binary_file( std::filesystem::path{ item.out_path }, *encoded );
            if( !written.has_value() )
            {
                return std::unexpected( std::move( written.error() ) );
            }

            result.captured += oneCapture;
        }
        return result;
    }

    grab::Result<grab::image::DiffResult>
    compare_files( const std::string&      path_a,
                   const std::string&      path_b,
                   grab::notify::Notifier* notifier )
    {
        auto bytes_a = read_binary_file( std::filesystem::path{ path_a } );
        if( !bytes_a.has_value() )
        {
            return std::unexpected( std::move( bytes_a.error() ) );
        }

        auto bytes_b = read_binary_file( std::filesystem::path{ path_b } );
        if( !bytes_b.has_value() )
        {
            return std::unexpected( std::move( bytes_b.error() ) );
        }

        auto image_a = grab::codec::decode_png( *bytes_a );
        if( !image_a.has_value() )
        {
            return std::unexpected( std::move( image_a.error() ) );
        }

        auto image_b = grab::codec::decode_png( *bytes_b );
        if( !image_b.has_value() )
        {
            return std::unexpected( std::move( image_b.error() ) );
        }

        auto diff = grab::image::compare( *image_a, *image_b );
        if( !diff.has_value() )
        {
            return std::unexpected( std::move( diff.error() ) );
        }

        if( diff->diff_pixels != noDiffPixels && notifier != nullptr )
        {
            auto notified = notifier->notify( diff_notification( *diff ) );
            if( !notified.has_value() )
            {
                return std::unexpected( std::move( notified.error() ) );
            }
        }

        return *diff;
    }

    grab::Result<std::uint32_t>
    watch_capture( grab::Screen&                   screen,
                   const std::vector<std::string>& wm_class_candidates,
                   const std::string&              out_path,
                   const std::function<bool()>&    should_stop )
    {
        if( !should_stop )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "watch_capture requires a stop predicate" );
        }

        const std::vector<std::string> candidates =
            grab::screen::normalized_wm_class_candidates( wm_class_candidates );
        if( candidates.empty() )
        {
            return grab::fail(
                grab::ErrorCode::InvalidArgument,
                "watch_capture requires at least one WM_CLASS candidate"
            );
        }

        RunningReactor running;
        grab::EventBus bus;
        auto           subscription = bus.subscribe(
            grab::EventFilter{
                .kinds      = { grab::EventKind::WindowTitleChanged },
                .categories = {},
            },
            subscriptionDepth
        );

        auto tracker =
            grab::drivers::desktop::x11::WindowTracker::start( nullptr,
                                                               running.reactor(),
                                                               bus,
                                                               trackerPollInterval );
        if( !tracker.has_value() )
        {
            running.stop_and_join();
            return std::unexpected( std::move( tracker.error() ) );
        }

        const auto                     path     = std::filesystem::path{ out_path };
        std::uint32_t                  captured = 0U;
        std::optional<grab::Image>     previous;
        const grab::kernel::TileDiffer differ;
        const grab::geometry::Size     tile_size{
            .width  = watchTileExtent,
            .height = watchTileExtent,
        };

        // Drain any pending matching title-change events, capturing the window
        // and writing a fresh PNG only when its pixels actually changed since the
        // last write. Returns the number of NEW captures written this drain.
        const auto drain_once = [&]() -> grab::Result<std::uint32_t>
        {
            std::uint32_t written_now = 0U;
            while( auto event = subscription.try_pop() )
            {
                if( !matching_title_change( *event, candidates ).has_value() )
                {
                    continue;
                }

                auto image = screen.window_by_class( candidates );
                if( !image.has_value() )
                {
                    return std::unexpected( std::move( image.error() ) );
                }

                bool changed = !previous.has_value();
                if( previous.has_value() )
                {
                    const auto result = differ.diff( *previous, *image, tile_size );
                    changed = result.kind != grab::kernel::TileDiffKind::NoChange;
                }
                if( !changed )
                {
                    continue;
                }

                auto encoded = grab::codec::encode_png( *image );
                if( !encoded.has_value() )
                {
                    return std::unexpected( std::move( encoded.error() ) );
                }
                auto write_result = write_binary_file( path, *encoded );
                if( !write_result.has_value() )
                {
                    return std::unexpected( std::move( write_result.error() ) );
                }
                previous     = std::move( *image );
                written_now += oneCapture;
            }
            return written_now;
        };

        grab::OperationContext                   wait_context;
        const grab::kernel::action::WaitEngine   engine{ wait_context };
        grab::kernel::action::PollingEventSource pacer;

        while( !should_stop() )
        {
            grab::kernel::action::NamedPredicate predicate{
                .name    = "watch_title_change",
                .observe = [&drain_once, &captured]()
                    -> grab::Result<grab::kernel::action::PredicateObservation>
                {
                    auto drained = drain_once();
                    if( !drained.has_value() )
                    {
                        return std::unexpected( std::move( drained.error() ) );
                    }
                    captured += *drained;
                    return grab::kernel::action::PredicateObservation{
                        .satisfied = *drained > 0U,
                        .detail    = "no matching title change yet",
                    };
                },
            };
            const grab::kernel::action::WaitParams params{
                .deadline = grab::Deadline::after( watchPollWindow ),
                .backoff  = {},
            };
            auto waited = engine.wait( predicate, params, pacer );
            if( !waited.has_value() &&
                waited.error().code != grab::ErrorCode::DeadlineExceeded )
            {
                tracker->stop();
                running.stop_and_join();
                return std::unexpected( std::move( waited.error() ) );
            }
        }

        auto final_drain = drain_once();
        if( !final_drain.has_value() )
        {
            tracker->stop();
            running.stop_and_join();
            return std::unexpected( std::move( final_drain.error() ) );
        }
        captured += *final_drain;

        tracker->stop();
        running.stop_and_join();
        if( !running.result().has_value() )
        {
            return std::unexpected( running.result().error() );
        }

        return captured;
    }

}    // namespace grab::screen
