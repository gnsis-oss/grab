#pragma once

// grab's logging facade.
//
// Two independent gates, both ordinary C++ — nothing here is a function-like
// macro, and no call site expands one:
//
//   compileLevel   an `inline constexpr int` from a generated header, one per
//                  build directory (cmake/Logging.cmake). Decides what EXISTS.
//                  Levels above it are discarded by `if constexpr`, so their
//                  emitter lambdas are never instantiated and their arguments
//                  are never evaluated.
//
//   runtime level  an atomic, default OFF. Decides what EMITS. Set by
//                  --log-level, GRAB_LOG, or set_runtime_level().
//
// Records are built through a sink lambda rather than a variadic call:
//
//     log::debug( []( auto& event ) {
//         event.tag( tags::raster ).value( "damage", n ).value( "bytes", b );
//     } );
//
// The lambda is what makes this zero-cost. A plain `log::debug(t, "{}", f())`
// would evaluate `f()` before the callee could decline it; deferring the whole
// body into an invocable is the only way to avoid that without a macro.

// `compileLevel` arrives as an inline constexpr from a generated header, one
// per build directory, rather than as a preprocessor define. See
// cmake/Logging.cmake.
#include "kernel/support/log_config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace grab::log
{

    // Compile levels: 0 disables all log calls; 1 enables nominal lifecycle
    // events; 2 adds verbose branch/candidate detail; 3 adds debug detail.
    inline constexpr int offLevel     = 0;
    inline constexpr int nominalLevel = 1;
    inline constexpr int verboseLevel = 2;
    inline constexpr int debugLevel   = 3;

    enum class Level : int
    {
        Off     = offLevel,
        Nominal = nominalLevel,
        Verbose = verboseLevel,
        Debug   = debugLevel,
    };

    // Longest record a single event may produce. Anything beyond is truncated
    // rather than allocated: a log record is a diagnostic, not a payload, and
    // an allocation on the reactor thread is exactly what this facility exists
    // to keep out of the frame budget.
    inline constexpr std::size_t recordCapacity = 4'096U;

    // Tags are short identifiers. Copied into the record rather than held as a
    // view, because an emitter's temporaries die before the record is flushed.
    inline constexpr std::size_t tagCapacity = 32U;

    [[nodiscard]]
    consteval bool
    enabled( Level level ) noexcept
    {
        return compileLevel >= static_cast<int>( level );
    }

    [[nodiscard]]
    constexpr std::string_view
    name( Level level ) noexcept
    {
        switch( level )
        {
            case Level::Off :
                return "off";
            case Level::Nominal :
                return "nominal";
            case Level::Verbose :
                return "verbose";
            case Level::Debug :
                return "debug";
        }
        return "unknown";
    }

    // ── Runtime control ────────────────────────────────────

    // The level that actually emits. Defaults to Off; on first read, and only
    // once, GRAB_LOG / GRAB_LOG_TAGS / GRAB_LOG_FILE are consulted so a test,
    // an example, or a CLI verb can be lit up without a flag.
    [[nodiscard]]
    Level
    runtime_level() noexcept;
    void
    set_runtime_level( Level level ) noexcept;

    // Empty allowlist means every tag passes. Applied at flush time, because
    // an emitter names its tag partway through building the record.
    void
    set_tag_filter( std::span<const std::string_view> tags ) noexcept;

    // Sinks. At most one is active. Switching closes the previous.
    void
    sink_to_stderr() noexcept;
    [[nodiscard]]
    bool
    sink_to_file( const std::filesystem::path& path ) noexcept;
    void
    sink_off() noexcept;

    // Reads GRAB_LOG, GRAB_LOG_TAGS, GRAB_LOG_FILE. Idempotent; an explicit
    // setter called afterwards wins.
    void
    configure_from_environment() noexcept;

    namespace detail
    {

        // True when `level` should emit right now. One relaxed atomic load on
        // the fast path.
        [[nodiscard]]
        bool
        emitting( Level level ) noexcept;

        // Hands a finished record to the active sink, after tag filtering.
        // `tag` is empty when the emitter never named one.
        void
        publish( std::string_view record,
                 std::string_view tag ) noexcept;

        // Claims the thread's record buffer. Returns nullptr when it is
        // already claimed, which happens only if an emitter lambda logs — a
        // nested record is dropped rather than corrupting the outer one.
        [[nodiscard]]
        char*
        claim_buffer() noexcept;
        void
        release_buffer() noexcept;

        // Seconds since the process's steady-clock zero.
        [[nodiscard]]
        double
        elapsed_seconds() noexcept;

        [[nodiscard]]
        std::uint64_t
        thread_id() noexcept;

        [[nodiscard]]
        std::string_view
        basename( std::string_view path ) noexcept;

    }    // namespace detail

    // ── Event ──────────────────────────────────────────────

    // Composes one record into a thread-local buffer and flushes it with a
    // single write on destruction. Streaming each field straight to stderr —
    // which is what this used to do — costs a stdio call per key and
    // interleaves badly between threads.
    class Event
    {
        public:

            Event( Level                       level,
                   const std::source_location& location ) noexcept;

            Event( const Event& ) = delete;
            Event&
            operator=( const Event& ) = delete;
            Event( Event&& )          = delete;
            Event&
            operator=( Event&& ) = delete;

            ~Event();

            Event&
            tag( std::string_view tag_name ) noexcept
            {
                const auto copied = std::min( tag_name.size(), tagCapacity );
                if( copied > 0 )
                {
                    std::memcpy( tag_.data(), tag_name.data(), copied );
                }
                tag_size_ = copied;
                write_key( "tag" );
                write_text( { tag_.data(), tag_size_ } );
                return *this;
            }

            template<typename Value>
            Event&
            value( std::string_view key,
                   const Value&     val ) noexcept
            {
                write_key( key );
                write_value( val );
                return *this;
            }

        private:

            char*                         buffer_   = nullptr;
            std::size_t                   size_     = 0;
            std::size_t                   tag_size_ = 0;
            std::array<char, tagCapacity> tag_{};

            // Appends, truncating at recordCapacity. Control characters become
            // '.' so one record stays one line — the log is line-oriented and a
            // stray newline in a value would split a record in two.
            void
            write_text( std::string_view text ) noexcept;

            void
            write_key( std::string_view key ) noexcept;

            void
            write_value( std::string_view val ) noexcept
            {
                write_text( val );
            }

            void
            write_value( const std::string& val ) noexcept
            {
                write_text( val );
            }

            void
            write_value( const char* val ) noexcept
            {
                write_text( val == nullptr ? std::string_view{ "null" }
                                           : std::string_view{ val } );
            }

            void
            write_value( bool val ) noexcept
            {
                write_text( val ? "true" : "false" );
            }

            // std::to_chars rather than fprintf: no varargs, no locale, no
            // allocation, and it writes straight into the record buffer.
            template<typename Value>
            requires std::integral<Value> || std::floating_point<Value>
            void
            write_value( Value val ) noexcept
            {
                if( buffer_ == nullptr || size_ >= recordCapacity )
                {
                    return;
                }
                const auto result =
                    std::to_chars( buffer_ + size_, buffer_ + recordCapacity, val );
                if( result.ec == std::errc{} )
                {
                    size_ = static_cast<std::size_t>( result.ptr - buffer_ );
                }
            }

            template<typename Value>
            requires std::is_enum_v<Value>
            void
            write_value( Value val ) noexcept
            {
                write_value( static_cast<std::underlying_type_t<Value>>( val ) );
            }
    };

    // ── Emitters ───────────────────────────────────────────

    // The trailing defaulted source_location is what makes file/line reachable
    // without a macro. A defaulted parameter after a forwarding reference is
    // well-formed, and `current()` is evaluated at the call site.
    template<Level level,
             typename Emit>
    void
    emit( Emit&&               emit_event,
          std::source_location location = std::source_location::current() )
    {
        if constexpr( enabled( level ) )
        {
            if( !detail::emitting( level ) )
            {
                return;
            }
            Event event{ level, location };
            std::forward<Emit>( emit_event )( event );
        }
    }

    template<typename Emit>
    void
    nominal( Emit&&               emit_event,
             std::source_location location = std::source_location::current() )
    {
        emit<Level::Nominal>( std::forward<Emit>( emit_event ), location );
    }

    template<typename Emit>
    void
    verbose( Emit&&               emit_event,
             std::source_location location = std::source_location::current() )
    {
        emit<Level::Verbose>( std::forward<Emit>( emit_event ), location );
    }

    template<typename Emit>
    void
    debug( Emit&&               emit_event,
           std::source_location location = std::source_location::current() )
    {
        emit<Level::Debug>( std::forward<Emit>( emit_event ), location );
    }

}    // namespace grab::log
