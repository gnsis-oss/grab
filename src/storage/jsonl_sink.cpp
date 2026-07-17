#include "core/permission.hpp"
#include "grab/event.hpp"
#include "grab/event_descriptor.hpp"
#include "grab/ids.hpp"
#include "grab/origin.hpp"
#include "grab/payload_fields.hpp"
#include "grab/result.hpp"
#include "storage/jsonl_sink.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace grab::storage
{
    namespace
    {

        constexpr int            invalidFd        = -1;
        constexpr int            posixFailure     = -1;
        constexpr int            writeFailure     = -1;
        constexpr int            noBytesWritten   = 0;
        constexpr int            yearWidth        = 4;
        constexpr int            monthDayWidth    = 2;
        constexpr std::uintmax_t bytesPerKilobyte = 1'024U;
        constexpr std::uintmax_t bytesPerMegabyte = bytesPerKilobyte * bytesPerKilobyte;
        constexpr std::string_view jsonlExtension = ".jsonl";
        constexpr std::string_view jsonlSuffix    = ".jsonl";
        constexpr std::string_view sinkClosedMessage = "jsonl sink is closed";
        constexpr std::string_view movedFromMessage  = "jsonl sink is moved-from";

        using OrderedJson                            = nlohmann::ordered_json;

        struct BufferedLine
        {
                std::string date;
                std::string line;
        };

        struct JsonlFile
        {
                std::filesystem::path path;
                std::string           name;
                std::uintmax_t        size = 0U;
        };

        [[nodiscard]]
        std::string
        posix_message( std::string_view step,
                       int              error_number )
        {
            return std::string{ step } +
                   ": " +
                   std::error_code{ error_number, std::generic_category() }.message();
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
        std::unexpected<grab::Error>
        unexpected_error( grab::ErrorCode code,
                          std::string     message )
        {
            return std::unexpected( make_error( code, std::move( message ) ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        exception_error( std::string_view      step,
                         const std::exception& exception )
        {
            return unexpected_error( grab::ErrorCode::InternalFault,
                                     std::string{ step } + ": " + exception.what() );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        unknown_exception_error( std::string_view step )
        {
            return unexpected_error( grab::ErrorCode::InternalFault,
                                     std::string{ step } + ": unknown exception" );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        closed_error()
        {
            return unexpected_error( grab::ErrorCode::SessionClosed,
                                     std::string{ sinkClosedMessage } );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        moved_from_error()
        {
            return unexpected_error( grab::ErrorCode::SessionClosed,
                                     std::string{ movedFromMessage } );
        }

        [[nodiscard]]
        grab::Result<void>
        ensure_json_number( double value )
        {
            if( !std::isfinite( value ) )
            {
                return unexpected_error( grab::ErrorCode::InvalidArgument,
                                         "jsonl numeric value is not finite" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::InputKey& payload )
        {
            return OrderedJson{
                {std::string{ grab::field_name( grab::PayloadField::KeyCode ) },
                 payload.code},
                {std::string{ grab::field_name( grab::PayloadField::KeyName ) },
                 payload.name},
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::KeyCombo& payload )
        {
            return OrderedJson{
                { std::string{ grab::field_name( grab::PayloadField::Text ) },
                 payload.text },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::MouseClick& payload )
        {
            return OrderedJson{
                {    std::string{ grab::field_name( grab::PayloadField::Button ) },
                 payload.button},
                {std::string{ grab::field_name( grab::PayloadField::ButtonName ) },
                 payload.name  },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::MouseMove& payload )
        {
            auto result = ensure_json_number( payload.delta );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return OrderedJson{
                { std::string{ grab::field_name( grab::PayloadField::Axis ) },
                 payload.axis },
                {std::string{ grab::field_name( grab::PayloadField::Delta ) },
                 payload.delta},
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::Idle& payload )
        {
            auto result = ensure_json_number( payload.idle_s );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return OrderedJson{
                { std::string{ grab::field_name( grab::PayloadField::IdleSeconds ) },
                 payload.idle_s },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::WindowChange& payload )
        {
            auto result = ensure_json_number( payload.duration_s );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return OrderedJson{
                {            std::string{ grab::field_name( grab::PayloadField::App ) },
                 payload.app            },
                {            std::string{ grab::field_name( grab::PayloadField::Pid ) },
                 payload.pid.to_string()},
                {          std::string{ grab::field_name( grab::PayloadField::Title ) },
                 payload.title          },
                {      std::string{ grab::field_name( grab::PayloadField::PrevTitle ) },
                 payload.prev_title     },
                {std::string{ grab::field_name( grab::PayloadField::DurationSeconds ) },
                 payload.duration_s     },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind        kind,
                           const grab::A11yEvent& payload )
        {
            const auto detail_field = kind == grab::EventKind::A11yStateChanged
                                        ? grab::PayloadField::State
                                        : grab::PayloadField::Detail;
            return OrderedJson{
                { std::string{ grab::field_name( grab::PayloadField::App ) },
                 payload.app                                                                },
                {std::string{ grab::field_name( grab::PayloadField::Role ) },
                 payload.role                                                               },
                {std::string{ grab::field_name( grab::PayloadField::Name ) },
                 payload.name                                                               },
                {            std::string{ grab::field_name( detail_field ) }, payload.detail},
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::IntegrationEvent& payload )
        {
            return OrderedJson{
                {   std::string{ grab::field_name( grab::PayloadField::App ) },
                 payload.app   },
                { std::string{ grab::field_name( grab::PayloadField::Title ) },
                 payload.title },
                {std::string{ grab::field_name( grab::PayloadField::Detail ) },
                 payload.detail},
                {  std::string{ grab::field_name( grab::PayloadField::Json ) },
                 payload.json  },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::StateSnapshot& payload )
        {
            return OrderedJson{
                { std::string{ grab::field_name( grab::PayloadField::Json ) },
                 payload.json },
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind,
                           const grab::GraphChange& payload )
        {
            return OrderedJson{
                {          std::string{ grab::field_name( grab::PayloadField::Node ) },
                 payload.node           },
                {       std::string{ grab::field_name( grab::PayloadField::Related ) },
                 payload.related        },
                {      std::string{ grab::field_name( grab::PayloadField::Relation ) },
                 payload.relation       },
                {std::string{ grab::field_name( grab::PayloadField::PreviousActive ) },
                 payload.previous_active},
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        serialize_payload( grab::EventKind      kind,
                           const grab::Payload& payload )
        {
            return std::visit(
                [kind]( const auto& typed_payload ) -> grab::Result<OrderedJson>
                {
                    return serialize_payload( kind, typed_payload );
                },
                payload
            );
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_line( const grab::Event& event )
        {
            auto timestamp = ensure_json_number( event.timestamp );
            if( !timestamp.has_value() )
            {
                return std::unexpected( std::move( timestamp.error() ) );
            }
            auto data = serialize_payload( event.kind, event.payload );
            if( !data.has_value() )
            {
                return std::unexpected( std::move( data.error() ) );
            }

            OrderedJson line = OrderedJson{
                {      "ts",              event.timestamp                            },
                {    "type",             std::string{ grab::wire_name( event.kind ) }},
                {"category",     std::string{ grab::category_name( event.category ) }},
                {     "seq",                                           event.sequence},
                {  "origin",
                 std::string{ grab::detail::event_origin_name.text_of( event.origin,
                 "unknown" ) }                                                       },
                {    "data",                                       std::move( *data )},
            };

            if( event.subject.has_value() )
            {
                line["subject"] = OrderedJson{
                    { "runtime", event.subject->runtime.value},
                    {    "tree",          event.subject->tree},
                    {   "epoch",   event.subject->epoch.value},
                    {    "node",          event.subject->node},
                    {"revision",      event.subject->revision},
                };
            }
            if( event.cause.has_value() )
            {
                line["cause"] = event.cause->value.to_string();
            }
            if( event.before_revision.has_value() )
            {
                line["before"] = *event.before_revision;
            }
            if( event.after_revision.has_value() )
            {
                line["after"] = *event.after_revision;
            }
            return line.dump();
        }

        [[nodiscard]]
        grab::Result<std::string>
        date_from_timestamp( double timestamp )
        {
            if( !std::isfinite( timestamp ) )
            {
                return unexpected_error( grab::ErrorCode::InvalidArgument,
                                         "jsonl event timestamp is not finite" );
            }

            constexpr auto minSeconds =
                static_cast<double>( std::numeric_limits<std::int64_t>::lowest() );
            constexpr auto maxSeconds =
                static_cast<double>( std::numeric_limits<std::int64_t>::max() );
            if( timestamp < minSeconds || timestamp > maxSeconds )
            {
                return unexpected_error( grab::ErrorCode::InvalidArgument,
                                         "jsonl event timestamp is out of range" );
            }

            using TimestampDuration    = std::chrono::duration<double>;
            const auto timestamp_point = std::chrono::sys_time<TimestampDuration>{
                TimestampDuration{ timestamp }
            };
            const auto day = std::chrono::floor<std::chrono::days>( timestamp_point );

            constexpr std::chrono::sys_days firstSupportedDay{
                std::chrono::year::min() / std::chrono::January / 1,
            };
            constexpr std::chrono::sys_days lastSupportedDay{
                std::chrono::year::max() / std::chrono::December / std::chrono::last,
            };
            if( day < firstSupportedDay || day > lastSupportedDay )
            {
                return unexpected_error( grab::ErrorCode::InvalidArgument,
                                         "jsonl event timestamp is out of range" );
            }

            const std::chrono::year_month_day date{ day };

            std::ostringstream                output;
            output << std::setfill( '0' ) << std::setw( yearWidth )
                   << static_cast<int>( date.year() ) << '-'
                   << std::setw( monthDayWidth )
                   << static_cast<unsigned int>( date.month() ) << '-'
                   << std::setw( monthDayWidth )
                   << static_cast<unsigned int>( date.day() );
            return output.str();
        }

        [[nodiscard]]
        std::filesystem::path
        path_for_date( const std::filesystem::path& dir,
                       std::string_view             date )
        {
            return dir / ( std::string{ date } + std::string{ jsonlSuffix } );
        }

    }    // namespace

    class JsonlSink::Impl
    {
        public:

            explicit Impl( JsonlOptions options );
            ~Impl() noexcept;

            Impl( const Impl& ) = delete;
            Impl&
            operator=( const Impl& ) = delete;
            Impl( Impl&& )           = delete;
            Impl&
            operator=( Impl&& ) = delete;

            [[nodiscard]]
            grab::Result<void>
            initialize() const;

            [[nodiscard]]
            grab::Result<void>
            write( const grab::Event& event );

            [[nodiscard]]
            grab::Result<void>
            flush();

            void
            close() noexcept;

        private:

            [[nodiscard]]
            grab::Result<void>
            open_for_date( std::string_view date );

            [[nodiscard]]
            static grab::Result<void>
            ensure_file_exists( const std::filesystem::path& path );

            [[nodiscard]]
            grab::Result<void>
            write_all( std::string_view bytes ) const;

            [[nodiscard]]
            grab::Result<void>
            fsync_current_file() const;

            [[nodiscard]]
            grab::Result<void>
            close_current_file();

            [[nodiscard]]
            grab::Result<void>
            enforce_retention();

            [[nodiscard]]
            std::uintmax_t
                                      disk_budget_bytes() const noexcept;

            JsonlOptions              options_;
            std::vector<BufferedLine> buffer_;
            int                       fd_ = invalidFd;
            std::string               current_date_;
            std::filesystem::path     current_path_;
            bool                      closed_ = false;
    };

    JsonlSink::Impl::Impl( JsonlOptions options ) :
        options_( std::move( options ) )
    {
    }

    JsonlSink::Impl::~Impl() noexcept
    {
        close();
    }

    grab::Result<void>
    JsonlSink::Impl::initialize() const
    {
        if( options_.dir.empty() )
        {
            return unexpected_error( grab::ErrorCode::InvalidArgument,
                                     "jsonl directory is empty" );
        }
        if( options_.buffer_limit == 0U )
        {
            return unexpected_error( grab::ErrorCode::InvalidArgument,
                                     "jsonl buffer limit must be greater than zero" );
        }

        std::error_code ec;
        std::filesystem::create_directories( options_.dir, ec );
        if( ec )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     "create_directories: " + ec.message() );
        }

        const bool is_directory = std::filesystem::is_directory( options_.dir, ec );
        if( ec )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     "is_directory: " + ec.message() );
        }
        if( !is_directory )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     "jsonl path is not a directory" );
        }

        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::write( const grab::Event& event )
    {
        if( closed_ )
        {
            return closed_error();
        }

        auto date = date_from_timestamp( event.timestamp );
        if( !date.has_value() )
        {
            return std::unexpected( std::move( date.error() ) );
        }
        auto line = serialize_line( event );
        if( !line.has_value() )
        {
            return std::unexpected( std::move( line.error() ) );
        }

        buffer_.push_back( BufferedLine{
            .date = std::move( *date ),
            .line = std::move( *line ),
        } );

        if( buffer_.size() >= options_.buffer_limit )
        {
            return flush();
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::flush()
    {
        if( closed_ || buffer_.empty() )
        {
            return {};
        }

        auto current = buffer_.cbegin();
        while( current != buffer_.cend() )
        {
            const std::string_view date = current->date;
            std::string            batch;
            while( current != buffer_.cend() && current->date == date )
            {
                batch += current->line;
                batch += '\n';
                ++current;
            }

            auto open_result = open_for_date( date );
            if( !open_result.has_value() )
            {
                return open_result;
            }

            auto write_result = write_all( batch );
            if( !write_result.has_value() )
            {
                return write_result;
            }

            auto fsync_result = fsync_current_file();
            if( !fsync_result.has_value() )
            {
                return fsync_result;
            }
        }

        buffer_.clear();
        return enforce_retention();
    }

    void
    JsonlSink::Impl::close() noexcept
    {
        try
        {
            const auto flush_result = flush();
            static_cast<void>( flush_result );
            const auto close_result = close_current_file();
            static_cast<void>( close_result );
            closed_ = true;
        }
        catch( ... )
        {
            closed_ = true;
        }
    }

    grab::Result<void>
    JsonlSink::Impl::open_for_date( std::string_view date )
    {
        if( fd_ != invalidFd && current_date_ == date )
        {
            return {};
        }

        auto close_result = close_current_file();
        if( !close_result.has_value() )
        {
            return close_result;
        }

        const auto path          = path_for_date( options_.dir, date );
        auto       ensure_result = ensure_file_exists( path );
        if( !ensure_result.has_value() )
        {
            return ensure_result;
        }

        constexpr auto openFlags =
            static_cast<int>( static_cast<unsigned int>( O_WRONLY ) |
                              static_cast<unsigned int>( O_APPEND ) |
                              static_cast<unsigned int>( O_CLOEXEC ) );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open(2).
        const int fd = ::open( path.c_str(), openFlags );
        if( fd == posixFailure )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     posix_message( "open", errno ) );
        }

        fd_           = fd;
        current_date_ = std::string{ date };
        current_path_ = path;
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::ensure_file_exists( const std::filesystem::path& path )
    {
        std::error_code ec;
        const bool      exists = std::filesystem::exists( path, ec );
        if( ec )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     "exists: " + ec.message() );
        }
        if( exists )
        {
            return {};
        }

        auto create_result =
            grab::core::StateDir::write_atomic( path, std::string_view{} );
        if( !create_result.has_value() )
        {
            return std::unexpected( std::move( create_result.error() ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::write_all( std::string_view bytes ) const
    {
        std::string_view unwritten = bytes;
        while( !unwritten.empty() )
        {
            const auto count = std::min( unwritten.size(),
                                         static_cast<std::size_t>( bytesPerMegabyte ) );
            const auto written = ::write( fd_, unwritten.data(), count );
            if( written == writeFailure )
            {
                const int error_number = errno;
                if( error_number == EINTR )
                {
                    continue;
                }
                return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                         posix_message( "write", error_number ) );
            }
            if( written == noBytesWritten )
            {
                return unexpected_error( grab::ErrorCode::InternalFault,
                                         "write: wrote zero bytes" );
            }
            unwritten.remove_prefix( static_cast<std::size_t>( written ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::fsync_current_file() const
    {
        while( ::fsync( fd_ ) == posixFailure )
        {
            const int error_number = errno;
            if( error_number == EINTR )
            {
                continue;
            }
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     posix_message( "fsync", error_number ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::close_current_file()
    {
        if( fd_ == invalidFd )
        {
            current_date_.clear();
            current_path_.clear();
            return {};
        }

        const int close_result = ::close( fd_ );
        fd_                    = invalidFd;
        current_date_.clear();
        current_path_.clear();
        if( close_result == posixFailure )
        {
            return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                     posix_message( "close", errno ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::enforce_retention()
    {
        std::vector<JsonlFile> files;
        std::uintmax_t         total = 0U;

        for( const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator( options_.dir ) )
        {
            if( entry.path().extension() != jsonlExtension )
            {
                continue;
            }

            std::error_code ec;
            const bool      regular_file = entry.is_regular_file( ec );
            if( ec )
            {
                return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                         "is_regular_file: " + ec.message() );
            }
            if( !regular_file )
            {
                continue;
            }

            const std::uintmax_t size = entry.file_size( ec );
            if( ec )
            {
                return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                         "file_size: " + ec.message() );
            }

            files.push_back( JsonlFile{
                .path = entry.path(),
                .name = entry.path().filename().string(),
                .size = size,
            } );
            total += size;
        }

        std::ranges::sort( files,
                           []( const JsonlFile& lhs, const JsonlFile& rhs ) noexcept
                           {
                               return lhs.name < rhs.name;
                           } );

        const std::uintmax_t budget = disk_budget_bytes();
        while( !files.empty() &&
               ( files.size() > options_.max_files || total > budget ) )
        {
            const JsonlFile& oldest = files.front();
            if( oldest.path == current_path_ )
            {
                auto close_result = close_current_file();
                if( !close_result.has_value() )
                {
                    return close_result;
                }
            }

            std::error_code ec;
            std::filesystem::remove( oldest.path, ec );
            if( ec )
            {
                return unexpected_error( grab::ErrorCode::DeviceInaccessible,
                                         "remove: " + ec.message() );
            }

            total -= oldest.size;
            files.erase( files.begin() );
        }

        return {};
    }

    std::uintmax_t
    JsonlSink::Impl::disk_budget_bytes() const noexcept
    {
        constexpr auto maxBytes = std::numeric_limits<std::uintmax_t>::max();
        const auto     max_mb   = maxBytes / bytesPerMegabyte;
        const auto     disk_mb  = static_cast<std::uintmax_t>( options_.max_disk_mb );
        if( disk_mb > max_mb )
        {
            return maxBytes;
        }
        return disk_mb * bytesPerMegabyte;
    }

    JsonlSink::JsonlSink( std::unique_ptr<Impl> impl ) noexcept :
        impl_( std::move( impl ) )
    {
    }

    JsonlSink::~JsonlSink()
    {
        close();
    }

    JsonlSink::JsonlSink( JsonlSink&& other ) noexcept = default;

    JsonlSink&
    JsonlSink::operator=( JsonlSink&& other ) noexcept
    {
        if( this != &other )
        {
            close();
            impl_ = std::move( other.impl_ );
        }
        return *this;
    }

    grab::Result<JsonlSink>
    JsonlSink::open( JsonlOptions options )
    {
        try
        {
            auto impl   = std::make_unique<Impl>( std::move( options ) );
            auto result = impl->initialize();
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return JsonlSink{ std::move( impl ) };
        }
        catch( const std::exception& exception )
        {
            return exception_error( "jsonl open", exception );
        }
        catch( ... )
        {
            return unknown_exception_error( "jsonl open" );
        }
    }

    grab::Result<void>
    JsonlSink::write( const grab::Event& event )
    {
        if( impl_ == nullptr )
        {
            return moved_from_error();
        }

        try
        {
            return impl_->write( event );
        }
        catch( const std::exception& exception )
        {
            return exception_error( "jsonl write", exception );
        }
        catch( ... )
        {
            return unknown_exception_error( "jsonl write" );
        }
    }

    grab::Result<void>
    JsonlSink::flush()
    {
        if( impl_ == nullptr )
        {
            return moved_from_error();
        }

        try
        {
            return impl_->flush();
        }
        catch( const std::exception& exception )
        {
            return exception_error( "jsonl flush", exception );
        }
        catch( ... )
        {
            return unknown_exception_error( "jsonl flush" );
        }
    }

    void
    JsonlSink::close() noexcept
    {
        if( impl_ != nullptr )
        {
            impl_->close();
        }
    }

}    // namespace grab::storage
