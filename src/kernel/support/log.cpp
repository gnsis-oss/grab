#include "kernel/support/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace grab::log
{
    namespace
    {

        constexpr int              unreadLevel       = -1;
        constexpr int              noSink            = -1;
        constexpr int              stderrDescriptor  = 2;
        constexpr int              openFailure       = -1;
        constexpr int              filePermissions   = 0644;
        constexpr char             fieldSeparator    = ' ';
        constexpr char             keyValueMarker    = '=';
        constexpr char             recordTerminator  = '\n';
        constexpr char             pathSeparator     = '/';
        constexpr char             tagSeparator      = ',';
        constexpr char             controlSubstitute = '.';
        constexpr char             lowestPrintable   = ' ';
        constexpr int              elapsedPrecision  = 6;

        constexpr std::string_view levelVariable     = "GRAB_LOG";
        constexpr std::string_view tagsVariable      = "GRAB_LOG_TAGS";
        constexpr std::string_view fileVariable      = "GRAB_LOG_FILE";

        // Guards every mutation of the sink and the tag allowlist, and is held
        // across the write so a sink cannot be closed under a writer. Writes
        // are one syscall; the contention that costs is the syscall itself.
        std::mutex&
        state_mutex() noexcept
        {
            static std::mutex mutex;
            return mutex;
        }

        std::atomic<int>&
        level_state() noexcept
        {
            static std::atomic<int> level{ unreadLevel };
            return level;
        }

        int&
        sink_descriptor() noexcept
        {
            static int descriptor = noSink;
            return descriptor;
        }

        // stderr is borrowed, never closed. A file sink is owned.
        bool&
        sink_owned() noexcept
        {
            static bool owned = false;
            return owned;
        }

        std::vector<std::string>&
        tag_allowlist() noexcept
        {
            static std::vector<std::string> tags;
            return tags;
        }

        void
        close_sink_locked() noexcept
        {
            if( sink_owned() && sink_descriptor() != noSink )
            {
                ( void )::close( sink_descriptor() );
            }
            sink_descriptor() = noSink;
            sink_owned()      = false;
        }

        [[nodiscard]]
        std::chrono::steady_clock::time_point
        epoch() noexcept
        {
            static const auto zero = std::chrono::steady_clock::now();
            return zero;
        }

        [[nodiscard]]
        bool
        parse_level( std::string_view text,
                     Level&           out ) noexcept
        {
            if( text == "off" || text == "0" )
            {
                out = Level::Off;
                return true;
            }
            if( text == "nominal" || text == "1" )
            {
                out = Level::Nominal;
                return true;
            }
            if( text == "verbose" || text == "2" )
            {
                out = Level::Verbose;
                return true;
            }
            if( text == "debug" || text == "3" )
            {
                out = Level::Debug;
                return true;
            }
            return false;
        }

        // Thread-local record buffer plus a claim flag. An emitter lambda that
        // itself logs would otherwise scribble over the record being built; the
        // nested record is dropped instead.
        struct Buffer
        {
                std::array<char, recordCapacity> storage{};
                bool                             claimed = false;
        };

        Buffer&
        buffer_state() noexcept
        {
            thread_local Buffer buffer;
            return buffer;
        }

    }    // namespace

    // ── Runtime control ────────────────────────────────────

    Level
    runtime_level() noexcept
    {
        const auto current = level_state().load( std::memory_order_relaxed );
        if( current != unreadLevel )
        {
            return static_cast<Level>( current );
        }
        configure_from_environment();
        return static_cast<Level>( level_state().load( std::memory_order_relaxed ) );
    }

    void
    set_runtime_level( Level level ) noexcept
    {
        level_state().store( static_cast<int>( level ), std::memory_order_relaxed );
    }

    void
    set_tag_filter( std::span<const std::string_view> tags ) noexcept
    {
        const std::scoped_lock lock{ state_mutex() };
        tag_allowlist().clear();
        tag_allowlist().reserve( tags.size() );
        for( const auto& entry : tags )
        {
            tag_allowlist().emplace_back( entry );
        }
    }

    void
    sink_to_stderr() noexcept
    {
        const std::scoped_lock lock{ state_mutex() };
        close_sink_locked();
        sink_descriptor() = stderrDescriptor;
        sink_owned()      = false;
    }

    bool
    sink_to_file( const std::filesystem::path& path ) noexcept
    {
        const int descriptor =
            ::open( path.c_str(),
                    O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,    // NOLINT
                    filePermissions );
        if( descriptor == openFailure )
        {
            return false;
        }
        const std::scoped_lock lock{ state_mutex() };
        close_sink_locked();
        sink_descriptor() = descriptor;
        sink_owned()      = true;
        return true;
    }

    void
    sink_off() noexcept
    {
        const std::scoped_lock lock{ state_mutex() };
        close_sink_locked();
    }

    void
    configure_from_environment() noexcept
    {
        // The level doubles as the "already configured" flag: leaving it at
        // unreadLevel is what makes the first read consult the environment.
        auto level = Level::Off;
        if( const char* const requested = std::getenv( levelVariable.data() ) )
        {
            if( !parse_level( requested, level ) )
            {
                level = Level::Off;
            }
        }

        // A named file that cannot be opened falls back to stderr rather than
        // silently discarding the trace the user asked for.
        const char* const file    = std::getenv( fileVariable.data() );
        const bool        to_file = file != nullptr && *file != '\0';
        if( !to_file || !sink_to_file( file ) )
        {
            sink_to_stderr();
        }

        if( const char* const tags = std::getenv( tagsVariable.data() ) )
        {
            std::vector<std::string_view> parsed;
            std::string_view              remaining{ tags };
            while( !remaining.empty() )
            {
                const auto comma = remaining.find( tagSeparator );
                if( comma == std::string_view::npos )
                {
                    parsed.push_back( remaining );
                    break;
                }
                if( comma > 0 )
                {
                    parsed.push_back( remaining.substr( 0, comma ) );
                }
                remaining.remove_prefix( comma + 1 );
            }
            set_tag_filter( parsed );
        }

        level_state().store( static_cast<int>( level ), std::memory_order_relaxed );
    }

    // ── detail ─────────────────────────────────────────────

    namespace detail
    {

        bool
        emitting( Level level ) noexcept
        {
            return static_cast<int>( runtime_level() ) >= static_cast<int>( level );
        }

        char*
        claim_buffer() noexcept
        {
            auto& buffer = buffer_state();
            if( buffer.claimed )
            {
                return nullptr;
            }
            buffer.claimed = true;
            return buffer.storage.data();
        }

        void
        release_buffer() noexcept
        {
            buffer_state().claimed = false;
        }

        double
        elapsed_seconds() noexcept
        {
            // The epoch is captured on first use, so it must be read BEFORE
            // now(): the operands of `now() - epoch()` are unsequenced, and
            // when now() wins the race on the very first record the result is
            // negative.
            const auto zero  = epoch();
            const auto delta = std::chrono::steady_clock::now() - zero;
            return std::chrono::duration<double>( delta ).count();
        }

        std::uint64_t
        thread_id() noexcept
        {
            thread_local const auto identifier =
                static_cast<std::uint64_t>( ::syscall( SYS_gettid ) );
            return identifier;
        }

        std::string_view
        basename( std::string_view path ) noexcept
        {
            const auto slash = path.find_last_of( pathSeparator );
            return slash == std::string_view::npos ? path : path.substr( slash + 1 );
        }

        void
        publish( std::string_view record,
                 std::string_view tag ) noexcept
        {
            const std::scoped_lock lock{ state_mutex() };
            if( sink_descriptor() == noSink || record.empty() )
            {
                return;
            }
            if( !tag_allowlist().empty() )
            {
                const bool matched =
                    std::ranges::find( tag_allowlist(), tag ) != tag_allowlist().end();
                if( !matched )
                {
                    return;
                }
            }
            ( void )::write( sink_descriptor(), record.data(), record.size() );
        }

    }    // namespace detail

    // ── Event ──────────────────────────────────────────────

    Event::Event( Level                       level,
                  const std::source_location& location ) noexcept :
        buffer_{ detail::claim_buffer() }
    {
        if( buffer_ == nullptr )
        {
            return;
        }
        // Prefix: +<elapsed> <level> <tid> <file>:<line>
        if( size_ < recordCapacity )
        {
            buffer_[size_] = '+';
            ++size_;
        }
        const auto elapsed = std::to_chars( buffer_ + size_,
                                            buffer_ + recordCapacity,
                                            detail::elapsed_seconds(),
                                            std::chars_format::fixed,
                                            elapsedPrecision );
        if( elapsed.ec == std::errc{} )
        {
            size_ = static_cast<std::size_t>( elapsed.ptr - buffer_ );
        }
        write_text( " " );
        write_text( name( level ) );
        write_text( " " );
        const auto identifier = std::to_chars( buffer_ + size_,
                                               buffer_ + recordCapacity,
                                               detail::thread_id() );
        if( identifier.ec == std::errc{} )
        {
            size_ = static_cast<std::size_t>( identifier.ptr - buffer_ );
        }
        write_text( " " );
        write_text( detail::basename( location.file_name() ) );
        write_text( ":" );
        const auto line =
            std::to_chars( buffer_ + size_, buffer_ + recordCapacity, location.line() );
        if( line.ec == std::errc{} )
        {
            size_ = static_cast<std::size_t>( line.ptr - buffer_ );
        }
    }

    Event::~Event()
    {
        if( buffer_ == nullptr )
        {
            return;
        }
        if( size_ < recordCapacity )
        {
            buffer_[size_] = recordTerminator;
            ++size_;
        }
        else
        {
            buffer_[recordCapacity - 1] = recordTerminator;
            size_                       = recordCapacity;
        }
        detail::publish( { buffer_, size_ }, { tag_.data(), tag_size_ } );
        detail::release_buffer();
    }

    void
    Event::write_text( std::string_view text ) noexcept
    {
        if( buffer_ == nullptr )
        {
            return;
        }
        const auto room    = recordCapacity - std::min( size_, recordCapacity );
        const auto copying = std::min( text.size(), room );
        for( std::size_t index = 0; index < copying; ++index )
        {
            const char character = text[index];
            // One record is one line: a control character in a value would
            // otherwise split the record in two for anything reading the log.
            buffer_[size_ + index] =
                character < lowestPrintable ? controlSubstitute : character;
        }
        size_ += copying;
    }

    void
    Event::write_key( std::string_view key ) noexcept
    {
        write_text( { &fieldSeparator, 1 } );
        write_text( key );
        write_text( { &keyValueMarker, 1 } );
    }

}    // namespace grab::log
