#include "config/pattern.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab::config
{
    namespace
    {

        enum class TokenKind : std::uint8_t
        {
            Timestamp,
            Date,
            Time,
            Sequence,
        };

        struct TokenValues
        {
                std::string timestamp;
                std::string date;
                std::string time;
                std::string sequence;
        };

        struct MatchState
        {
                std::optional<std::string_view> timestamp;
                std::optional<std::string_view> date;
                std::optional<std::string_view> time;
                std::optional<std::string_view> sequence;
        };

        constexpr std::size_t beginIndex          = 0U;
        constexpr std::size_t nextOffset          = 1U;
        constexpr std::size_t yearWidth           = 4U;
        constexpr std::size_t componentWidth      = 2U;
        constexpr std::size_t millisecondWidth    = 3U;
        constexpr std::size_t sequenceWidth       = 5U;
        constexpr std::size_t dateWidth           = 8U;
        constexpr std::size_t timeWidth           = 6U;
        constexpr std::size_t timestampTimeOffset = 9U;
        constexpr std::size_t timestampDotOffset  = 15U;
        constexpr std::size_t timestampMsOffset   = 16U;
        constexpr std::size_t timestampWidth      = 19U;
        constexpr std::size_t maximumSequenceWidth =
            std::numeric_limits<std::uint32_t>::digits10 + nextOffset;

        constexpr int              minimumYear          = 0;
        constexpr int              maximumYear          = 9'999;
        constexpr int              epochYearOffset      = 1'900;
        constexpr int              calendarMonthOffset  = 1;
        constexpr std::uint32_t    decimalBase          = 10U;
        constexpr std::uint32_t    maximumHour          = 23U;
        constexpr std::uint32_t    maximumMinuteSecond  = 59U;

        constexpr char             openingBrace         = '{';
        constexpr char             closingBrace         = '}';
        constexpr char             zeroCharacter        = '0';
        constexpr char             nineCharacter        = '9';
        constexpr char             timestampSeparator   = 'T';
        constexpr char             millisecondSeparator = '.';

        constexpr std::string_view filenamePointer      = "/watch/filename";
        constexpr std::string_view errorSeparator       = ": ";
        constexpr std::string_view pngExtension         = ".png";
        constexpr std::string_view parentDirectory      = "..";
        constexpr std::string_view timestampToken       = "{timestamp}";
        constexpr std::string_view dateToken            = "{date}";
        constexpr std::string_view timeToken            = "{time}";
        constexpr std::string_view sequenceToken        = "{seq}";
        constexpr std::string_view relativeReason       = "must be relative";
        constexpr std::string_view parentReason =
            "must not contain '..' path components";
        constexpr std::string_view unknownReason      = "unknown token ";
        constexpr std::string_view unterminatedReason = "unterminated token";
        constexpr std::string_view closingReason      = "unexpected closing brace";
        constexpr std::string_view yearReason =
            "timestamp year must be between 0000 and 9999";
        constexpr std::string_view timeReason = "timestamp cannot be represented in UTC";

        [[nodiscard]]
        std::unexpected<grab::Error>
        pattern_error( std::string reason )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ filenamePointer } +
                                   std::string{ errorSeparator } +
                                   std::move( reason ) );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_relative_path( std::string_view value )
        {
            const std::filesystem::path path{ std::string{ value } };
            if( path.is_absolute() || path.has_root_name() || path.has_root_directory() )
            {
                return pattern_error( std::string{ relativeReason } );
            }
            for( const auto& component : path )
            {
                if( component.generic_string() == parentDirectory )
                {
                    return pattern_error( std::string{ parentReason } );
                }
            }
            return {};
        }

        [[nodiscard]]
        std::string
        effective_pattern( std::string_view pattern )
        {
            std::string effective{ pattern };
            if( !effective.ends_with( pngExtension ) )
            {
                effective.append( pngExtension );
            }
            return effective;
        }

        [[nodiscard]]
        std::optional<TokenKind>
        token_at( std::string_view pattern )
        {
            if( pattern.starts_with( timestampToken ) )
            {
                return TokenKind::Timestamp;
            }
            if( pattern.starts_with( dateToken ) )
            {
                return TokenKind::Date;
            }
            if( pattern.starts_with( timeToken ) )
            {
                return TokenKind::Time;
            }
            if( pattern.starts_with( sequenceToken ) )
            {
                return TokenKind::Sequence;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::string_view
        token_text( TokenKind kind )
        {
            switch( kind )
            {
                case TokenKind::Timestamp :
                    return timestampToken;
                case TokenKind::Date :
                    return dateToken;
                case TokenKind::Time :
                    return timeToken;
                case TokenKind::Sequence :
                    return sequenceToken;
            }
            return {};
        }

        [[nodiscard]]
        std::string
        padded_decimal( std::uint32_t value,
                        std::size_t   width )
        {
            std::string digits = std::to_string( value );
            if( digits.size() < width )
            {
                digits.insert( beginIndex, width - digits.size(), zeroCharacter );
            }
            return digits;
        }

        void
        append_padded( std::string&  output,
                       std::uint32_t value,
                       std::size_t   width )
        {
            output.append( padded_decimal( value, width ) );
        }

        [[nodiscard]]
        grab::Result<TokenValues>
        make_token_values( const PatternContext& context )
        {
            const auto milliseconds =
                std::chrono::floor<std::chrono::milliseconds>( context.now );
            const auto seconds =
                std::chrono::floor<std::chrono::seconds>( milliseconds );
            const std::time_t raw_time = std::chrono::system_clock::to_time_t( seconds );
            std::tm           utc_time{};
            if( ::gmtime_r( &raw_time, &utc_time ) == nullptr )
            {
                return pattern_error( std::string{ timeReason } );
            }
            const int year = utc_time.tm_year + epochYearOffset;
            if( year < minimumYear || year > maximumYear )
            {
                return pattern_error( std::string{ yearReason } );
            }

            TokenValues values;
            values.date.reserve( dateWidth );
            append_padded( values.date, static_cast<std::uint32_t>( year ), yearWidth );
            append_padded( values.date,
                           static_cast<std::uint32_t>( utc_time.tm_mon +
                                                       calendarMonthOffset ),
                           componentWidth );
            append_padded( values.date,
                           static_cast<std::uint32_t>( utc_time.tm_mday ),
                           componentWidth );

            values.time.reserve( timeWidth );
            append_padded( values.time,
                           static_cast<std::uint32_t>( utc_time.tm_hour ),
                           componentWidth );
            append_padded( values.time,
                           static_cast<std::uint32_t>( utc_time.tm_min ),
                           componentWidth );
            append_padded( values.time,
                           static_cast<std::uint32_t>( utc_time.tm_sec ),
                           componentWidth );

            values.timestamp.reserve( timestampWidth );
            values.timestamp.append( values.date );
            values.timestamp.push_back( timestampSeparator );
            values.timestamp.append( values.time );
            values.timestamp.push_back( millisecondSeparator );
            append_padded(
                values.timestamp,
                static_cast<std::uint32_t>( ( milliseconds - seconds ).count() ),
                millisecondWidth
            );
            values.sequence = padded_decimal( context.seq, sequenceWidth );
            return values;
        }

        [[nodiscard]]
        std::string_view
        token_value( TokenKind          kind,
                     const TokenValues& values )
        {
            switch( kind )
            {
                case TokenKind::Timestamp :
                    return values.timestamp;
                case TokenKind::Date :
                    return values.date;
                case TokenKind::Time :
                    return values.time;
                case TokenKind::Sequence :
                    return values.sequence;
            }
            return {};
        }

        [[nodiscard]]
        bool
        parse_decimal( std::string_view text,
                       std::uint32_t&   value )
        {
            if( text.empty() )
            {
                return false;
            }
            value = static_cast<std::uint32_t>( beginIndex );
            for( const char character : text )
            {
                if( character < zeroCharacter || character > nineCharacter )
                {
                    return false;
                }
                const auto digit =
                    static_cast<std::uint32_t>( character - zeroCharacter );
                if( value >
                    ( std::numeric_limits<std::uint32_t>::max() - digit ) /
                    decimalBase )
                {
                    return false;
                }
                value = ( value * decimalBase ) + digit;
            }
            return true;
        }

        [[nodiscard]]
        bool
        valid_date( std::string_view text )
        {
            if( text.size() != dateWidth )
            {
                return false;
            }
            std::uint32_t year{};
            std::uint32_t month{};
            std::uint32_t day{};
            if( !parse_decimal( text.substr( beginIndex, yearWidth ), year ) ||
                !parse_decimal( text.substr( yearWidth, componentWidth ), month ) ||
                !parse_decimal( text.substr( yearWidth + componentWidth,
                                             componentWidth ),
                                day ) )
            {
                return false;
            }
            return std::chrono::year_month_day{
                std::chrono::year{ static_cast<int>( year ) },
                std::chrono::month{ month },
                std::chrono::day{ day },
            }
                .ok();
        }

        [[nodiscard]]
        bool
        valid_time( std::string_view text )
        {
            if( text.size() != timeWidth )
            {
                return false;
            }
            std::uint32_t hour{};
            std::uint32_t minute{};
            std::uint32_t second{};
            if( !parse_decimal( text.substr( beginIndex, componentWidth ), hour ) ||
                !parse_decimal( text.substr( componentWidth, componentWidth ),
                                minute ) ||
                !parse_decimal( text.substr( componentWidth + componentWidth,
                                             componentWidth ),
                                second ) )
            {
                return false;
            }
            return hour <=
                   maximumHour &&
                   minute <=
                   maximumMinuteSecond &&
                   second <= maximumMinuteSecond;
        }

        [[nodiscard]]
        bool
        valid_token( TokenKind        kind,
                     std::string_view value )
        {
            switch( kind )
            {
                case TokenKind::Timestamp :
                    {
                        std::uint32_t milliseconds{};
                        return value.size() ==
                               timestampWidth &&
                               value.at( dateWidth ) ==
                               timestampSeparator &&
                               value.at( timestampDotOffset ) ==
                               millisecondSeparator &&
                               valid_date( value.substr( beginIndex, dateWidth ) ) &&
                               valid_time( value.substr( timestampTimeOffset,
                                                         timeWidth ) ) &&
                               parse_decimal( value.substr( timestampMsOffset,
                                                            millisecondWidth ),
                                              milliseconds );
                    }
                case TokenKind::Date :
                    return valid_date( value );
                case TokenKind::Time :
                    return valid_time( value );
                case TokenKind::Sequence :
                    {
                        std::uint32_t sequence{};
                        return value.size() >=
                               sequenceWidth &&
                               value.size() <=
                               maximumSequenceWidth &&
                               parse_decimal( value, sequence ) &&
                               padded_decimal( sequence, sequenceWidth ) == value;
                    }
            }
            return false;
        }

        [[nodiscard]]
        bool
        accept_value( std::optional<std::string_view>& observed,
                      std::string_view                 value )
        {
            if( observed.has_value() && *observed != value )
            {
                return false;
            }
            observed = value;
            return true;
        }

        [[nodiscard]]
        bool
        accept_token( TokenKind        kind,
                      std::string_view value,
                      MatchState&      state )
        {
            if( !valid_token( kind, value ) )
            {
                return false;
            }
            switch( kind )
            {
                case TokenKind::Timestamp :
                    {
                        const std::string_view date =
                            value.substr( beginIndex, dateWidth );
                        const std::string_view time =
                            value.substr( timestampTimeOffset, timeWidth );
                        return accept_value( state.timestamp, value ) &&
                               accept_value( state.date, date ) &&
                               accept_value( state.time, time );
                    }
                case TokenKind::Date :
                    return accept_value( state.date, value );
                case TokenKind::Time :
                    return accept_value( state.time, value );
                case TokenKind::Sequence :
                    return accept_value( state.sequence, value );
            }
            return false;
        }

        [[nodiscard]]
        std::size_t
        fixed_token_width( TokenKind kind )
        {
            switch( kind )
            {
                case TokenKind::Timestamp :
                    return timestampWidth;
                case TokenKind::Date :
                    return dateWidth;
                case TokenKind::Time :
                    return timeWidth;
                case TokenKind::Sequence :
                    break;
            }
            return beginIndex;
        }

        [[nodiscard]]
        bool
        match_from( std::string_view pattern,
                    std::size_t      pattern_position,
                    std::string_view name,
                    std::size_t      name_position,
                    MatchState       state )
        {
            while( pattern_position < pattern.size() )
            {
                const char character = pattern.at( pattern_position );
                if( character != openingBrace )
                {
                    if( character ==
                        closingBrace ||
                        name_position >=
                        name.size() ||
                        name.at( name_position ) != character )
                    {
                        return false;
                    }
                    ++pattern_position;
                    ++name_position;
                    continue;
                }

                const auto kind = token_at( pattern.substr( pattern_position ) );
                if( !kind.has_value() )
                {
                    return false;
                }
                const std::size_t next_pattern =
                    pattern_position + token_text( *kind ).size();
                if( *kind == TokenKind::Sequence && !state.sequence.has_value() )
                {
                    for( std::size_t width = sequenceWidth;
                         width <= maximumSequenceWidth;
                         ++width )
                    {
                        if( width <= name.size() - name_position )
                        {
                            MatchState             candidate_state = state;
                            const std::string_view value =
                                name.substr( name_position, width );
                            if( accept_token( *kind, value, candidate_state ) &&
                                match_from( pattern,
                                            next_pattern,
                                            name,
                                            name_position + width,
                                            candidate_state ) )
                            {
                                return true;
                            }
                        }
                    }
                    return false;
                }

                const std::size_t width =
                    *kind == TokenKind::Sequence
                        ? state.sequence.value_or( std::string_view{} ).size()
                        : fixed_token_width( *kind );
                if( name_position > name.size() || width > name.size() - name_position )
                {
                    return false;
                }
                const std::string_view value = name.substr( name_position, width );
                if( !accept_token( *kind, value, state ) )
                {
                    return false;
                }
                pattern_position  = next_pattern;
                name_position    += width;
            }
            return name_position == name.size();
        }

    }    // namespace

    grab::Result<std::string>
    render_filename( std::string_view      pattern,
                     const PatternContext& context )
    {
        if( const auto valid = validate_relative_path( pattern ); !valid )
        {
            return std::unexpected{ valid.error() };
        }
        const auto values = make_token_values( context );
        if( !values )
        {
            return std::unexpected{ values.error() };
        }

        const std::string effective = effective_pattern( pattern );
        std::string       rendered;
        rendered.reserve( effective.size() );
        std::size_t position = beginIndex;
        while( position < effective.size() )
        {
            const char character = effective.at( position );
            if( character != openingBrace )
            {
                if( character == closingBrace )
                {
                    return pattern_error( std::string{ closingReason } );
                }
                rendered.push_back( character );
                ++position;
                continue;
            }

            const auto kind =
                token_at( std::string_view{ effective }.substr( position ) );
            if( !kind.has_value() )
            {
                const std::size_t close =
                    effective.find( closingBrace, position + nextOffset );
                if( close == std::string::npos )
                {
                    return pattern_error( std::string{ unterminatedReason } );
                }
                std::string reason{ unknownReason };
                reason.append( effective.substr( position,
                                                 close - position + nextOffset ) );
                return pattern_error( std::move( reason ) );
            }
            rendered.append( token_value( *kind, *values ) );
            position += token_text( *kind ).size();
        }
        return rendered;
    }

    bool
    matches_pattern( std::string_view pattern,
                     std::string_view name )
    {
        if( !validate_relative_path( pattern ) || !validate_relative_path( name ) )
        {
            return false;
        }
        const std::string effective = effective_pattern( pattern );
        return match_from( effective, beginIndex, name, beginIndex, MatchState{} );
    }

}    // namespace grab::config
