#include "codec/png.hpp"
#include "core/reactor.hpp"
#include "event/window_x11.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "image/compare.hpp"
#include "notify/notifier.hpp"
#include "screen/workflow.hpp"

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

        constexpr unsigned char kAsciiUpperA                  = 'A';
        constexpr unsigned char kAsciiUpperZ                  = 'Z';
        constexpr unsigned char kAsciiCaseOffset              = 'a' - 'A';
        constexpr char          kCandidateSeparator           = ',';
        constexpr std::int32_t  kDefaultNotificationTimeoutMs = -1;
        constexpr std::uint32_t kOneCapture                   = 1U;
        constexpr std::uint64_t kNoDiffPixels                 = 0U;
        constexpr std::size_t   kSubscriptionDepth            = 64U;
        constexpr auto          kTrackerPollInterval = std::chrono::milliseconds{ 50 };
        constexpr auto          kWatchPollInterval   = std::chrono::milliseconds{ 20 };
        constexpr std::string_view kDefaultMissLabel = "<empty>";
        constexpr std::string_view kNotificationApp  = "grab";
        constexpr std::string_view kDiffSummary      = "screens differ";
        constexpr std::string_view kDiffPixelsLabel  = "diff_pixels=";
        constexpr std::string_view kMatchRatioLabel  = " match_ratio=";

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
        char
        ascii_lower( char value ) noexcept
        {
            const auto code = static_cast<unsigned char>( value );
            if( code >= kAsciiUpperA && code <= kAsciiUpperZ )
            {
                return static_cast<char>( code + kAsciiCaseOffset );
            }
            return value;
        }

        [[nodiscard]]
        std::string
        ascii_lower_copy( std::string_view text )
        {
            std::string result;
            result.reserve( text.size() );
            std::ranges::transform( text, std::back_inserter( result ), ascii_lower );
            return result;
        }

        [[nodiscard]]
        std::vector<std::string>
        normalized_candidates( const std::vector<std::string>& candidates )
        {
            std::vector<std::string> result;
            result.reserve( candidates.size() );
            for( const std::string& candidate : candidates )
            {
                if( !candidate.empty() )
                {
                    result.push_back( ascii_lower_copy( candidate ) );
                }
            }
            return result;
        }

        [[nodiscard]]
        bool
        wm_class_matches( std::string_view                wm_class,
                          const std::vector<std::string>& candidates )
        {
            const std::string lowered_class = ascii_lower_copy( wm_class );
            return std::ranges::any_of( candidates,
                                        [&lowered_class]( const std::string& candidate )
                                        {
                                            return lowered_class.contains( candidate );
                                        } );
        }

        [[nodiscard]]
        std::string
        miss_label( const std::vector<std::string>& candidates )
        {
            if( candidates.empty() )
            {
                return std::string{ kDefaultMissLabel };
            }

            std::string label;
            for( const std::string& candidate : candidates )
            {
                if( label.empty() )
                {
                    label = candidate;
                    continue;
                }
                label.push_back( kCandidateSeparator );
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
                return grab::fail( grab::ErrorCode::invalid_argument,
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
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "failed to open output file: " + path.string() );
            }

            if( !output_bytes.empty() )
            {
                stream.write( output_bytes.data(),
                              static_cast<std::streamsize>( output_bytes.size() ) );
            }
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
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
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "failed to open input file: " + path.string() );
            }

            const std::vector<char> input_bytes{
                std::istreambuf_iterator<char>{ stream },
                std::istreambuf_iterator<char>{}
            };
            if( stream.bad() )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
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
        grab::Result<void>
        capture_window_to_file( grab::Screen&                   screen,
                                const std::vector<std::string>& candidates,
                                const std::filesystem::path&    path )
        {
            auto image = screen.window_by_class( candidates );
            if( !image.has_value() )
            {
                return std::unexpected( std::move( image.error() ) );
            }

            auto encoded = grab::codec::encode_png( *image );
            if( !encoded.has_value() )
            {
                return std::unexpected( std::move( encoded.error() ) );
            }

            return write_binary_file( path, *encoded );
        }

        [[nodiscard]]
        std::optional<grab::WindowChange>
        matching_title_change( const grab::Event&              event,
                               const std::vector<std::string>& candidates )
        {
            if( event.kind != grab::EventKind::window_title_changed )
            {
                return std::nullopt;
            }

            const auto* payload = std::get_if<grab::WindowChange>( &event.payload );
            if( payload == nullptr || !wm_class_matches( payload->app, candidates ) )
            {
                return std::nullopt;
            }

            return *payload;
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        capture_pending_title_changes( grab::Screen&                   screen,
                                       grab::Subscription&             subscription,
                                       const std::vector<std::string>& candidates,
                                       const std::filesystem::path&    path )
        {
            std::uint32_t captured = 0U;
            while( auto event = subscription.try_pop() )
            {
                if( !matching_title_change( *event, candidates ).has_value() )
                {
                    continue;
                }

                auto written = capture_window_to_file( screen, candidates, path );
                if( !written.has_value() )
                {
                    return std::unexpected( std::move( written.error() ) );
                }
                captured += kOneCapture;
            }
            return captured;
        }

        [[nodiscard]]
        grab::notify::Notification
        diff_notification( const grab::image::DiffResult& diff )
        {
            return grab::notify::Notification{
                .app_name   = std::string{ kNotificationApp },
                .summary    = std::string{ kDiffSummary },
                .body       = std::string{ kDiffPixelsLabel } +
                              std::to_string( diff.diff_pixels ) +
                              std::string{ kMatchRatioLabel } +
                              std::to_string( diff.match_ratio ),
                .icon       = {},
                .timeout_ms = kDefaultNotificationTimeoutMs,
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
                if( image.error().code == grab::ErrorCode::window_not_found )
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

            result.captured += kOneCapture;
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

        if( diff->diff_pixels != kNoDiffPixels && notifier != nullptr )
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
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "watch_capture requires a stop predicate" );
        }

        const std::vector<std::string> candidates =
            normalized_candidates( wm_class_candidates );
        if( candidates.empty() )
        {
            return grab::fail(
                grab::ErrorCode::invalid_argument,
                "watch_capture requires at least one WM_CLASS candidate"
            );
        }

        RunningReactor running;
        grab::EventBus bus;
        auto           subscription = bus.subscribe(
            grab::EventFilter{
                .kinds      = { grab::EventKind::window_title_changed },
                .categories = {},
            },
            kSubscriptionDepth
        );

        auto tracker = grab::event::WindowTracker::start( nullptr,
                                                          running.reactor(),
                                                          bus,
                                                          kTrackerPollInterval );
        if( !tracker.has_value() )
        {
            running.stop_and_join();
            return std::unexpected( std::move( tracker.error() ) );
        }

        std::uint32_t captured = 0U;
        const auto    path     = std::filesystem::path{ out_path };
        while( !should_stop() )
        {
            auto pending =
                capture_pending_title_changes( screen, subscription, candidates, path );
            if( !pending.has_value() )
            {
                tracker->stop();
                running.stop_and_join();
                return std::unexpected( std::move( pending.error() ) );
            }
            captured += *pending;
            std::this_thread::sleep_for( kWatchPollInterval );
        }

        auto pending =
            capture_pending_title_changes( screen, subscription, candidates, path );
        if( !pending.has_value() )
        {
            tracker->stop();
            running.stop_and_join();
            return std::unexpected( std::move( pending.error() ) );
        }
        captured += *pending;

        tracker->stop();
        running.stop_and_join();
        if( !running.result().has_value() )
        {
            return std::unexpected( running.result().error() );
        }

        return captured;
    }

}    // namespace grab::screen
