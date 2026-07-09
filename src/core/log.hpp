#pragma once

#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#define GRAB_LOG_LEVEL_OFF     0
#define GRAB_LOG_LEVEL_NOMINAL 1
#define GRAB_LOG_LEVEL_VERBOSE 2
#define GRAB_LOG_LEVEL_DEBUG   3

#ifndef LOG_COMPILE_LEVEL
    #define LOG_COMPILE_LEVEL GRAB_LOG_LEVEL_NOMINAL
#endif

namespace grab::log
{

    // Compile levels: 0 disables all log calls; 1 enables nominal lifecycle
    // events; 2 adds verbose branch/candidate detail; 3 adds debug detail.
    inline constexpr int kOffLevel     = GRAB_LOG_LEVEL_OFF;
    inline constexpr int kNominalLevel = GRAB_LOG_LEVEL_NOMINAL;
    inline constexpr int kVerboseLevel = GRAB_LOG_LEVEL_VERBOSE;
    inline constexpr int kDebugLevel   = GRAB_LOG_LEVEL_DEBUG;
    inline constexpr int kCompileLevel = LOG_COMPILE_LEVEL;

    enum class Level : int
    {
        off     = kOffLevel,
        nominal = kNominalLevel,
        verbose = kVerboseLevel,
        debug   = kDebugLevel,
    };

    [[nodiscard]]
    consteval bool
    enabled( Level level ) noexcept
    {
        return kCompileLevel >= static_cast<int>( level );
    }

    class Event
    {
        public:

            Event()               = default;

            Event( const Event& ) = delete;
            Event&
            operator=( const Event& ) = delete;
            Event( Event&& )          = delete;
            Event&
            operator=( Event&& ) = delete;

            ~Event()
            {
                ( void )std::fputc( '\n', stderr );
            }

            Event&
            tag( std::string_view tag_name ) noexcept
            {
                write_key_value_prefix( "tag" );
                write_text( tag_name );
                return *this;
            }

            template<typename Value>
            Event&
            value( std::string_view key,
                   const Value&     value ) noexcept
            {
                write_key_value_prefix( key );
                write_value( value );
                return *this;
            }

        private:

            bool first_ = true;

            void
            write_separator() noexcept
            {
                if( first_ )
                {
                    first_ = false;
                    return;
                }
                ( void )std::fputc( ' ', stderr );
            }

            void
            write_key_value_prefix( std::string_view key ) noexcept
            {
                write_separator();
                write_text( key );
                ( void )std::fputc( '=', stderr );
            }

            static void
            write_text( std::string_view text ) noexcept
            {
                ( void )std::fwrite( text.data(), sizeof( char ), text.size(), stderr );
            }

            static void
            write_value( std::string_view value ) noexcept
            {
                write_text( value );
            }

            static void
            write_value( const std::string& value ) noexcept
            {
                write_text( value );
            }

            static void
            write_value( const char* value ) noexcept
            {
                if( value == nullptr )
                {
                    write_text( "null" );
                    return;
                }
                ( void )std::fputs( value, stderr );
            }

            static void
            write_value( bool value ) noexcept
            {
                write_text( value ? "true" : "false" );
            }

            template<std::signed_integral Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr, "%lld", static_cast<long long>( value ) );
            }

            template<std::unsigned_integral Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr,
                                      "%llu",
                                      static_cast<unsigned long long>( value ) );
            }

            template<std::floating_point Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr, "%.17g", static_cast<double>( value ) );
            }

            template<typename Value>
            requires std::is_enum_v<Value>
            static void
            write_value( Value value ) noexcept
            {
                write_value( static_cast<std::underlying_type_t<Value>>( value ) );
            }
    };

    template<Level level,
             typename Emit>
    void
    emit( Emit&& emit_event )
    {
        if constexpr( enabled( level ) )
        {
            Event event;
            std::forward<Emit>( emit_event )( event );
        }
    }

    template<typename Emit>
    void
    nominal( Emit&& emit_event )
    {
        emit<Level::nominal>( std::forward<Emit>( emit_event ) );
    }

    template<typename Emit>
    void
    verbose( Emit&& emit_event )
    {
        emit<Level::verbose>( std::forward<Emit>( emit_event ) );
    }

    template<typename Emit>
    void
    debug( Emit&& emit_event )
    {
        emit<Level::debug>( std::forward<Emit>( emit_event ) );
    }

}    // namespace grab::log
