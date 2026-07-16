#include "core/reactor.hpp"
#include "drivers/desktop/x11/record.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "grab/capture.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "kernel/capture/pacing_governor.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace grab::screen
{
    namespace
    {

        constexpr std::uint32_t noFrameLimit          = 0U;
        constexpr std::uint32_t noFramesWritten       = 0U;
        constexpr std::int64_t  noPacketDuration      = 0;
        constexpr std::int64_t  singleFrameDuration   = 1;
        constexpr std::int64_t  firstPresentationTime = 0;
        constexpr std::int64_t  defaultBitRate        = 2'000'000;
        constexpr int           libavSuccess          = 0;
        constexpr int           noBFrames             = 0;
        constexpr int           frameAlignment        = 32;
        constexpr int           scaleSourcePlane      = 0;
        constexpr int           noOutputContextFlags  = 0;
        constexpr auto          noTimerDelay          = std::chrono::nanoseconds{ 0 };
        constexpr AVPixelFormat encoderPixelFormat    = AV_PIX_FMT_YUV420P;
        constexpr AVCodecID     primaryCodec          = AV_CODEC_ID_H264;
        constexpr AVCodecID     fallbackCodec         = AV_CODEC_ID_MPEG4;
        constexpr const char*   h264PresetName        = "preset";
        constexpr const char*   h264PresetValue       = "ultrafast";
        constexpr const char*   h264TuneName          = "tune";
        constexpr const char*   h264TuneValue         = "zerolatency";

        struct EncoderSelection
        {
                AVCodecContext* context = nullptr;
        };

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

        [[nodiscard]]
        grab::Error
        make_error( grab::ErrorCode code,
                    std::string     message )
        {
            return grab::Error{
                .code       = code,
                .message    = std::move( message ),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        grab::Error
        libav_error( grab::ErrorCode  code,
                     std::string_view step,
                     int              libav_code )
        {
            return make_error(
                code,
                std::string{ step } + ": " + libav_error_message( libav_code )
            );
        }

        [[nodiscard]]
        grab::Error
        exception_error( std::string_view      step,
                         const std::exception& exception )
        {
            return make_error( grab::ErrorCode::InternalFault,
                               std::string{ step } + ": " + exception.what() );
        }

        [[nodiscard]]
        grab::Result<int>
        checked_int( std::uint32_t    value,
                     std::string_view name )
        {
            if( value > static_cast<std::uint32_t>( std::numeric_limits<int>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   std::string{ name } + " exceeds libav limits" );
            }
            return static_cast<int>( value );
        }

        [[nodiscard]]
        grab::Result<AVPixelFormat>
        source_pixel_format( grab::PixelFormat format )
        {
            switch( format )
            {
                case grab::PixelFormat::Bgra :
                    return AV_PIX_FMT_BGRA;
                case grab::PixelFormat::Rgba :
                    return AV_PIX_FMT_RGBA;
                case grab::PixelFormat::Rgb :
                    return AV_PIX_FMT_RGB24;
                case grab::PixelFormat::Bgr :
                    return AV_PIX_FMT_BGR24;
                case grab::PixelFormat::Gray :
                    return AV_PIX_FMT_GRAY8;
            }

            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "unsupported capture pixel format" );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_frame_storage( const grab::Image& image )
        {
            if( image.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "recording frame must be non-empty" );
            }

            const auto min_stride =
                static_cast<std::size_t>( image.width ) *
                static_cast<std::size_t>( grab::bytes_per_pixel( image.format ) );
            if( image.stride < min_stride )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "capture frame stride is shorter than its pixels" );
            }

            const auto required_size = static_cast<std::size_t>( image.stride ) *
                                       static_cast<std::size_t>( image.height );
            if( image.pixels.size() < required_size )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "capture frame storage is shorter than expected" );
            }

            if( image.stride >
                static_cast<std::uint32_t>( std::numeric_limits<int>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "capture frame stride exceeds libav limits" );
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<AVFormatContext*>
        allocate_output_context( const std::string& path )
        {
            AVFormatContext* context = nullptr;
            const int        result  = avformat_alloc_output_context2( &context,
                                                                       nullptr,
                                                                       nullptr,
                                                                       path.c_str() );
            if( result < libavSuccess || context == nullptr )
            {
                if( result < libavSuccess )
                {
                    return std::unexpected(
                        libav_error( grab::ErrorCode::InvalidArgument,
                                     "guess output media format",
                                     result )
                    );
                }
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "could not guess output media format" );
            }
            return context;
        }

        [[nodiscard]]
        grab::Result<AVCodecContext*>
        open_encoder_context( AVCodecID     codec_id,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t fps,
                              bool          global_header )
        {
            const AVCodec* const codec = avcodec_find_encoder( codec_id );
            if( codec == nullptr )
            {
                return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                                   "requested libav encoder is unavailable" );
            }

            auto* context = avcodec_alloc_context3( codec );
            if( context == nullptr )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "could not allocate libav encoder context" );
            }

            auto width_int = checked_int( width, "recording frame width" );
            if( !width_int.has_value() )
            {
                avcodec_free_context( &context );
                return std::unexpected( std::move( width_int.error() ) );
            }

            auto height_int = checked_int( height, "recording frame height" );
            if( !height_int.has_value() )
            {
                avcodec_free_context( &context );
                return std::unexpected( std::move( height_int.error() ) );
            }

            auto fps_int = checked_int( fps, "recording frame rate" );
            if( !fps_int.has_value() )
            {
                avcodec_free_context( &context );
                return std::unexpected( std::move( fps_int.error() ) );
            }

            context->codec_id     = codec_id;
            context->codec_type   = AVMEDIA_TYPE_VIDEO;
            context->bit_rate     = defaultBitRate;
            context->width        = *width_int;
            context->height       = *height_int;
            context->time_base    = AVRational{ .num = 1, .den = *fps_int };
            context->framerate    = AVRational{ .num = *fps_int, .den = 1 };
            context->gop_size     = *fps_int;
            context->max_b_frames = noBFrames;
            context->pix_fmt      = encoderPixelFormat;
            if( global_header )
            {
                context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            if( codec_id == AV_CODEC_ID_H264 )
            {
                const int preset_result = av_opt_set( context->priv_data,
                                                      h264PresetName,
                                                      h264PresetValue,
                                                      noOutputContextFlags );
                const int tune_result   = av_opt_set( context->priv_data,
                                                      h264TuneName,
                                                      h264TuneValue,
                                                      noOutputContextFlags );
                static_cast<void>( preset_result );
                static_cast<void>( tune_result );
            }

            const int open_result = avcodec_open2( context, codec, nullptr );
            if( open_result < libavSuccess )
            {
                avcodec_free_context( &context );
                return std::unexpected( libav_error( grab::ErrorCode::ProviderFailed,
                                                     "open libav encoder",
                                                     open_result ) );
            }

            return context;
        }

        [[nodiscard]]
        grab::Result<EncoderSelection>
        open_preferred_encoder( std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t fps,
                                bool          global_header )
        {
            auto primary =
                open_encoder_context( primaryCodec, width, height, fps, global_header );
            if( primary.has_value() )
            {
                return EncoderSelection{
                    .context = *primary,
                };
            }

            auto fallback =
                open_encoder_context( fallbackCodec, width, height, fps, global_header );
            if( fallback.has_value() )
            {
                return EncoderSelection{
                    .context = *fallback,
                };
            }

            return std::unexpected(
                make_error( grab::ErrorCode::CapabilityUnavailable,
                            "no supported libav video encoder is available: " +
                                primary.error().message +
                                "; fallback: " +
                                fallback.error().message )
            );
        }

        [[nodiscard]]
        bool
        format_needs_file_io( const AVOutputFormat& format ) noexcept
        {
            return ( format.flags & AVFMT_NOFILE ) == noOutputContextFlags;
        }

        [[nodiscard]]
        bool
        format_needs_global_header( const AVOutputFormat& format ) noexcept
        {
            return ( format.flags & AVFMT_GLOBALHEADER ) != noOutputContextFlags;
        }

    }    // namespace

    struct Recorder::State
    {
            State( grab::core::Reactor&                         reactor_value,
                   grab::drivers::desktop::x11::X11CaptureRoute route_value,
                   RecordOptions options_value ) noexcept :
                reactor( &reactor_value ),
                route( std::move( route_value ) ),
                options( std::move( options_value ) )
            {
            }

            ~State()
            {
                try
                {
                    const std::scoped_lock lock( mutex );
                    if( !finalized )
                    {
                        auto result = finish_locked();
                        static_cast<void>( result );
                    }
                    cleanup_locked();
                }
                catch( ... )
                {
                    return;
                }
            }

            State( const State& ) = delete;
            State&
            operator=( const State& ) = delete;
            State( State&& )          = delete;
            State&
            operator=( State&& ) = delete;

            static void
            schedule_next_timer(
                const std::shared_ptr<State>&         state,
                std::chrono::steady_clock::time_point deadline
            ) noexcept;

            static void
            handle_timer( const std::shared_ptr<State>& state ) noexcept;

            [[nodiscard]]
            grab::Result<void>
            initialize( const grab::Image& first_frame )
            {
                const std::scoped_lock lock( mutex );

                if( options.path.empty() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "recording output path must be non-empty" );
                }

                auto governor_result =
                    grab::kernel::capture::PacingGovernor::for_fps( options.fps );
                if( !governor_result.has_value() )
                {
                    return std::unexpected( std::move( governor_result.error() ) );
                }
                governor     = *governor_result;

                auto storage = validate_frame_storage( first_frame );
                if( !storage.has_value() )
                {
                    return storage;
                }

                auto source_format = source_pixel_format( first_frame.format );
                if( !source_format.has_value() )
                {
                    return std::unexpected( std::move( source_format.error() ) );
                }

                width       = first_frame.width;
                height      = first_frame.height;
                source_pix  = *source_format;
                max_frames  = options.max_frames;
                next_pts    = firstPresentationTime;
                frame_count = noFramesWritten;

                auto output = allocate_output_context( options.path );
                if( !output.has_value() )
                {
                    return std::unexpected( std::move( output.error() ) );
                }
                format_context = *output;

                auto encoder   = open_preferred_encoder(
                    width,
                    height,
                    options.fps,
                    format_needs_global_header( *format_context->oformat )
                );
                if( !encoder.has_value() )
                {
                    return std::unexpected( std::move( encoder.error() ) );
                }
                codec_context = encoder->context;

                stream        = avformat_new_stream( format_context, nullptr );
                if( stream == nullptr )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "could not allocate output video stream" );
                }
                stream->time_base = codec_context->time_base;

                const int parameters_result =
                    avcodec_parameters_from_context( stream->codecpar, codec_context );
                if( parameters_result < libavSuccess )
                {
                    return std::unexpected(
                        libav_error( grab::ErrorCode::ProviderFailed,
                                     "copy encoder parameters to output stream",
                                     parameters_result )
                    );
                }

                auto frame_result = allocate_frame_locked();
                if( !frame_result.has_value() )
                {
                    return frame_result;
                }

                auto scaler_result = allocate_scaler_locked();
                if( !scaler_result.has_value() )
                {
                    return scaler_result;
                }

                packet = av_packet_alloc();
                if( packet == nullptr )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "could not allocate libav packet" );
                }

                auto opened = open_container_locked();
                if( !opened.has_value() )
                {
                    return opened;
                }

                running   = true;
                finalized = false;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            record_one_frame()
            {
                const std::scoped_lock lock( mutex );
                if( !running || finalized )
                {
                    return {};
                }

                auto captured_frame = route.capture_display();
                if( !captured_frame.has_value() )
                {
                    return std::unexpected( std::move( captured_frame.error() ) );
                }

                auto converted = convert_frame_locked( captured_frame->image );
                if( !converted.has_value() )
                {
                    return converted;
                }

                auto encoded = encode_frame_locked( frame );
                if( !encoded.has_value() )
                {
                    return encoded;
                }

                ++frame_count;
                ++next_pts;

                if( max_frames != noFrameLimit && frame_count >= max_frames )
                {
                    auto finished = finish_locked();
                    if( !finished.has_value() )
                    {
                        return finished;
                    }
                }

                return {};
            }

            void
            remember_async_error( grab::Error error ) noexcept
            {
                try
                {
                    const std::scoped_lock lock( mutex );
                    remember_error_locked( std::move( error ) );
                    if( !finalized )
                    {
                        auto result = finish_locked();
                        if( !result.has_value() )
                        {
                            remember_error_locked( std::move( result.error() ) );
                        }
                    }
                }
                catch( ... )
                {
                    return;
                }
            }

            [[nodiscard]]
            std::chrono::steady_clock::time_point
            next_capture_deadline( std::chrono::steady_clock::time_point from ) const
            {
                const std::scoped_lock lock( mutex );
                if( !governor.has_value() )
                {
                    return from;
                }
                return governor->next_deadline( from );
            }

            [[nodiscard]]
            grab::core::Reactor*
            active_reactor() const
            {
                const std::scoped_lock lock( mutex );
                if( !running || finalized )
                {
                    return nullptr;
                }
                return reactor;
            }

            [[nodiscard]]
            grab::Result<void>
            stop()
            {
                const std::scoped_lock lock( mutex );
                if( !finalized )
                {
                    const auto result = finish_locked();
                    if( !result.has_value() )
                    {
                        remember_error_locked( result.error() );
                    }
                }

                if( async_error.has_value() )
                {
                    return std::unexpected( *async_error );
                }
                return {};
            }

        private:

            [[nodiscard]]
            grab::Result<void>
            open_container_locked()
            {
                if( format_needs_file_io( *format_context->oformat ) )
                {
                    const int open_result = avio_open( &format_context->pb,
                                                       options.path.c_str(),
                                                       AVIO_FLAG_WRITE );
                    if( open_result < libavSuccess )
                    {
                        return std::unexpected(
                            libav_error( grab::ErrorCode::DeviceInaccessible,
                                         "open output media file",
                                         open_result )
                        );
                    }
                }

                const int header_result =
                    avformat_write_header( format_context, nullptr );
                if( header_result < libavSuccess )
                {
                    return std::unexpected( libav_error( grab::ErrorCode::ProviderFailed,
                                                         "write output media header",
                                                         header_result ) );
                }
                header_written = true;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            allocate_frame_locked()
            {
                frame = av_frame_alloc();
                if( frame == nullptr )
                {
                    return grab::fail( grab::ErrorCode::InternalFault,
                                       "could not allocate libav frame" );
                }

                auto width_int = checked_int( width, "recording frame width" );
                if( !width_int.has_value() )
                {
                    return std::unexpected( std::move( width_int.error() ) );
                }
                auto height_int = checked_int( height, "recording frame height" );
                if( !height_int.has_value() )
                {
                    return std::unexpected( std::move( height_int.error() ) );
                }

                frame->format           = static_cast<int>( codec_context->pix_fmt );
                frame->width            = *width_int;
                frame->height           = *height_int;
                const int buffer_result = av_frame_get_buffer( frame, frameAlignment );
                if( buffer_result < libavSuccess )
                {
                    return std::unexpected( libav_error( grab::ErrorCode::InternalFault,
                                                         "allocate libav frame buffers",
                                                         buffer_result ) );
                }
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            allocate_scaler_locked()
            {
                auto width_int = checked_int( width, "recording frame width" );
                if( !width_int.has_value() )
                {
                    return std::unexpected( std::move( width_int.error() ) );
                }
                auto height_int = checked_int( height, "recording frame height" );
                if( !height_int.has_value() )
                {
                    return std::unexpected( std::move( height_int.error() ) );
                }

                scaler = sws_getContext( *width_int,
                                         *height_int,
                                         source_pix,
                                         *width_int,
                                         *height_int,
                                         codec_context->pix_fmt,
                                         SWS_BILINEAR,
                                         nullptr,
                                         nullptr,
                                         nullptr );
                if( scaler == nullptr )
                {
                    return grab::fail( grab::ErrorCode::ProviderFailed,
                                       "could not allocate libav pixel scaler" );
                }
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            convert_frame_locked( const grab::Image& image )
            {
                if( image.width != width || image.height != height )
                {
                    return grab::fail( grab::ErrorCode::GeometryUntrusted,
                                       "recording frame dimensions changed" );
                }

                auto storage = validate_frame_storage( image );
                if( !storage.has_value() )
                {
                    return storage;
                }

                auto pixel_format = source_pixel_format( image.format );
                if( !pixel_format.has_value() )
                {
                    return std::unexpected( std::move( pixel_format.error() ) );
                }
                if( *pixel_format != source_pix )
                {
                    return grab::fail( grab::ErrorCode::GeometryUntrusted,
                                       "recording frame pixel format changed" );
                }

                const int writable_result = av_frame_make_writable( frame );
                if( writable_result < libavSuccess )
                {
                    return std::unexpected( libav_error( grab::ErrorCode::ProviderFailed,
                                                         "make libav frame writable",
                                                         writable_result ) );
                }

                using SourcePointer           = const std::uint8_t*;
                const auto* const pixel_bytes = image.pixels.data();
                // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
                const auto* const source = std::bit_cast<SourcePointer>( pixel_bytes );
                const std::array<const std::uint8_t*, AV_NUM_DATA_POINTERS> source_data{
                    source,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                };
                const std::array<int, AV_NUM_DATA_POINTERS> source_linesize{
                    static_cast<int>( image.stride ),
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                };
                auto* const       destination_data     = std::begin( frame->data );
                const auto* const destination_linesize = std::begin( frame->linesize );
                const int         scaled_height = sws_scale( scaler,
                                                             source_data.data(),
                                                             source_linesize.data(),
                                                             scaleSourcePlane,
                                                             frame->height,
                                                             destination_data,
                                                             destination_linesize );
                if( scaled_height != frame->height )
                {
                    return grab::fail(
                        grab::ErrorCode::ProviderFailed,
                        "libav pixel scaler did not convert a full frame"
                    );
                }

                frame->pts = next_pts;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            encode_frame_locked( const AVFrame* input_frame )
            {
                const int send_result = avcodec_send_frame( codec_context, input_frame );
                if( send_result < libavSuccess )
                {
                    return std::unexpected( libav_error( grab::ErrorCode::ProviderFailed,
                                                         "send frame to libav encoder",
                                                         send_result ) );
                }

                return write_available_packets_locked();
            }

            [[nodiscard]]
            grab::Result<void>
            write_available_packets_locked()
            {
                while( true )
                {
                    const int receive_result =
                        avcodec_receive_packet( codec_context, packet );
                    if( receive_result ==
                        AVERROR( EAGAIN ) ||
                        receive_result == AVERROR_EOF )
                    {
                        return {};
                    }
                    if( receive_result < libavSuccess )
                    {
                        return std::unexpected(
                            libav_error( grab::ErrorCode::ProviderFailed,
                                         "receive packet from libav encoder",
                                         receive_result )
                        );
                    }

                    if( packet->duration == noPacketDuration )
                    {
                        packet->duration = singleFrameDuration;
                    }
                    av_packet_rescale_ts( packet,
                                          codec_context->time_base,
                                          stream->time_base );
                    packet->stream_index = stream->index;
                    const int write_result =
                        av_interleaved_write_frame( format_context, packet );
                    if( write_result < libavSuccess )
                    {
                        return std::unexpected(
                            libav_error( grab::ErrorCode::ProviderFailed,
                                         "write encoded video packet",
                                         write_result )
                        );
                    }
                }
            }

            [[nodiscard]]
            grab::Result<void>
            finish_locked()
            {
                running = false;

                std::optional<grab::Error> finish_error;
                if( codec_context != nullptr && header_written )
                {
                    const auto flushed = encode_frame_locked( nullptr );
                    if( !flushed.has_value() )
                    {
                        finish_error = flushed.error();
                    }
                }

                if( format_context != nullptr && header_written )
                {
                    const int trailer_result = av_write_trailer( format_context );
                    if( trailer_result < libavSuccess && !finish_error.has_value() )
                    {
                        finish_error = libav_error( grab::ErrorCode::ProviderFailed,
                                                    "write output media trailer",
                                                    trailer_result );
                    }
                }

                finalized = true;
                cleanup_locked();

                if( finish_error.has_value() )
                {
                    return std::unexpected( *finish_error );
                }
                return {};
            }

            void
            cleanup_locked() noexcept
            {
                if( packet != nullptr )
                {
                    av_packet_free( &packet );
                }
                if( frame != nullptr )
                {
                    av_frame_free( &frame );
                }
                if( scaler != nullptr )
                {
                    sws_freeContext( scaler );
                    scaler = nullptr;
                }
                if( codec_context != nullptr )
                {
                    avcodec_free_context( &codec_context );
                }
                if( format_context != nullptr )
                {
                    if( format_context->pb !=
                        nullptr &&
                        format_needs_file_io( *format_context->oformat ) )
                    {
                        const int close_result = avio_closep( &format_context->pb );
                        static_cast<void>( close_result );
                    }
                    avformat_free_context( format_context );
                    format_context = nullptr;
                }
                stream         = nullptr;
                header_written = false;
            }

            void
            remember_error_locked( grab::Error error )
            {
                if( !async_error.has_value() )
                {
                    async_error = std::move( error );
                }
            }

            mutable std::mutex                                   mutex;
            grab::core::Reactor*                                 reactor = nullptr;
            grab::drivers::desktop::x11::X11CaptureRoute         route;
            RecordOptions                                        options;
            std::optional<grab::kernel::capture::PacingGovernor> governor;
            std::uint32_t                                        width  = 0U;
            std::uint32_t                                        height = 0U;
            std::uint32_t              max_frames                       = noFrameLimit;
            std::uint32_t              frame_count    = noFramesWritten;
            std::int64_t               next_pts       = firstPresentationTime;
            AVPixelFormat              source_pix     = AV_PIX_FMT_NONE;
            AVFormatContext*           format_context = nullptr;
            AVCodecContext*            codec_context  = nullptr;
            AVStream*                  stream         = nullptr;
            SwsContext*                scaler         = nullptr;
            AVFrame*                   frame          = nullptr;
            AVPacket*                  packet         = nullptr;
            bool                       running        = false;
            bool                       finalized      = true;
            bool                       header_written = false;
            std::optional<grab::Error> async_error;
    };

    void
    Recorder::State::schedule_next_timer(
        const std::shared_ptr<State>&         state,
        std::chrono::steady_clock::time_point deadline
    ) noexcept
    {
        try
        {
            grab::core::Reactor* const reactor = state->active_reactor();
            if( reactor == nullptr )
            {
                return;
            }

            const auto now   = std::chrono::steady_clock::now();
            const auto delay = deadline > now ? deadline - now : noTimerDelay;
            static_cast<void>( reactor->add_timer( delay,
                                                   [state]
                                                   {
                                                       State::handle_timer( state );
                                                   } ) );
        }
        catch( const std::exception& exception )
        {
            state->remember_async_error( exception_error( "schedule recording timer",
                                                          exception ) );
        }
        catch( ... )
        {
            state->remember_async_error(
                make_error( grab::ErrorCode::InternalFault,
                            "schedule recording timer: unknown exception" )
            );
        }
    }

    void
    Recorder::State::handle_timer( const std::shared_ptr<State>& state ) noexcept
    {
        try
        {
            const auto fired_at = std::chrono::steady_clock::now();
            auto       recorded = state->record_one_frame();
            if( !recorded.has_value() )
            {
                state->remember_async_error( std::move( recorded.error() ) );
                return;
            }
            State::schedule_next_timer( state,
                                        state->next_capture_deadline( fired_at ) );
        }
        catch( const std::exception& exception )
        {
            state->remember_async_error( exception_error( "record display frame",
                                                          exception ) );
        }
        catch( ... )
        {
            state->remember_async_error(
                make_error( grab::ErrorCode::InternalFault,
                            "record display frame: unknown exception" )
            );
        }
    }

    Recorder::Recorder( std::shared_ptr<State> state ) noexcept :
        state_( std::move( state ) )
    {
    }

    Recorder::~Recorder()
    {
        if( state_ != nullptr )
        {
            const auto result = state_->stop();
            static_cast<void>( result );
        }
    }

    Recorder::Recorder( Recorder&& other ) noexcept :
        state_( std::move( other.state_ ) )
    {
    }

    Recorder&
    Recorder::operator=( Recorder&& other ) noexcept
    {
        if( this != &other )
        {
            if( state_ != nullptr )
            {
                const auto result = state_->stop();
                static_cast<void>( result );
            }
            state_ = std::move( other.state_ );
        }
        return *this;
    }

    grab::Result<Recorder>
    Recorder::start( grab::core::Reactor& reactor,
                     const RecordOptions& options )
    {
        const char* const display =
            options.display.empty() ? nullptr : options.display.c_str();
        auto route = grab::drivers::desktop::x11::X11CaptureRoute::open( display );
        if( !route.has_value() )
        {
            return std::unexpected( std::move( route.error() ) );
        }

        auto first_frame = route->capture_display();
        if( !first_frame.has_value() )
        {
            return std::unexpected( std::move( first_frame.error() ) );
        }

        auto moved_route = std::move( *route );
        auto state =
            std::make_shared<State>( reactor, std::move( moved_route ), options );
        auto initialized = state->initialize( first_frame->image );
        if( !initialized.has_value() )
        {
            return std::unexpected( std::move( initialized.error() ) );
        }

        State::schedule_next_timer(
            state,
            state->next_capture_deadline( std::chrono::steady_clock::now() )
        );
        return Recorder{ std::move( state ) };
    }

    grab::Result<void>
    Recorder::stop()
    {
        if( state_ == nullptr )
        {
            return {};
        }
        return state_->stop();
    }

}    // namespace grab::screen
