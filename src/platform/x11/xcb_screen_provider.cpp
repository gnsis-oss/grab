#include "core/checked.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "platform/x11/cursor_overlay.hpp"
#include "platform/x11/pixel_format.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_reply.hpp"
#include "platform/x11/xcb_screen_provider.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <system_error>
#include <utility>
#include <vector>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::platform::x11
{

    namespace
    {

        constexpr std::uint16_t empty_dimension  = 0U;
        constexpr std::uint8_t  bits_per_byte    = 8U;
        constexpr std::uint8_t  z_pixmap_format  = XCB_IMAGE_FORMAT_Z_PIXMAP;
        constexpr std::uint32_t all_planes_mask  = ~std::uint32_t{ 0U };
        constexpr std::uint32_t shm_offset       = 0U;
        constexpr std::uint8_t  shm_writable     = 0U;
        constexpr int           posix_failure    = -1;
        constexpr int           no_shm_flags     = 0;
        constexpr int           shm_create_flag  = IPC_CREAT;
        constexpr int           owner_read_write = 0600;
        constexpr int           invalid_shm_id   = -1;

        [[nodiscard]]
        std::string
        posix_error( std::string step,
                     int         error_number )
        {
            step += ": ";
            step += std::error_code{ error_number, std::generic_category() }.message();
            return step;
        }

        [[nodiscard]]
        grab::Result<void>
        validate_region( std::uint16_t width,
                         std::uint16_t height )
        {
            if( width == empty_dimension || height == empty_dimension )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "capture geometry dimensions must be non-zero" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::uint8_t>
        get_root_depth( const XcbConnection& conn )
        {
            const xcb_get_geometry_cookie_t cookie =
                xcb_get_geometry( conn.get(), conn.root() );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_get_geometry_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_get_geometry failed for root window" );
            }
            return reply->depth;
        }

        [[nodiscard]]
        grab::Result<XPixmapFormat>
        get_bpp_for_depth( const XcbConnection& conn,
                           std::uint8_t         depth )
        {
            const xcb_setup_t* setup = xcb_get_setup( conn.get() );
            if( setup == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "X setup is unavailable" );
            }

            xcb_format_iterator_t iter = xcb_setup_pixmap_formats_iterator( setup );
            while( iter.rem > 0 )
            {
                if( iter.data != nullptr && iter.data->depth == depth )
                {
                    return XPixmapFormat{
                        .depth          = iter.data->depth,
                        .bits_per_pixel = iter.data->bits_per_pixel,
                    };
                }
                xcb_format_next( &iter );
            }

            return grab::fail( grab::ErrorCode::provider_failed,
                               "no X pixmap format matches root depth" );
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        get_image_byte_order( const XcbConnection& conn )
        {
            const xcb_setup_t* setup = xcb_get_setup( conn.get() );
            if( setup == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "X setup is unavailable" );
            }
            return setup->image_byte_order;
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        checked_stride( std::uint16_t width,
                        std::uint8_t  bits_per_pixel )
        {
            if( bits_per_pixel % bits_per_byte != 0U )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "X pixmap bits_per_pixel is not byte-aligned" );
            }
            const std::size_t bytes_per_pixel = bits_per_pixel / bits_per_byte;
            return grab::checked_mul<std::size_t>( static_cast<std::size_t>( width ),
                                                   bytes_per_pixel,
                                                   grab::ErrorCode::invalid_argument,
                                                   "capture stride is too large" );
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        checked_image_size( std::size_t   stride,
                            std::uint16_t height )
        {
            return grab::checked_mul<std::size_t>( stride,
                                                   static_cast<std::size_t>( height ),
                                                   grab::ErrorCode::invalid_argument,
                                                   "capture image is too large" );
        }

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        copy_reply_bytes( const xcb_get_image_reply_t& reply,
                          std::size_t                  expected_byte_count )
        {
            const int length = xcb_get_image_data_length( &reply );
            if( length < 0 || !std::in_range<std::size_t>( length ) )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_get_image returned an invalid data length" );
            }

            const auto actual_byte_count = static_cast<std::size_t>( length );
            if( actual_byte_count != expected_byte_count )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_get_image returned unexpected data length" );
            }

            const std::uint8_t* data = xcb_get_image_data( &reply );
            if( data == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_get_image returned null data" );
            }

            const std::span<const std::uint8_t> pixels{ data, expected_byte_count };
            return std::vector<std::uint8_t>{ pixels.begin(), pixels.end() };
        }

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        read_plain( const XcbConnection& conn,
                    std::int16_t         x,
                    std::int16_t         y,
                    std::uint16_t        width,
                    std::uint16_t        height,
                    std::size_t          expected_byte_count )
        {
            const xcb_get_image_cookie_t cookie    = xcb_get_image( conn.get(),
                                                                    z_pixmap_format,
                                                                    conn.root(),
                                                                    x,
                                                                    y,
                                                                    width,
                                                                    height,
                                                                    all_planes_mask );
            xcb_generic_error_t*         error_raw = nullptr;
            auto                         reply =
                make_xcb_reply( xcb_get_image_reply( conn.get(), cookie, &error_raw ) );
            auto error = make_xcb_reply( error_raw );

            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_get_image failed for root window" );
            }
            return copy_reply_bytes( *reply, expected_byte_count );
        }

        [[nodiscard]]
        int
        shm_creation_flags() noexcept
        {
            const auto create = static_cast<unsigned int>( shm_create_flag );
            const auto owner  = static_cast<unsigned int>( owner_read_write );
            return static_cast<int>( create | owner );
        }

        [[nodiscard]]
        void*
        shmat_failure_pointer() noexcept
        {
            constexpr std::intptr_t shmat_failure_address = -1;
            // POSIX defines shmat failure as reinterpret_cast<void*>( -1 ).
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
            return reinterpret_cast<void*>( shmat_failure_address );
        }

        class SharedImageMemory
        {
            public:

                SharedImageMemory()                           = default;
                SharedImageMemory( const SharedImageMemory& ) = delete;

                SharedImageMemory( SharedImageMemory&& other ) noexcept :
                    connection( std::exchange( other.connection,
                                               nullptr ) ),
                    id( std::exchange( other.id,
                                       invalid_shm_id ) ),
                    xcb_segment( std::exchange( other.xcb_segment,
                                                XCB_NONE ) ),
                    address( std::exchange( other.address,
                                            nullptr ) ),
                    attached( std::exchange( other.attached,
                                             false ) )
                {
                }

                SharedImageMemory&
                operator=( const SharedImageMemory& ) = delete;

                SharedImageMemory&
                operator=( SharedImageMemory&& other ) noexcept
                {
                    if( this != &other )
                    {
                        cleanup();
                        connection  = std::exchange( other.connection, nullptr );
                        id          = std::exchange( other.id, invalid_shm_id );
                        xcb_segment = std::exchange( other.xcb_segment, XCB_NONE );
                        address     = std::exchange( other.address, nullptr );
                        attached    = std::exchange( other.attached, false );
                    }
                    return *this;
                }

                ~SharedImageMemory()
                {
                    cleanup();
                }

                [[nodiscard]]
                static grab::Result<SharedImageMemory>
                create( const XcbConnection& conn,
                        std::size_t          byte_count )
                {
                    const int id =
                        ::shmget( IPC_PRIVATE, byte_count, shm_creation_flags() );
                    if( id == posix_failure )
                    {
                        return grab::fail( grab::ErrorCode::provider_failed,
                                           posix_error( "shmget", errno ) );
                    }

                    void* const address = ::shmat( id, nullptr, no_shm_flags );
                    if( address == shmat_failure_pointer() )
                    {
                        const int error_number = errno;
                        ( void )::shmctl( id, IPC_RMID, nullptr );
                        return grab::fail( grab::ErrorCode::provider_failed,
                                           posix_error( "shmat", error_number ) );
                    }

                    SharedImageMemory memory{
                        conn.get(),
                        id,
                        xcb_generate_id( conn.get() ),
                        address,
                    };
                    auto attach_result = memory.attach();
                    if( !attach_result.has_value() )
                    {
                        return grab::fail( attach_result.error().code,
                                           attach_result.error().message );
                    }
                    return memory;
                }

                [[nodiscard]]
                xcb_shm_seg_t
                segment() const noexcept
                {
                    return xcb_segment;
                }

                [[nodiscard]]
                const std::uint8_t*
                data() const noexcept
                {
                    return static_cast<const std::uint8_t*>( address );
                }

            private:

                SharedImageMemory( xcb_connection_t* connection,
                                   int               id,
                                   xcb_shm_seg_t     segment,
                                   void*             address ) noexcept :
                    connection( connection ),
                    id( id ),
                    xcb_segment( segment ),
                    address( address )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                attach()
                {
                    const xcb_void_cookie_t cookie =
                        xcb_shm_attach_checked( connection,
                                                xcb_segment,
                                                static_cast<std::uint32_t>( id ),
                                                shm_writable );
                    auto error =
                        make_xcb_reply( xcb_request_check( connection, cookie ) );
                    if( error != nullptr )
                    {
                        return grab::fail( grab::ErrorCode::provider_failed,
                                           "xcb_shm_attach failed" );
                    }
                    attached = true;
                    return {};
                }

                void
                cleanup() noexcept
                {
                    if( attached && connection != nullptr )
                    {
                        ( void )xcb_shm_detach( connection, xcb_segment );
                        ( void )xcb_flush( connection );
                    }
                    if( address != nullptr )
                    {
                        ( void )::shmdt( address );
                    }
                    if( id != invalid_shm_id )
                    {
                        ( void )::shmctl( id, IPC_RMID, nullptr );
                    }
                    connection  = nullptr;
                    id          = invalid_shm_id;
                    xcb_segment = XCB_NONE;
                    address     = nullptr;
                    attached    = false;
                }

                xcb_connection_t* connection  = nullptr;
                int               id          = invalid_shm_id;
                xcb_shm_seg_t     xcb_segment = XCB_NONE;
                void*             address     = nullptr;
                bool              attached    = false;
        };

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        read_via_shm( const XcbConnection& conn,
                      std::int16_t         x,
                      std::int16_t         y,
                      std::uint16_t        width,
                      std::uint16_t        height,
                      std::size_t          expected_byte_count )
        {
            auto memory = SharedImageMemory::create( conn, expected_byte_count );
            if( !memory.has_value() )
            {
                return read_plain( conn, x, y, width, height, expected_byte_count );
            }

            const xcb_shm_get_image_cookie_t cookie =
                xcb_shm_get_image( conn.get(),
                                   conn.root(),
                                   x,
                                   y,
                                   width,
                                   height,
                                   all_planes_mask,
                                   z_pixmap_format,
                                   memory->segment(),
                                   shm_offset );
            xcb_generic_error_t* error_raw = nullptr;
            auto                 reply     = make_xcb_reply(
                xcb_shm_get_image_reply( conn.get(), cookie, &error_raw )
            );
            auto error = make_xcb_reply( error_raw );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "xcb_shm_get_image failed for root window" );
            }

            const std::span<const std::uint8_t> pixels{
                memory->data(),
                expected_byte_count,
            };
            return std::vector<std::uint8_t>{ pixels.begin(), pixels.end() };
        }

        [[nodiscard]]
        grab::Result<std::vector<std::uint8_t>>
        read_pixels( const XcbConnection& conn,
                     std::int16_t         x,
                     std::int16_t         y,
                     std::uint16_t        width,
                     std::uint16_t        height,
                     std::size_t          expected_byte_count )
        {
            if( conn.has_shm() )
            {
                return read_via_shm( conn, x, y, width, height, expected_byte_count );
            }
            return read_plain( conn, x, y, width, height, expected_byte_count );
        }

    }    // namespace

    grab::Result<grab::Image>
    capture_region( const XcbConnection& conn,
                    std::int16_t         x,
                    std::int16_t         y,
                    std::uint16_t        width,
                    std::uint16_t        height,
                    bool                 draw_cursor )
    {
        auto valid = validate_region( width, height );
        if( !valid.has_value() )
        {
            return grab::fail( valid.error().code, valid.error().message );
        }

        auto depth = get_root_depth( conn );
        if( !depth.has_value() )
        {
            return grab::fail( depth.error().code, depth.error().message );
        }
        auto pixmap_format = get_bpp_for_depth( conn, *depth );
        if( !pixmap_format.has_value() )
        {
            return grab::fail( pixmap_format.error().code,
                               pixmap_format.error().message );
        }
        auto byte_order = get_image_byte_order( conn );
        if( !byte_order.has_value() )
        {
            return grab::fail( byte_order.error().code, byte_order.error().message );
        }
        auto format = pixel_format_for( pixmap_format->depth,
                                        pixmap_format->bits_per_pixel,
                                        *byte_order );
        if( !format.has_value() )
        {
            return grab::fail( format.error().code, format.error().message );
        }

        auto stride = checked_stride( width, pixmap_format->bits_per_pixel );
        if( !stride.has_value() )
        {
            return grab::fail( stride.error().code, stride.error().message );
        }
        auto byte_count = checked_image_size( *stride, height );
        if( !byte_count.has_value() )
        {
            return grab::fail( byte_count.error().code, byte_count.error().message );
        }

        auto bytes = read_pixels( conn, x, y, width, height, *byte_count );
        if( !bytes.has_value() )
        {
            return grab::fail( bytes.error().code, bytes.error().message );
        }

        auto                   raw = std::move( *bytes );
        std::vector<std::byte> pixels;
        pixels.reserve( raw.size() );
        for( const std::uint8_t value : raw )
        {
            pixels.push_back( std::byte{ value } );
        }

        grab::Image image{
            .width  = width,
            .height = height,
            .stride = static_cast<std::uint32_t>( *stride ),
            .format = *format,
            .pixels = std::move( pixels ),
        };

        if( draw_cursor && conn.has_xfixes() )
        {
            auto cursor = draw_xfixes_cursor(
                conn.get(),
                std::span<std::uint8_t>{
                    reinterpret_cast<std::uint8_t*>( image.pixels.data() ),
                    image.pixels.size()
                },
                image.stride,
                grab::geometry::Rectangle{
                    .x      = x,
                    .y      = y,
                    .width  = width,
                    .height = height,
                }
            );
            if( !cursor.has_value() )
            {
                return grab::fail( cursor.error().code, cursor.error().message );
            }
        }

        return image;
    }

}    // namespace grab::platform::x11
