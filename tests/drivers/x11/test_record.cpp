#include "core/reactor.hpp"
#include "drivers/desktop/x11/record.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>
extern "C"
{
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}
// clang-format on

namespace
{

    constexpr const char*      xvfbDisplay        = ":91";
    constexpr const char*      badDisplay         = ":bad-nonexistent-91";
    constexpr const char*      outputExtension    = ".mp4";
    constexpr const char*      outputPrefix       = "grab-record-test-";
    constexpr std::uint32_t    recordFps          = 10U;
    constexpr std::uint32_t    shortClipFrames    = 10U;
    constexpr std::uint32_t    unlimitedFrames    = 0U;
    constexpr std::uint32_t    firstTempIndex     = 1U;
    constexpr std::uint32_t    tempIndexStep      = 1U;
    constexpr std::uint32_t    requiredPackets    = 1U;
    constexpr auto             threadReadyTimeout = std::chrono::seconds{ 2 };
    constexpr auto             postTimeout        = std::chrono::seconds{ 2 };
    constexpr auto             recordTimeout      = std::chrono::seconds{ 8 };
    constexpr auto             manualStopWarmup   = std::chrono::milliseconds{ 500 };
    constexpr auto             pollInterval       = std::chrono::milliseconds{ 100 };
    constexpr int              libavSuccess       = 0;
    constexpr int              noVideoStream      = -1;

    std::atomic<std::uint32_t> g_next_temp_index{ firstTempIndex };

