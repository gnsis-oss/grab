#include "grab/result.hpp"
#include "inventory/manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace grab::inventory
{

    namespace
    {

        constexpr char             quote                   = '"';
        constexpr char             backslash               = '\\';
        constexpr char             newline                 = '\n';
        constexpr char             tab                     = '\t';
        constexpr char             carriage_return         = '\r';
        constexpr char             space                   = ' ';
        constexpr char             comma                   = ',';
        constexpr char             object_open             = '{';
        constexpr char             object_close            = '}';
        constexpr char             array_open              = '[';
        constexpr char             array_close             = ']';
        constexpr std::size_t      object_indent           = 2U;
        constexpr std::size_t      field_indent            = 4U;
        constexpr auto             control_character_limit = 0X20U;
        constexpr auto             high_nibble_shift       = 4U;
        constexpr auto             nibble_mask             = 0X0FU;
        constexpr std::string_view hex_digits              = "0123456789abcdef";

        [[nodiscard]]
        std::string
        filesystem_error_message( std::string_view       operation,
                                  const std::error_code& error )
        {
            std::string message{ operation };
            message += ": ";
            message += error.message();
            return message;
        }

        void
        append_indent( std::string& output,
                       std::size_t  count )
        {
            output.append( count, space );
        }

        void
        append_control_escape( std::string&  output,
                               unsigned char byte )
        {
            const auto high_nibble =
                static_cast<std::string_view::size_type>( ( byte >> high_nibble_shift ) &
                                                          nibble_mask );
            const auto low_nibble =
                static_cast<std::string_view::size_type>( byte & nibble_mask );
            output += "\\u00";
            output += hex_digits.at( high_nibble );
            output += hex_digits.at( low_nibble );
        }

        void
        append_json_string( std::string&     output,
                            std::string_view value )
        {
            output += quote;
            for( const char current : value )
            {
                switch( current )
                {
                    case quote :
                        output += "\\\"";
                        break;
                    case backslash :
                        output += "\\\\";
                        break;
                    case newline :
                        output += "\\n";
                        break;
                    case tab :
                        output += "\\t";
                        break;
                    case carriage_return :
                        output += "\\r";
                        break;
                    default :
                        const auto byte = static_cast<unsigned char>( current );
                        if( byte < control_character_limit )
                        {
                            append_control_escape( output, byte );
                        }
                        else
                        {
                            output += current;
                        }
                        break;
                }
            }
            output += quote;
        }

        void
        append_field( std::string&     output,
                      std::string_view key,
                      std::string_view value,
                      bool             trailing_comma )
        {
            append_indent( output, field_indent );
            append_json_string( output, key );
            output += ": ";
            append_json_string( output, value );
            if( trailing_comma )
            {
                output += comma;
            }
            output += newline;
        }

        void
        append_entry( std::string& output,
                      const Entry& entry,
                      bool         trailing_comma )
        {
            append_indent( output, object_indent );
            output += object_open;
            output += newline;
            append_field( output, "name", entry.name, true );
            append_field( output, "category", entry.category, true );
            append_field( output, "module", entry.module, true );
            append_field( output, "source_file", entry.source_file, true );
            append_field( output, "render_method", entry.render_method, true );
            append_field( output, "output_path", entry.output_path, true );
            append_field( output, "status", entry.status, true );
            append_field( output, "notes", entry.notes, false );
            append_indent( output, object_indent );
            output += object_close;
            if( trailing_comma )
            {
                output += comma;
            }
            output += newline;
        }

        [[nodiscard]]
        bool
        manifest_entry_less( const Entry& lhs,
                             const Entry& rhs )
        {
            return std::tie( lhs.render_method, lhs.output_path, lhs.name ) <
                   std::tie( rhs.render_method, rhs.output_path, rhs.name );
        }

        [[nodiscard]]
        std::string
        render_manifest( const std::vector<Entry>& entries )
        {
            std::string output;
            output += array_open;
            output += newline;
            for( std::size_t index = 0U; index < entries.size(); ++index )
            {
                append_entry( output,
                              entries.at( index ),
                              index + 1U != entries.size() );
            }
            output += array_close;
            output += newline;
            return output;
        }

    }    // namespace

    grab::Result<void>
    write_manifest( std::string_view   path,
                    std::vector<Entry> entries )
    {
        std::ranges::sort( entries, manifest_entry_less );
        const std::string           contents = render_manifest( entries );

        const std::filesystem::path output_path{ std::string{ path } };
        const std::filesystem::path parent = output_path.parent_path();
        if( !parent.empty() )
        {
            std::error_code error;
            std::filesystem::create_directories( parent, error );
            if( error )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   filesystem_error_message( "create manifest directory",
                                                             error ) );
            }
        }

        std::ofstream output{ output_path };
        if( !output.is_open() )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               "failed to open manifest path" );
        }
        output << contents;
        if( !output )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               "failed to write manifest path" );
        }
        return {};
    }

}    // namespace grab::inventory
