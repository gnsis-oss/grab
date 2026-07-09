#include "core/json.hpp"
#include "core/permission.hpp"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "storage/jsonl_sink.hpp"

#include <algorithm>
#include <cerrno>
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

        constexpr int            kInvalidFd            = -1;
        constexpr int            kPosixFailure         = -1;
        constexpr int            kWriteFailure         = -1;
        constexpr int            kNoBytesWritten       = 0;
        constexpr std::size_t    kJsonStringPrefixSize = 5U;
        constexpr std::size_t    kJsonStringSuffixSize = 1U;
        constexpr int            kYearWidth            = 4;
        constexpr int            kMonthDayWidth        = 2;
        constexpr std::uintmax_t kBytesPerKilobyte     = 1'024U;
        constexpr std::uintmax_t kBytesPerMegabyte =
            kBytesPerKilobyte * kBytesPerKilobyte;
        constexpr std::int64_t     kSecondsPerDay              = 86'400;
        constexpr std::int64_t     kUnixEpochToCivilOffsetDays = 719'468;
        constexpr std::int64_t     kDaysPerEra                 = 146'097;
        constexpr std::int64_t     kYearsPerEra                = 400;
        constexpr std::int64_t     kDaysPerNormalYear          = 365;
        constexpr std::int64_t     kDaysPerFourYears           = 1'460;
        constexpr std::int64_t     kDaysPerCentury             = 36'524;
        constexpr std::int64_t     kLeapYearCycle              = 4;
        constexpr std::int64_t     kCenturyCycle               = 100;
        constexpr std::int64_t     kMarchMonthNumerator        = 5;
        constexpr std::int64_t     kMarchMonthOffset           = 2;
        constexpr std::int64_t     kDaysPerMarchMonthBlock     = 153;
        constexpr std::int64_t     kCalendarOrdinalBase        = 1;
        constexpr std::int64_t     kMarchMonthCutoff           = 10;
        constexpr std::int64_t     kMarchToJanuaryOffset       = 3;
        constexpr std::int64_t     kMarchToCalendarOffset      = 9;
        constexpr std::int64_t     kFebruaryNumber             = 2;
        constexpr std::string_view kJsonProbeKey               = "v";
        constexpr std::string_view kJsonlExtension             = ".jsonl";
        constexpr std::string_view kJsonlSuffix                = ".jsonl";
        constexpr std::string_view kSinkClosedMessage          = "jsonl sink is closed";
        constexpr std::string_view kMovedFromMessage = "jsonl sink is moved-from";

        struct CivilDate
        {
                std::int64_t year  = 0;
                unsigned int month = 0U;
                unsigned int day   = 0U;
        };

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
            return unexpected_error( grab::ErrorCode::internal_fault,
                                     std::string{ step } + ": " + exception.what() );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        unknown_exception_error( std::string_view step )
        {
            return unexpected_error( grab::ErrorCode::internal_fault,
                                     std::string{ step } + ": unknown exception" );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        closed_error()
        {
            return unexpected_error( grab::ErrorCode::session_closed,
                                     std::string{ kSinkClosedMessage } );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        moved_from_error()
        {
            return unexpected_error( grab::ErrorCode::session_closed,
                                     std::string{ kMovedFromMessage } );
        }

        [[nodiscard]]
        std::string_view
        kind_name( grab::EventKind kind ) noexcept
        {
            switch( kind )
            {
                case grab::EventKind::unspecified :
                    return "unspecified";
                case grab::EventKind::key_down :
                    return "input.key_down";
                case grab::EventKind::key_up :
                    return "input.key_up";
                case grab::EventKind::key_combo :
                    return "input.key_combo";
                case grab::EventKind::mouse_click :
                    return "input.mouse_click";
                case grab::EventKind::mouse_move :
                    return "input.mouse_move";
                case grab::EventKind::idle_start :
                    return "input.idle_start";
                case grab::EventKind::idle_end :
                    return "input.idle_end";
                case grab::EventKind::window_focus_changed :
                    return "window.focus_changed";
                case grab::EventKind::window_title_changed :
                    return "window.title_changed";
                case grab::EventKind::window_created :
                    return "window.created";
                case grab::EventKind::window_closed :
                    return "window.closed";
                case grab::EventKind::a11y_button_clicked :
                    return "a11y.button_clicked";
                case grab::EventKind::a11y_menu_opened :
                    return "a11y.menu_opened";
                case grab::EventKind::a11y_menu_closed :
                    return "a11y.menu_closed";
                case grab::EventKind::a11y_focus_changed :
                    return "a11y.focus_changed";
                case grab::EventKind::a11y_text_changed :
                    return "a11y.text_changed";
                case grab::EventKind::a11y_state_changed :
                    return "a11y.state_changed";
                case grab::EventKind::app_tab_changed :
                    return "app.tab_changed";
                case grab::EventKind::app_context_update :
                    return "app.context_update";
                case grab::EventKind::browser_tab_switched :
                    return "browser.tab_switched";
                case grab::EventKind::state_snapshot :
                    return "state.snapshot";
            }

            return "unspecified";
        }

        [[nodiscard]]
        std::string_view
        category_name( grab::EventCategory category ) noexcept
        {
            switch( category )
            {
                case grab::EventCategory::unspecified :
                    return "unspecified";
                case grab::EventCategory::input :
                    return "input";
                case grab::EventCategory::window :
                    return "window";
                case grab::EventCategory::accessibility :
                    return "accessibility";
                case grab::EventCategory::integration :
                    return "integration";
                case grab::EventCategory::browser :
                    return "browser";
                case grab::EventCategory::state :
                    return "state";
            }

            return "unspecified";
        }

        [[nodiscard]]
        std::string
        json_string( std::string_view value )
        {
            grab::core::json::Writer writer;
            writer.begin_object();
            writer.field( kJsonProbeKey, value );
            writer.end_object();
            const std::string object = std::move( writer ).take();
            return object.substr(
                kJsonStringPrefixSize,
                object.size() - kJsonStringPrefixSize - kJsonStringSuffixSize
            );
        }

        [[nodiscard]]
        grab::Result<std::string>
        format_double( double value )
        {
            if( !std::isfinite( value ) )
            {
                return unexpected_error( grab::ErrorCode::invalid_argument,
                                         "jsonl numeric value is not finite" );
            }

            std::ostringstream output;
            output << std::setprecision( std::numeric_limits<double>::max_digits10 )
                   << value;
            if( output.fail() )
            {
                return unexpected_error( grab::ErrorCode::internal_fault,
                                         "jsonl numeric formatting failed" );
            }
            return output.str();
        }

        class JsonObjectBuilder
        {
            public:

                JsonObjectBuilder()
                {
                    out_ += '{';
                }

                void
                string_field( std::string_view key,
                              std::string_view value )
                {
                    field_prefix( key );
                    out_ += json_string( value );
                }

                void
                uint_field( std::string_view key,
                            std::uint64_t    value )
                {
                    field_prefix( key );
                    out_ += std::to_string( value );
                }

                [[nodiscard]]
                grab::Result<void>
                double_field( std::string_view key,
                              double           value )
                {
                    auto formatted = format_double( value );
                    if( !formatted.has_value() )
                    {
                        return std::unexpected( std::move( formatted.error() ) );
                    }
                    field_prefix( key );
                    out_ += *formatted;
                    return {};
                }

                [[nodiscard]]
                std::string
                take() &&
                {
                    out_ += '}';
                    return std::move( out_ );
                }

            private:

                void
                field_prefix( std::string_view key )
                {
                    if( needs_comma_ )
                    {
                        out_ += ',';
                    }
                    out_         += json_string( key );
                    out_         += ':';
                    needs_comma_  = true;
                }

                std::string out_;
                bool        needs_comma_ = false;
        };

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::InputKey& payload )
        {
            JsonObjectBuilder object;
            object.uint_field( "code", payload.code );
            object.string_field( "name", payload.name );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::KeyCombo& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "text", payload.text );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::MouseClick& payload )
        {
            JsonObjectBuilder object;
            object.uint_field( "button", payload.button );
            object.string_field( "name", payload.name );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::MouseMove& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "axis", payload.axis );
            auto result = object.double_field( "delta", payload.delta );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::Idle& payload )
        {
            JsonObjectBuilder object;
            auto              result = object.double_field( "idle_s", payload.idle_s );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::WindowChange& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "app", payload.app );
            object.string_field( "pid", payload.pid );
            object.string_field( "title", payload.title );
            object.string_field( "prev_title", payload.prev_title );
            auto result = object.double_field( "duration_s", payload.duration_s );
            if( !result.has_value() )
            {
                return std::unexpected( std::move( result.error() ) );
            }
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::A11yEvent& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "app", payload.app );
            object.string_field( "role", payload.role );
            object.string_field( "name", payload.name );
            object.string_field( "detail", payload.detail );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::IntegrationEvent& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "app", payload.app );
            object.string_field( "title", payload.title );
            object.string_field( "detail", payload.detail );
            object.string_field( "json", payload.json );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::BrowserTab& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "app", payload.app );
            object.string_field( "pid", payload.pid );
            object.string_field( "tab_title", payload.tab_title );
            object.string_field( "prev_tab_title", payload.prev_tab_title );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::StateSnapshot& payload )
        {
            JsonObjectBuilder object;
            object.string_field( "json", payload.json );
            return std::move( object ).take();
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_payload( const grab::Payload& payload )
        {
            return std::visit(
                []( const auto& typed_payload ) -> grab::Result<std::string>
                {
                    return serialize_payload( typed_payload );
                },
                payload
            );
        }

        [[nodiscard]]
        grab::Result<std::string>
        serialize_line( const grab::Event& event )
        {
            auto timestamp = format_double( event.timestamp );
            if( !timestamp.has_value() )
            {
                return std::unexpected( std::move( timestamp.error() ) );
            }
            auto data = serialize_payload( event.payload );
            if( !data.has_value() )
            {
                return std::unexpected( std::move( data.error() ) );
            }

            std::string line;
            line += "{\"ts\":";
            line += *timestamp;
            line += ",\"type\":";
            line += json_string( kind_name( event.kind ) );
            line += ",\"tier\":";
            line += json_string( category_name( event.category ) );
            line += ",\"data\":";
            line += *data;
            line += '}';
            return line;
        }

        [[nodiscard]]
        std::int64_t
        days_since_unix_epoch( std::int64_t seconds ) noexcept
        {
            auto       days = seconds / kSecondsPerDay;
            const auto rem  = seconds % kSecondsPerDay;
            if( seconds < 0 && rem != 0 )
            {
                --days;
            }
            return days;
        }

        [[nodiscard]]
        CivilDate
        civil_from_days( std::int64_t days_since_epoch ) noexcept
        {
            const auto z = days_since_epoch + kUnixEpochToCivilOffsetDays;
            const auto era =
                ( z >= 0 ? z : z - ( kDaysPerEra - kCalendarOrdinalBase ) ) /
                kDaysPerEra;
            const auto day_of_era  = z - ( era * kDaysPerEra );
            const auto year_of_era = ( day_of_era -
                                       ( day_of_era / kDaysPerFourYears ) +
                                       ( day_of_era / kDaysPerCentury ) -
                                       ( day_of_era / kDaysPerEra ) ) /
                                     kDaysPerNormalYear;
            auto       year        = year_of_era + ( era * kYearsPerEra );
            const auto day_of_year =
                day_of_era - ( ( kDaysPerNormalYear * year_of_era ) +
                               ( year_of_era / kLeapYearCycle ) -
                               ( year_of_era / kCenturyCycle ) );
            const auto month_prime =
                ( ( ( kMarchMonthNumerator * day_of_year ) + kMarchMonthOffset ) /
                  kDaysPerMarchMonthBlock );
            const auto day =
                day_of_year -
                ( ( ( kDaysPerMarchMonthBlock * month_prime ) + kMarchMonthOffset ) /
                  kMarchMonthNumerator ) +
                kCalendarOrdinalBase;
            const auto month = month_prime < kMarchMonthCutoff
                                 ? month_prime + kMarchToJanuaryOffset
                                 : month_prime - kMarchToCalendarOffset;
            if( month <= kFebruaryNumber )
            {
                ++year;
            }

            return CivilDate{
                .year  = year,
                .month = static_cast<unsigned int>( month ),
                .day   = static_cast<unsigned int>( day ),
            };
        }

        [[nodiscard]]
        grab::Result<std::string>
        date_from_timestamp( double timestamp )
        {
            if( !std::isfinite( timestamp ) )
            {
                return unexpected_error( grab::ErrorCode::invalid_argument,
                                         "jsonl event timestamp is not finite" );
            }

            constexpr auto kMinSeconds =
                static_cast<double>( std::numeric_limits<std::int64_t>::lowest() );
            constexpr auto kMaxSeconds =
                static_cast<double>( std::numeric_limits<std::int64_t>::max() );
            if( timestamp < kMinSeconds || timestamp > kMaxSeconds )
            {
                return unexpected_error( grab::ErrorCode::invalid_argument,
                                         "jsonl event timestamp is out of range" );
            }

            const auto seconds   = static_cast<std::int64_t>( std::floor( timestamp ) );
            const CivilDate date = civil_from_days( days_since_unix_epoch( seconds ) );

            std::ostringstream output;
            output << std::setfill( '0' ) << std::setw( kYearWidth ) << date.year << '-'
                   << std::setw( kMonthDayWidth ) << date.month << '-'
                   << std::setw( kMonthDayWidth ) << date.day;
            return output.str();
        }

        [[nodiscard]]
        std::filesystem::path
        path_for_date( const std::filesystem::path& dir,
                       std::string_view             date )
        {
            return dir / ( std::string{ date } + std::string{ kJsonlSuffix } );
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
            int                       fd_ = kInvalidFd;
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
            return unexpected_error( grab::ErrorCode::invalid_argument,
                                     "jsonl directory is empty" );
        }
        if( options_.buffer_limit == 0U )
        {
            return unexpected_error( grab::ErrorCode::invalid_argument,
                                     "jsonl buffer limit must be greater than zero" );
        }

        std::error_code ec;
        std::filesystem::create_directories( options_.dir, ec );
        if( ec )
        {
            return unexpected_error( grab::ErrorCode::device_inaccessible,
                                     "create_directories: " + ec.message() );
        }

        const bool is_directory = std::filesystem::is_directory( options_.dir, ec );
        if( ec )
        {
            return unexpected_error( grab::ErrorCode::device_inaccessible,
                                     "is_directory: " + ec.message() );
        }
        if( !is_directory )
        {
            return unexpected_error( grab::ErrorCode::device_inaccessible,
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
        if( fd_ != kInvalidFd && current_date_ == date )
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

        constexpr auto kOpenFlags =
            static_cast<int>( static_cast<unsigned int>( O_WRONLY ) |
                              static_cast<unsigned int>( O_APPEND ) |
                              static_cast<unsigned int>( O_CLOEXEC ) );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): POSIX open(2).
        const int fd = ::open( path.c_str(), kOpenFlags );
        if( fd == kPosixFailure )
        {
            return unexpected_error( grab::ErrorCode::device_inaccessible,
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
            return unexpected_error( grab::ErrorCode::device_inaccessible,
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
                                         static_cast<std::size_t>( kBytesPerMegabyte ) );
            const auto written = ::write( fd_, unwritten.data(), count );
            if( written == kWriteFailure )
            {
                const int error_number = errno;
                if( error_number == EINTR )
                {
                    continue;
                }
                return unexpected_error( grab::ErrorCode::device_inaccessible,
                                         posix_message( "write", error_number ) );
            }
            if( written == kNoBytesWritten )
            {
                return unexpected_error( grab::ErrorCode::internal_fault,
                                         "write: wrote zero bytes" );
            }
            unwritten.remove_prefix( static_cast<std::size_t>( written ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::fsync_current_file() const
    {
        while( ::fsync( fd_ ) == kPosixFailure )
        {
            const int error_number = errno;
            if( error_number == EINTR )
            {
                continue;
            }
            return unexpected_error( grab::ErrorCode::device_inaccessible,
                                     posix_message( "fsync", error_number ) );
        }
        return {};
    }

    grab::Result<void>
    JsonlSink::Impl::close_current_file()
    {
        if( fd_ == kInvalidFd )
        {
            current_date_.clear();
            current_path_.clear();
            return {};
        }

        const int close_result = ::close( fd_ );
        fd_                    = kInvalidFd;
        current_date_.clear();
        current_path_.clear();
        if( close_result == kPosixFailure )
        {
            return unexpected_error( grab::ErrorCode::device_inaccessible,
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
            if( entry.path().extension() != kJsonlExtension )
            {
                continue;
            }

            std::error_code ec;
            const bool      regular_file = entry.is_regular_file( ec );
            if( ec )
            {
                return unexpected_error( grab::ErrorCode::device_inaccessible,
                                         "is_regular_file: " + ec.message() );
            }
            if( !regular_file )
            {
                continue;
            }

            const std::uintmax_t size = entry.file_size( ec );
            if( ec )
            {
                return unexpected_error( grab::ErrorCode::device_inaccessible,
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
                return unexpected_error( grab::ErrorCode::device_inaccessible,
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
        constexpr auto kMaxBytes = std::numeric_limits<std::uintmax_t>::max();
        const auto     max_mb    = kMaxBytes / kBytesPerMegabyte;
        const auto     disk_mb   = static_cast<std::uintmax_t>( options_.max_disk_mb );
        if( disk_mb > max_mb )
        {
            return kMaxBytes;
        }
        return disk_mb * kBytesPerMegabyte;
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