    [[nodiscard]]
    std::string
    libav_error_message( int code )
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
        if( av_strerror( code, buffer.data(), buffer.size() ) < libavSuccess )
        {
            return "unknown libav error " + std::to_string( code );
        }
        return std::string{ buffer.data() };
    }

    class TempMediaPath
    {
        public:

            TempMediaPath() :
                path_( std::filesystem::temp_directory_path() /
                       ( std::string{ outputPrefix } +
                         std::to_string( static_cast<long long>( ::getpid() ) ) +
                         "-" +
                         std::to_string(
                             g_next_temp_index.fetch_add( tempIndexStep,
                                                          std::memory_order_relaxed )
                         ) +
                         outputExtension ) )
            {
                std::error_code error;
                std::filesystem::remove( path_, error );
            }

            ~TempMediaPath()
            {
                std::error_code error;
                std::filesystem::remove( path_, error );
            }

            TempMediaPath( const TempMediaPath& ) = delete;
            TempMediaPath&
            operator=( const TempMediaPath& ) = delete;
            TempMediaPath( TempMediaPath&& )  = delete;
            TempMediaPath&
            operator=( TempMediaPath&& ) = delete;

            [[nodiscard]]
            const std::filesystem::path&
            path() const noexcept
            {
                return path_;
            }

            [[nodiscard]]
            std::string
            string() const
            {
                return path_.string();
            }

        private:

            std::filesystem::path path_;
    };

    class RunningReactor
    {
        public:

            RunningReactor() :
                started_future_( started_.get_future() ),
                thread_(
                    [this]
                    {
                        started_.set_value();
                        run_result_ = reactor_.run();
                    }
                )
            {
            }

            ~RunningReactor()
            {
                stop();
            }

            RunningReactor( const RunningReactor& ) = delete;
            RunningReactor&
            operator=( const RunningReactor& ) = delete;
            RunningReactor( RunningReactor&& ) = delete;
            RunningReactor&
            operator=( RunningReactor&& ) = delete;

            [[nodiscard]]
            bool
            wait_until_started()
            {
                return started_future_.wait_for( threadReadyTimeout ) ==
                       std::future_status::ready;
            }

            [[nodiscard]]
            grab::core::Reactor&
            reactor() noexcept
            {
                return reactor_;
            }

            void
            stop()
            {
                reactor_.stop();
                if( thread_.joinable() )
                {
                    thread_.join();
                }
            }

            [[nodiscard]]
            const grab::Result<void>&
            run_result() const noexcept
            {
                return run_result_;
            }

        private:

            grab::core::Reactor reactor_;
            std::promise<void>  started_;
            std::future<void>   started_future_;
            grab::Result<void>  run_result_;
            std::thread         thread_;
    };

    class InputFormat
    {
        public:

            explicit InputFormat( AVFormatContext* context ) noexcept :
                context_( context )
            {
            }

            ~InputFormat()
            {
                if( context_ != nullptr )
                {
                    avformat_close_input( &context_ );
                }
            }

            InputFormat( const InputFormat& ) = delete;
            InputFormat&
            operator=( const InputFormat& ) = delete;
            InputFormat( InputFormat&& )    = delete;
            InputFormat&
            operator=( InputFormat&& ) = delete;

            [[nodiscard]]
            AVFormatContext*
            get() const noexcept
            {
                return context_;
            }

        private:

            AVFormatContext* context_ = nullptr;
    };

    class Packet
    {
        public:

            Packet() :
                packet_( av_packet_alloc() )
            {
            }

            ~Packet()
            {
                if( packet_ != nullptr )
                {
                    av_packet_free( &packet_ );
                }
            }

            Packet( const Packet& ) = delete;
            Packet&
            operator=( const Packet& ) = delete;
            Packet( Packet&& )         = delete;
            Packet&
            operator=( Packet&& ) = delete;

            [[nodiscard]]
            AVPacket*
            get() const noexcept
            {
                return packet_;
            }

        private:

            AVPacket* packet_ = nullptr;
    };

    [[nodiscard]]
    testing::AssertionResult
    file_has_payload( const std::filesystem::path& path )
    {
        std::error_code error;
        if( !std::filesystem::exists( path, error ) )
        {
            return testing::AssertionFailure() << path << " does not exist";
        }
        if( std::filesystem::file_size( path, error ) == 0U )
        {
            return testing::AssertionFailure() << path << " is empty";
        }
        return testing::AssertionSuccess();
    }

    [[nodiscard]]
    testing::AssertionResult
    demuxes_video_packet( const std::filesystem::path& path )
    {
        AVFormatContext*  raw_context = nullptr;
        const std::string path_string = path.string();
        const int         open_result =
            avformat_open_input( &raw_context, path_string.c_str(), nullptr, nullptr );
        if( open_result < libavSuccess )
        {
            return testing::AssertionFailure()
                << "avformat_open_input failed: " << libav_error_message( open_result );
        }
        InputFormat input{ raw_context };

        const int   stream_result = avformat_find_stream_info( input.get(), nullptr );
        if( stream_result < libavSuccess )
        {
            return testing::AssertionFailure() << "avformat_find_stream_info failed: "
                                               << libav_error_message( stream_result );
        }

        int video_stream = noVideoStream;
        for( unsigned int index = 0U; index < input.get()->nb_streams; ++index )
        {
            const AVStream* const stream = input.get()->streams[index];
            if( stream !=
                nullptr &&
                stream->codecpar !=
                nullptr &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
            {
                video_stream          = static_cast<int>( index );
                const AVCodecID codec = stream->codecpar->codec_id;
                if( codec != AV_CODEC_ID_H264 && codec != AV_CODEC_ID_MPEG4 )
                {
                    return testing::AssertionFailure()
                        << "unexpected video codec id " << static_cast<int>( codec );
                }
                break;
            }
        }

        if( video_stream == noVideoStream )
        {
            return testing::AssertionFailure() << "no video stream found";
        }

        Packet packet;
        if( packet.get() == nullptr )
        {
            return testing::AssertionFailure() << "av_packet_alloc failed";
        }

        std::uint32_t video_packets = 0U;
        while( av_read_frame( input.get(), packet.get() ) >= libavSuccess )
        {
            if( packet.get()->stream_index == video_stream )
            {
                ++video_packets;
            }
            av_packet_unref( packet.get() );
            if( video_packets >= requiredPackets )
            {
                return testing::AssertionSuccess();
            }
        }

        return testing::AssertionFailure() << "no video packets found";
    }

    [[nodiscard]]
    testing::AssertionResult
    wait_until_demuxable( const std::filesystem::path& path )
    {
        const auto deadline = std::chrono::steady_clock::now() + recordTimeout;
        testing::AssertionResult last_result = testing::AssertionFailure()
                                            << "recording was never checked";
        while( std::chrono::steady_clock::now() < deadline )
        {
            last_result = demuxes_video_packet( path );
            if( last_result )
            {
                return testing::AssertionSuccess();
            }
            std::this_thread::sleep_for( pollInterval );
        }
        return last_result;
    }

}    // namespace

