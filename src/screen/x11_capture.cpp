#include "grab/image.hpp"
#include "grab/result.hpp"
#include "screen/x11_capture.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
// clang-format off
#include <sys/ipc.h>
#include <sys/shm.h>
// clang-format on
#include <utility>
#include <vector>
#include <xcb/composite.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::screen
{
    namespace
    {

        constexpr int              kXcbOk                  = 0;
        constexpr int              kXcbFlushFailed         = 0;
        constexpr std::intptr_t    kSystemCallFailed       = -1;
        constexpr int              kInvalidSharedMemoryId  = -1;
        constexpr std::uint8_t     kXcbFalse               = 0U;
        constexpr std::uint8_t     kBitsPerByte            = 8U;
        constexpr std::uint8_t     kTwentyFourBitDepth     = 24U;
        constexpr std::uint8_t     kThirtyTwoBitDepth      = 32U;
        constexpr int              kEightBitDepth          = 8;
        constexpr std::uint8_t     kFourBytesPerPixel      = 4U;
        constexpr std::uint8_t     kOpaqueAlpha            = 0XFFU;
        constexpr std::uint8_t     kFullChannelValue       = 0XFFU;
        constexpr std::uint8_t     kX11SuccessResponse     = 1U;
        constexpr int              kUnixUserReadWrite      = 0600;
        constexpr std::uint32_t    kAllPlanes              = 0XFF'FF'FF'FFU;
        constexpr std::uint32_t    kNoOffset               = 0U;
        constexpr std::uint32_t    kNoChannels             = 0U;

        constexpr std::string_view kCompositeExtensionName = "Composite";
        constexpr std::string_view kShmExtensionName       = "MIT-SHM";

        template<typename T>
        using XcbOwned = std::unique_ptr<T, decltype( &std::free )>;

        using XcbConnection =
            std::unique_ptr<xcb_connection_t, decltype( &xcb_disconnect )>;

        struct ScreenInfo
        {
                std::uint32_t root             = 0U;
                std::uint16_t width            = 0U;
                std::uint16_t height           = 0U;
                std::uint8_t  root_depth       = 0U;
                std::uint32_t root_visual      = 0U;
                std::uint8_t  image_byte_order = 0U;
        };

        struct PixmapFormat
        {
                std::uint8_t depth          = 0U;
                std::uint8_t bits_per_pixel = 0U;
                std::uint8_t scanline_pad   = 0U;
        };

        struct VisualMasks
        {
                std::uint32_t red   = 0U;
                std::uint32_t green = 0U;
                std::uint32_t blue  = 0U;
        };

        struct CaptureSpec
        {
                std::uint32_t drawable = 0U;
                std::int16_t  x        = 0;
                std::int16_t  y        = 0;
                std::uint16_t width    = 0U;
                std::uint16_t height   = 0U;
                std::uint8_t  depth    = 0U;
        };

        struct CapturedBuffer
        {
                std::vector<std::byte> pixels;
                std::uint8_t           depth  = 0U;
                std::uint32_t          visual = 0U;
        };

        template<typename T>
        [[nodiscard]]
        XcbOwned<T>
        take_xcb_owned( T* pointer ) noexcept
        {
            return XcbOwned<T>{ pointer, &std::free };
        }

        [[nodiscard]]
        std::string
        errno_message( std::string_view operation )
        {
            return std::string{ operation } +
                   " failed: " +
                   std::error_code{ errno, std::generic_category() }.message();
        }

        [[nodiscard]]
        grab::ErrorCode
        window_error_code( const xcb_generic_error_t& error ) noexcept
        {
            if( error.error_code == XCB_WINDOW )
            {
                return grab::ErrorCode::stale_window;
            }
            if( error.error_code == XCB_MATCH || error.error_code == XCB_DRAWABLE )
            {
                return grab::ErrorCode::window_not_found;
            }
            return grab::ErrorCode::protocol_error;
        }

        [[nodiscard]]
        grab::Result<void>
        fail_if_connection_closed( const xcb_connection_t* connection )
        {
            if( connection == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB capture connection is not open" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        flush_connection( xcb_connection_t* connection,
                          std::string_view  operation )
        {
            if( xcb_flush( connection ) <=
                kXcbFlushFailed ||
                xcb_connection_has_error( connection ) != kXcbOk )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   std::string{ operation } + " flush failed" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   std::string{ operation } +
                                       " failed with X error " +
                                       std::to_string( error->error_code ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        check_window_request( xcb_connection_t* connection,
                              xcb_void_cookie_t cookie,
                              std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( window_error_code( *error ),
                                   std::string{ operation } +
                                       " failed with X error " +
                                       std::to_string( error->error_code ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        require_extension( xcb_connection_t* connection,
                           std::string_view  name )
        {
            const auto extension_cookie =
                xcb_query_extension( connection,
                                     static_cast<std::uint16_t>( name.size() ),
                                     name.data() );
            xcb_generic_error_t* raw_extension_error = nullptr;
            const auto           extension_reply =
                take_xcb_owned( xcb_query_extension_reply( connection,
                                                           extension_cookie,
                                                           &raw_extension_error ) );
            const auto extension_error = take_xcb_owned( raw_extension_error );
            if( extension_error !=
                nullptr ||
                extension_reply ==
                nullptr ||
                extension_reply->present == 0U )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   std::string{ name } + " extension is unavailable" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        require_composite( xcb_connection_t* connection )
        {
            auto extension = require_extension( connection, kCompositeExtensionName );
            if( !extension.has_value() )
            {
                return extension;
            }

            xcb_generic_error_t* raw_error = nullptr;
            const auto reply = take_xcb_owned( xcb_composite_query_version_reply(
                connection,
                xcb_composite_query_version( connection,
                                             XCB_COMPOSITE_MAJOR_VERSION,
                                             XCB_COMPOSITE_MINOR_VERSION ),
                &raw_error
            ) );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "Composite version query failed" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        require_shm( xcb_connection_t* connection )
        {
            auto extension = require_extension( connection, kShmExtensionName );
            if( !extension.has_value() )
            {
                return extension;
            }

            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_shm_query_version_reply( connection,
                                             xcb_shm_query_version( connection ),
                                             &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "MIT-SHM version query failed" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<ScreenInfo>
        default_screen_info( xcb_connection_t* connection,
                             int               screen_index )
        {
            const xcb_setup_t* const setup = xcb_get_setup( connection );
            if( setup == nullptr || setup->status != kX11SuccessResponse )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB setup is unavailable" );
            }

            xcb_screen_iterator_t iterator = xcb_setup_roots_iterator( setup );
            for( int current_screen = 0;
                 current_screen < screen_index && iterator.rem > 0;
                 ++current_screen )
            {
                xcb_screen_next( &iterator );
            }

            if( iterator.data == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB default screen is unavailable" );
            }

            const xcb_screen_t& screen = *iterator.data;
            return ScreenInfo{
                .root             = screen.root,
                .width            = screen.width_in_pixels,
                .height           = screen.height_in_pixels,
                .root_depth       = screen.root_depth,
                .root_visual      = screen.root_visual,
                .image_byte_order = setup->image_byte_order,
            };
        }

        [[nodiscard]]
        grab::Result<PixmapFormat>
        pixmap_format_for_depth( xcb_connection_t* connection,
                                 std::uint8_t      depth )
        {
            const xcb_setup_t* const setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB setup is unavailable" );
            }

            xcb_format_iterator_t iterator = xcb_setup_pixmap_formats_iterator( setup );
            while( iterator.rem > 0 )
            {
                if( iterator.data != nullptr && iterator.data->depth == depth )
                {
                    return PixmapFormat{
                        .depth          = iterator.data->depth,
                        .bits_per_pixel = iterator.data->bits_per_pixel,
                        .scanline_pad   = iterator.data->scanline_pad,
                    };
                }
                xcb_format_next( &iterator );
            }

            return grab::fail( grab::ErrorCode::protocol_error,
                               "XCB pixmap format is unavailable for depth " +
                                   std::to_string( depth ) );
        }

        [[nodiscard]]
        grab::Result<VisualMasks>
        visual_masks_for_id( xcb_connection_t* connection,
                             std::uint32_t     visual )
        {
            const xcb_setup_t* const setup = xcb_get_setup( connection );
            if( setup == nullptr )
            {
                return grab::fail( grab::ErrorCode::device_inaccessible,
                                   "XCB setup is unavailable" );
            }

            xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator( setup );
            while( screen_iterator.rem > 0 )
            {
                if( screen_iterator.data != nullptr )
                {
                    xcb_depth_iterator_t depth_iterator =
                        xcb_screen_allowed_depths_iterator( screen_iterator.data );
                    while( depth_iterator.rem > 0 )
                    {
                        if( depth_iterator.data != nullptr )
                        {
                            xcb_visualtype_iterator_t visual_iterator =
                                xcb_depth_visuals_iterator( depth_iterator.data );
                            while( visual_iterator.rem > 0 )
                            {
                                if( visual_iterator.data !=
                                    nullptr &&
                                    visual_iterator.data->visual_id == visual )
                                {
                                    return VisualMasks{
                                        .red   = visual_iterator.data->red_mask,
                                        .green = visual_iterator.data->green_mask,
                                        .blue  = visual_iterator.data->blue_mask,
                                    };
                                }
                                xcb_visualtype_next( &visual_iterator );
                            }
                        }
                        xcb_depth_next( &depth_iterator );
                    }
                }
                xcb_screen_next( &screen_iterator );
            }

            return grab::fail( grab::ErrorCode::protocol_error,
                               "XCB visual masks are unavailable for visual " +
                                   std::to_string( visual ) );
        }

        [[nodiscard]]
        std::size_t
        padded_scanline_bytes( std::uint16_t width,
                               std::uint8_t  bits_per_pixel,
                               std::uint8_t  scanline_pad ) noexcept
        {
            const auto bits_per_row = static_cast<std::size_t>( width ) *
                                      static_cast<std::size_t>( bits_per_pixel );
            const auto pad          = static_cast<std::size_t>( scanline_pad );
            const auto padded_bits =
                pad == 0U ? bits_per_row : ( ( bits_per_row + pad - 1U ) / pad ) * pad;
            return padded_bits / kBitsPerByte;
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        capture_size_bytes( std::uint16_t       width,
                            std::uint16_t       height,
                            const PixmapFormat& format )
        {
            const auto stride   = padded_scanline_bytes( width,
                                                         format.bits_per_pixel,
                                                         format.scanline_pad );
            const auto max_size = std::numeric_limits<std::size_t>::max();
            if( height != 0U && stride > max_size / static_cast<std::size_t>( height ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "X11 capture size overflowed" );
            }
            return stride * static_cast<std::size_t>( height );
        }

        [[nodiscard]]
        std::uint8_t
        source_bytes_per_pixel( std::uint8_t bits_per_pixel ) noexcept
        {
            const auto bits = static_cast<std::uint16_t>( bits_per_pixel );
            return static_cast<std::uint8_t>(
                ( bits + static_cast<std::uint16_t>( kBitsPerByte - 1U ) ) / kBitsPerByte
            );
        }

        [[nodiscard]]
        std::uint32_t
        read_pixel( const std::vector<std::byte>& source,
                    std::size_t                   offset,
                    std::uint8_t                  byte_count,
                    std::uint8_t                  image_byte_order ) noexcept
        {
            std::uint32_t value = 0U;
            if( image_byte_order == XCB_IMAGE_ORDER_MSB_FIRST )
            {
                for( std::uint8_t index = 0U; index < byte_count; ++index )
                {
                    value <<= kBitsPerByte;
                    value  |= static_cast<std::uint32_t>(
                        std::to_integer<std::uint8_t>( source.at( offset + index ) )
                    );
                }
                return value;
            }

            for( std::uint8_t index = 0U; index < byte_count; ++index )
            {
                value |= static_cast<std::uint32_t>(
                             std::to_integer<std::uint8_t>( source.at( offset + index ) )
                         )
                      << ( static_cast<std::uint32_t>( index ) * kBitsPerByte );
            }
            return value;
        }

        [[nodiscard]]
        std::uint8_t
        channel_from_mask( std::uint32_t pixel,
                           std::uint32_t mask ) noexcept
        {
            if( mask == kNoChannels )
            {
                return 0U;
            }

            const auto shift = std::countr_zero( mask );
            const auto width = std::popcount( mask );
            const auto raw   = ( pixel & mask ) >> shift;
            if( std::cmp_greater_equal( width, kEightBitDepth ) )
            {
                return static_cast<std::uint8_t>(
                    raw >> static_cast<std::uint32_t>( width - kEightBitDepth )
                );
            }

            const auto max_raw =
                ( std::uint32_t{ 1U } << static_cast<std::uint32_t>( width ) ) - 1U;
            return static_cast<std::uint8_t>( ( raw * kFullChannelValue ) / max_raw );
        }

        [[nodiscard]]
        grab::PixelFormat
        output_format_for_source( std::uint8_t bytes_per_pixel ) noexcept
        {
            if( bytes_per_pixel >= kFourBytesPerPixel )
            {
                return grab::PixelFormat::bgra;
            }
            return grab::PixelFormat::bgr;
        }

        [[nodiscard]]
        std::uint32_t
        output_stride( std::uint16_t     width,
                       grab::PixelFormat format ) noexcept
        {
            return static_cast<std::uint32_t>( width ) * grab::bytes_per_pixel( format );
        }

        [[nodiscard]]
        grab::Result<grab::Image>
        convert_image( xcb_connection_t*     connection,
                       std::uint16_t         width,
                       std::uint16_t         height,
                       std::uint8_t          bits_per_pixel,
                       std::uint8_t          scanline_pad,
                       std::uint8_t          image_byte_order,
                       const CapturedBuffer& captured )
        {
            auto visual_masks = visual_masks_for_id( connection, captured.visual );
            if( !visual_masks.has_value() )
            {
                return std::unexpected( std::move( visual_masks.error() ) );
            }

            const auto source_bpp = source_bytes_per_pixel( bits_per_pixel );
            if( source_bpp == 0U )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB image has zero bytes per pixel" );
            }

            const auto source_stride =
                padded_scanline_bytes( width, bits_per_pixel, scanline_pad );
            const auto required_size =
                source_stride * static_cast<std::size_t>( height );
            if( captured.pixels.size() < required_size )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB image data is shorter than expected" );
            }

            const auto  format = output_format_for_source( source_bpp );
            grab::Image image{
                .width  = width,
                .height = height,
                .stride = output_stride( width, format ),
                .format = format,
                .pixels = {},
            };
            image.pixels.resize( static_cast<std::size_t>( image.stride ) *
                                 static_cast<std::size_t>( image.height ) );

            const auto output_bpp = grab::bytes_per_pixel( image.format );
            for( std::uint32_t row = 0U; row < image.height; ++row )
            {
                const auto source_row_offset =
                    static_cast<std::size_t>( row ) * source_stride;
                const auto output_row_offset =
                    static_cast<std::size_t>( row ) * image.stride;
                for( std::uint32_t column = 0U; column < image.width; ++column )
                {
                    const auto source_pixel_offset =
                        source_row_offset +
                        ( static_cast<std::size_t>( column ) * source_bpp );
                    const auto output_pixel_offset =
                        output_row_offset +
                        ( static_cast<std::size_t>( column ) * output_bpp );
                    const auto pixel = read_pixel( captured.pixels,
                                                   source_pixel_offset,
                                                   source_bpp,
                                                   image_byte_order );

                    image.pixels.at( output_pixel_offset ) = static_cast<std::byte>(
                        channel_from_mask( pixel, visual_masks->blue )
                    );
                    image.pixels.at( output_pixel_offset + 1U ) = static_cast<std::byte>(
                        channel_from_mask( pixel, visual_masks->green )
                    );
                    image.pixels.at( output_pixel_offset + 2U ) =
                        static_cast<std::byte>( channel_from_mask( pixel,
                                                                   visual_masks->red ) );
                    if( output_bpp == kFourBytesPerPixel )
                    {
                        image.pixels.at( output_pixel_offset + 3U ) =
                            static_cast<std::byte>( kOpaqueAlpha );
                    }
                }
            }

            return image;
        }

        class SharedSegment
        {
            public:

                [[nodiscard]]
                static grab::Result<SharedSegment>
                attach( xcb_connection_t* connection,
                        std::size_t       size )
                {
                    if( size ==
                        0U ||
                        size >
                        static_cast<std::size_t>( std::numeric_limits<int>::max() ) )
                    {
                        return grab::fail( grab::ErrorCode::invalid_argument,
                                           "MIT-SHM capture size is invalid" );
                    }

                    const int id =
                        shmget( IPC_PRIVATE, size, IPC_CREAT | kUnixUserReadWrite );
                    if( id == kInvalidSharedMemoryId )
                    {
                        return grab::fail( grab::ErrorCode::device_inaccessible,
                                           errno_message( "shmget" ) );
                    }

                    void* const address = shmat( id, nullptr, 0 );
                    auto* const failed_address =
                        std::bit_cast<void*>( kSystemCallFailed );
                    if( address == failed_address )
                    {
                        const int saved_errno = errno;
                        static_cast<void>( shmctl( id, IPC_RMID, nullptr ) );
                        errno = saved_errno;
                        return grab::fail( grab::ErrorCode::device_inaccessible,
                                           errno_message( "shmat" ) );
                    }

                    const auto segment =
                        static_cast<xcb_shm_seg_t>( xcb_generate_id( connection ) );
                    auto attach_result = check_request(
                        connection,
                        xcb_shm_attach_checked( connection,
                                                segment,
                                                static_cast<std::uint32_t>( id ),
                                                kXcbFalse ),
                        "MIT-SHM attach"
                    );
                    if( !attach_result.has_value() )
                    {
                        static_cast<void>( shmdt( address ) );
                        static_cast<void>( shmctl( id, IPC_RMID, nullptr ) );
                        return std::unexpected( std::move( attach_result.error() ) );
                    }

                    return SharedSegment{ connection, segment, id, address, size };
                }

                ~SharedSegment()
                {
                    cleanup();
                }

                SharedSegment( const SharedSegment& ) = delete;
                SharedSegment&
                operator=( const SharedSegment& ) = delete;

                SharedSegment( SharedSegment&& other ) noexcept :
                    connection_( std::exchange( other.connection_,
                                                nullptr ) ),
                    segment_( std::exchange( other.segment_,
                                             0U ) ),
                    id_( std::exchange( other.id_,
                                        kInvalidSharedMemoryId ) ),
                    address_( std::exchange( other.address_,
                                             nullptr ) ),
                    size_( std::exchange( other.size_,
                                          0U ) )
                {
                }

                SharedSegment&
                operator=( SharedSegment&& other ) noexcept
                {
                    if( this != &other )
                    {
                        cleanup();
                        connection_ = std::exchange( other.connection_, nullptr );
                        segment_    = std::exchange( other.segment_, 0U );
                        id_         = std::exchange( other.id_, kInvalidSharedMemoryId );
                        address_    = std::exchange( other.address_, nullptr );
                        size_       = std::exchange( other.size_, 0U );
                    }
                    return *this;
                }

                [[nodiscard]]
                xcb_shm_seg_t
                segment() const noexcept
                {
                    return segment_;
                }

                [[nodiscard]]
                std::byte*
                data() const noexcept
                {
                    return static_cast<std::byte*>( address_ );
                }

                [[nodiscard]]
                std::size_t
                size() const noexcept
                {
                    return size_;
                }

            private:

                SharedSegment( xcb_connection_t* connection,
                               xcb_shm_seg_t     segment,
                               int               id,
                               void*             address,
                               std::size_t       size ) noexcept :
                    connection_( connection ),
                    segment_( segment ),
                    id_( id ),
                    address_( address ),
                    size_( size )
                {
                }

                void
                cleanup() noexcept
                {
                    if( connection_ != nullptr && segment_ != 0U )
                    {
                        xcb_shm_detach( connection_, segment_ );
                        static_cast<void>( xcb_flush( connection_ ) );
                    }
                    if( address_ != nullptr )
                    {
                        static_cast<void>( shmdt( address_ ) );
                    }
                    if( id_ != kInvalidSharedMemoryId )
                    {
                        static_cast<void>( shmctl( id_, IPC_RMID, nullptr ) );
                    }
                    connection_ = nullptr;
                    segment_    = 0U;
                    id_         = kInvalidSharedMemoryId;
                    address_    = nullptr;
                    size_       = 0U;
                }

                xcb_connection_t* connection_ = nullptr;
                xcb_shm_seg_t     segment_    = 0U;
                int               id_         = kInvalidSharedMemoryId;
                void*             address_    = nullptr;
                std::size_t       size_       = 0U;
        };

        [[nodiscard]]
        grab::Result<CapturedBuffer>
        capture_with_shm( xcb_connection_t*   connection,
                          const CaptureSpec&  spec,
                          const PixmapFormat& format )
        {
            auto size = capture_size_bytes( spec.width, spec.height, format );
            if( !size.has_value() )
            {
                return std::unexpected( std::move( size.error() ) );
            }

            auto segment = SharedSegment::attach( connection, *size );
            if( !segment.has_value() )
            {
                return std::unexpected( std::move( segment.error() ) );
            }

            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_shm_get_image_reply( connection,
                                         xcb_shm_get_image( connection,
                                                            spec.drawable,
                                                            spec.x,
                                                            spec.y,
                                                            spec.width,
                                                            spec.height,
                                                            kAllPlanes,
                                                            XCB_IMAGE_FORMAT_Z_PIXMAP,
                                                            segment->segment(),
                                                            kNoOffset ),
                                         &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                return grab::fail( window_error_code( *error ),
                                   "MIT-SHM GetImage failed with X error " +
                                       std::to_string( error->error_code ) );
            }
            if( reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "MIT-SHM GetImage returned no reply" );
            }

            const auto     reply_size = static_cast<std::size_t>( reply->size );
            const auto     copy_size  = std::min( segment->size(), reply_size );
            CapturedBuffer captured{
                .pixels = {},
                .depth  = reply->depth,
                .visual = reply->visual,
            };
            captured.pixels.resize( copy_size );
            const std::span<const std::byte> source{ segment->data(), copy_size };
            std::ranges::copy( source, captured.pixels.begin() );
            return captured;
        }

        [[nodiscard]]
        grab::Result<CapturedBuffer>
        capture_without_shm( xcb_connection_t*  connection,
                             const CaptureSpec& spec )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned(
                xcb_get_image_reply( connection,
                                     xcb_get_image( connection,
                                                    XCB_IMAGE_FORMAT_Z_PIXMAP,
                                                    spec.drawable,
                                                    spec.x,
                                                    spec.y,
                                                    spec.width,
                                                    spec.height,
                                                    kAllPlanes ),
                                     &raw_error )
            );
            const auto error = take_xcb_owned( raw_error );
            if( error != nullptr )
            {
                return grab::fail( window_error_code( *error ),
                                   "XCB GetImage failed with X error " +
                                       std::to_string( error->error_code ) );
            }
            if( reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB GetImage returned no reply" );
            }

            const int data_length = xcb_get_image_data_length( reply.get() );
            if( data_length < 0 )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB GetImage returned a negative data length" );
            }
            const auto* const data = xcb_get_image_data( reply.get() );
            CapturedBuffer    captured{
                .pixels = {},
                .depth  = reply->depth,
                .visual = reply->visual,
            };
            const auto data_size = static_cast<std::size_t>( data_length );
            captured.pixels.reserve( data_size );
            for( const auto value : std::span<const std::uint8_t>{ data, data_size } )
            {
                captured.pixels.push_back( static_cast<std::byte>( value ) );
            }
            return captured;
        }

        [[nodiscard]]
        grab::Result<grab::Image>
        capture_drawable( xcb_connection_t*  connection,
                          std::uint8_t       image_byte_order,
                          const CaptureSpec& spec )
        {
            auto format = pixmap_format_for_depth( connection, spec.depth );
            if( !format.has_value() )
            {
                return std::unexpected( std::move( format.error() ) );
            }

            auto captured = capture_with_shm( connection, spec, *format );
            if( !captured.has_value() )
            {
                captured = capture_without_shm( connection, spec );
            }
            if( !captured.has_value() )
            {
                return std::unexpected( std::move( captured.error() ) );
            }

            if( captured->depth !=
                spec.depth &&
                captured->depth !=
                kTwentyFourBitDepth &&
                captured->depth != kThirtyTwoBitDepth )
            {
                return grab::fail( grab::ErrorCode::protocol_error,
                                   "XCB GetImage returned an unexpected depth" );
            }

            return convert_image( connection,
                                  spec.width,
                                  spec.height,
                                  format->bits_per_pixel,
                                  format->scanline_pad,
                                  image_byte_order,
                                  *captured );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_region( std::int16_t  x,
                         std::int16_t  y,
                         std::uint16_t width,
                         std::uint16_t height,
                         std::uint16_t screen_width,
                         std::uint16_t screen_height )
        {
            if( width == 0U || height == 0U )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "X11 capture region must be non-empty" );
            }
            if( std::cmp_less( x, 0 ) || std::cmp_less( y, 0 ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "X11 capture region must start inside the screen" );
            }

            const auto right =
                static_cast<std::int32_t>( x ) + static_cast<std::int32_t>( width );
            const auto bottom =
                static_cast<std::int32_t>( y ) + static_cast<std::int32_t>( height );
            if( std::cmp_greater( right, screen_width ) ||
                std::cmp_greater( bottom, screen_height ) )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "X11 capture region exceeds the screen bounds" );
            }

            return {};
        }

    }    // namespace

    X11Capturer::X11Capturer( xcb_connection_t* connection,
                              std::uint32_t     root,
                              std::uint16_t     screen_width,
                              std::uint16_t     screen_height,
                              std::uint8_t      root_depth,
                              std::uint32_t     root_visual,
                              std::uint8_t      image_byte_order ) noexcept :
        connection_( connection ),
        root_( root ),
        screen_width_( screen_width ),
        screen_height_( screen_height ),
        root_depth_( root_depth ),
        root_visual_( root_visual ),
        image_byte_order_( image_byte_order )
    {
    }

    X11Capturer::~X11Capturer()
    {
        if( connection_ != nullptr )
        {
            xcb_disconnect( connection_ );
        }
    }

    X11Capturer::X11Capturer( X11Capturer&& other ) noexcept :
        connection_( std::exchange( other.connection_,
                                    nullptr ) ),
        root_( std::exchange( other.root_,
                              0U ) ),
        screen_width_( std::exchange( other.screen_width_,
                                      0U ) ),
        screen_height_( std::exchange( other.screen_height_,
                                       0U ) ),
        root_depth_( std::exchange( other.root_depth_,
                                    0U ) ),
        root_visual_( std::exchange( other.root_visual_,
                                     0U ) ),
        image_byte_order_( std::exchange( other.image_byte_order_,
                                          0U ) ),
        redirected_windows_( std::move( other.redirected_windows_ ) )
    {
    }

    X11Capturer&
    X11Capturer::operator=( X11Capturer&& other ) noexcept
    {
        if( this != &other )
        {
            if( connection_ != nullptr )
            {
                xcb_disconnect( connection_ );
            }
            connection_         = std::exchange( other.connection_, nullptr );
            root_               = std::exchange( other.root_, 0U );
            screen_width_       = std::exchange( other.screen_width_, 0U );
            screen_height_      = std::exchange( other.screen_height_, 0U );
            root_depth_         = std::exchange( other.root_depth_, 0U );
            root_visual_        = std::exchange( other.root_visual_, 0U );
            image_byte_order_   = std::exchange( other.image_byte_order_, 0U );
            redirected_windows_ = std::move( other.redirected_windows_ );
        }
        return *this;
    }

    grab::Result<X11Capturer>
    X11Capturer::open( const char* display )
    {
        int           screen_index = 0;
        XcbConnection connection{
            xcb_connect( display, &screen_index ),
            &xcb_disconnect
        };
        if( connection ==
            nullptr ||
            xcb_connection_has_error( connection.get() ) != kXcbOk )
        {
            return grab::fail( grab::ErrorCode::device_inaccessible,
                               "XCB display connection failed" );
        }

        auto composite_result = require_composite( connection.get() );
        if( !composite_result.has_value() )
        {
            return std::unexpected( std::move( composite_result.error() ) );
        }

        auto shm_result = require_shm( connection.get() );
        if( !shm_result.has_value() )
        {
            return std::unexpected( std::move( shm_result.error() ) );
        }

        auto screen = default_screen_info( connection.get(), screen_index );
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }

        return X11Capturer{
            connection.release(),
            screen->root,
            screen->width,
            screen->height,
            screen->root_depth,
            screen->root_visual,
            screen->image_byte_order
        };
    }

    grab::Result<grab::Image>
    X11Capturer::capture_window( std::uint32_t window )
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return std::unexpected( std::move( open_result.error() ) );
        }
        if( window == 0U || window == root_ )
        {
            return grab::fail( grab::ErrorCode::window_not_found,
                               "X11 capture target window is invalid" );
        }

        const auto redirected = std::ranges::find( redirected_windows_, window );
        if( redirected == redirected_windows_.end() )
        {
            auto redirect_result =
                check_window_request( connection_,
                                      xcb_composite_redirect_window_checked(
                                          connection_,
                                          window,
                                          XCB_COMPOSITE_REDIRECT_AUTOMATIC
                                      ),
                                      "Composite RedirectWindow" );
            if( !redirect_result.has_value() )
            {
                return std::unexpected( std::move( redirect_result.error() ) );
            }
            auto flush_result =
                flush_connection( connection_, "Composite RedirectWindow" );
            if( !flush_result.has_value() )
            {
                return std::unexpected( std::move( flush_result.error() ) );
            }
            redirected_windows_.push_back( window );
        }

        xcb_generic_error_t* raw_geometry_error = nullptr;
        const auto           geometry           = take_xcb_owned(
            xcb_get_geometry_reply( connection_,
                                    xcb_get_geometry( connection_, window ),
                                    &raw_geometry_error )
        );
        const auto geometry_error = take_xcb_owned( raw_geometry_error );
        if( geometry_error != nullptr )
        {
            return grab::fail( window_error_code( *geometry_error ),
                               "XCB GetGeometry failed with X error " +
                                   std::to_string( geometry_error->error_code ) );
        }
        if( geometry == nullptr )
        {
            return grab::fail( grab::ErrorCode::stale_window,
                               "XCB GetGeometry returned no reply" );
        }
        if( geometry->width == 0U || geometry->height == 0U )
        {
            return grab::fail( grab::ErrorCode::geometry_untrusted,
                               "X11 capture target window has empty geometry" );
        }

        return capture_drawable( connection_,
                                 image_byte_order_,
                                 CaptureSpec{
                                     .drawable = window,
                                     .x        = 0,
                                     .y        = 0,
                                     .width    = geometry->width,
                                     .height   = geometry->height,
                                     .depth    = geometry->depth,
                                 } );
    }

    grab::Result<grab::Image>
    X11Capturer::capture_display()
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return std::unexpected( std::move( open_result.error() ) );
        }

        return capture_drawable( connection_,
                                 image_byte_order_,
                                 CaptureSpec{
                                     .drawable = root_,
                                     .x        = 0,
                                     .y        = 0,
                                     .width    = screen_width_,
                                     .height   = screen_height_,
                                     .depth    = root_depth_,
                                 } );
    }

    grab::Result<grab::Image>
    X11Capturer::capture_region( std::int16_t  x,
                                 std::int16_t  y,
                                 std::uint16_t width,
                                 std::uint16_t height )
    {
        auto open_result = fail_if_connection_closed( connection_ );
        if( !open_result.has_value() )
        {
            return std::unexpected( std::move( open_result.error() ) );
        }

        auto region =
            validate_region( x, y, width, height, screen_width_, screen_height_ );
        if( !region.has_value() )
        {
            return std::unexpected( std::move( region.error() ) );
        }

        return capture_drawable( connection_,
                                 image_byte_order_,
                                 CaptureSpec{
                                     .drawable = root_,
                                     .x        = x,
                                     .y        = y,
                                     .width    = width,
                                     .height   = height,
                                     .depth    = root_depth_,
                                 } );
    }

}    // namespace grab::screen
