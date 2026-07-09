#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace grab::core::json
{

    // Minimal streaming JSON writer: enough for doctor output, nothing more.
    class Writer
    {
        public:

            void
            begin_object()
            {
                open( '{' );
            }

            void
            end_object()
            {
                close( '}' );
            }

            void
            begin_array( std::string_view key )
            {
                field_prefix( key );
                open( '[' );
            }

            void
            end_array()
            {
                close( ']' );
            }

            void
            begin_object_in_array()
            {
                element_prefix();
                open( '{' );
            }

            void
            field_object_start( std::string_view key )
            {
                field_prefix( key );
                open( '{' );
            }

            void
            field( std::string_view key,
                   std::string_view value )
            {
                field_prefix( key );
                write_string( value );
            }

            // Without this overload a string literal would bind to the bool
            // overload (pointer-to-bool standard conversion beats string_view's
            // user-defined conversion).
            void
            field( std::string_view key,
                   const char*      value )
            {
                field( key, std::string_view( value ) );
            }

            void
            field( std::string_view key,
                   bool             value )
            {
                field_prefix( key );
                out_         += value ? "true" : "false";
                needs_comma_  = true;
            }

            void
            field( std::string_view key,
                   std::uint64_t    value )
            {
                field_prefix( key );
                out_         += std::to_string( value );
                needs_comma_  = true;
            }

            void
            field( std::string_view key,
                   std::int64_t     value )
            {
                field_prefix( key );
                out_         += std::to_string( value );
                needs_comma_  = true;
            }

            [[nodiscard]]
            std::string
            take() &&
            {
                return std::move( out_ );
            }

        private:

            void
            open( char bracket )
            {
                out_         += bracket;
                needs_comma_  = false;
            }

            void
            close( char bracket )
            {
                out_         += bracket;
                needs_comma_  = true;
            }

            void
            element_prefix()
            {
                if( needs_comma_ )
                {
                    out_ += ',';
                }
                needs_comma_ = false;
            }

            void
            field_prefix( std::string_view key )
            {
                element_prefix();
                write_string( key );
                out_         += ':';
                needs_comma_  = false;
            }

            void
            write_string( std::string_view value )
            {
                constexpr std::string_view kHexDigits              = "0123456789abcdef";
                constexpr auto             kControlCharacterLimit  = 0X20U;
                constexpr auto             kHighNibbleShift        = 4U;
                constexpr auto             kNibbleMask             = 0X0FU;

                out_                                              += '"';
                for( const char current : value )
                {
                    switch( current )
                    {
                        case '"' :
                            out_ += "\\\"";
                            break;
                        case '\\' :
                            out_ += "\\\\";
                            break;
                        case '\n' :
                            out_ += "\\n";
                            break;
                        case '\t' :
                            out_ += "\\t";
                            break;
                        case '\r' :
                            out_ += "\\r";
                            break;
                        default :
                            const auto byte = static_cast<unsigned char>( current );
                            if( byte < kControlCharacterLimit )
                            {
                                const auto high_nibble =
                                    static_cast<std::string_view::size_type>(
                                        ( byte >> kHighNibbleShift ) & kNibbleMask
                                    );
                                const auto low_nibble =
                                    static_cast<std::string_view::size_type>(
                                        byte & kNibbleMask
                                    );
                                out_ += "\\u00";
                                out_ += kHexDigits[high_nibble];
                                out_ += kHexDigits[low_nibble];
                            }
                            else
                            {
                                out_ += current;
                            }
                            break;
                    }
                }
                out_         += '"';
                needs_comma_  = true;
            }

            std::string out_;
            bool        needs_comma_ = false;
    };

}    // namespace grab::core::json