TEST( Recorder,
      RecordsShortClipToValidFile )
{
    TempMediaPath  path;
    RunningReactor reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    auto recorder = grab::screen::Recorder::start( reactor.reactor(),
                                                   grab::screen::RecordOptions{
                                                       .display    = xvfbDisplay,
                                                       .path       = path.string(),
                                                       .fps        = recordFps,
                                                       .max_frames = shortClipFrames,
                                                   } );
    ASSERT_TRUE( recorder.has_value() ) << recorder.error().message;

    EXPECT_TRUE( wait_until_demuxable( path.path() ) );

    auto stopped = recorder->stop();
    EXPECT_TRUE( stopped.has_value() ) << stopped.error().message;
    EXPECT_TRUE( file_has_payload( path.path() ) );
    EXPECT_TRUE( demuxes_video_packet( path.path() ) );

    reactor.stop();
    EXPECT_TRUE( reactor.run_result().has_value() )
        << reactor.run_result().error().message;
}

TEST( Recorder,
      StopIsIdempotent )
{
    TempMediaPath  path;
    RunningReactor reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    auto recorder = grab::screen::Recorder::start( reactor.reactor(),
                                                   grab::screen::RecordOptions{
                                                       .display    = xvfbDisplay,
                                                       .path       = path.string(),
                                                       .fps        = recordFps,
                                                       .max_frames = unlimitedFrames,
                                                   } );
    ASSERT_TRUE( recorder.has_value() ) << recorder.error().message;

    std::this_thread::sleep_for( manualStopWarmup );

    auto first_stop = recorder->stop();
    EXPECT_TRUE( first_stop.has_value() ) << first_stop.error().message;
    auto second_stop = recorder->stop();
    EXPECT_TRUE( second_stop.has_value() ) << second_stop.error().message;
    EXPECT_TRUE( file_has_payload( path.path() ) );
    EXPECT_TRUE( demuxes_video_packet( path.path() ) );

    reactor.stop();
    EXPECT_TRUE( reactor.run_result().has_value() )
        << reactor.run_result().error().message;
}

TEST( Recorder,
      StartFailsOnBadDisplay )
{
    TempMediaPath  path;
    RunningReactor reactor;
    ASSERT_TRUE( reactor.wait_until_started() );

    auto recorder = grab::screen::Recorder::start( reactor.reactor(),
                                                   grab::screen::RecordOptions{
                                                       .display    = badDisplay,
                                                       .path       = path.string(),
                                                       .fps        = recordFps,
                                                       .max_frames = shortClipFrames,
                                                   } );

    ASSERT_FALSE( recorder.has_value() );
    EXPECT_EQ( recorder.error().code, grab::ErrorCode::DeviceInaccessible );

    std::promise<void> posted;
    auto               posted_future = posted.get_future();
    reactor.reactor().post(
        [&posted]
        {
            posted.set_value();
        }
    );
    EXPECT_EQ( posted_future.wait_for( postTimeout ), std::future_status::ready );

    reactor.stop();
    EXPECT_TRUE( reactor.run_result().has_value() )
        << reactor.run_result().error().message;
}
