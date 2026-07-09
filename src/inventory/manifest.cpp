#include "grab/result.hpp"
#include "inventory/manifest.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace grab::inventory
{

    namespace
    {

        constexpr int kManifestIndent = 2;

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

        [[nodiscard]]
        nlohmann::ordered_json
        entry_to_json( const Entry& entry )
        {
            return nlohmann::ordered_json{
                {         "name",          entry.name},
                {     "category",      entry.category},
                {       "module",        entry.module},
                {  "source_file",   entry.source_file},
                {"render_method", entry.render_method},
                {  "output_path",   entry.output_path},
                {       "status",        entry.status},
                {        "notes",         entry.notes},
            };
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
            nlohmann::ordered_json manifest = nlohmann::ordered_json::array();
            for( const auto& entry : entries )
            {
                manifest.push_back( entry_to_json( entry ) );
            }
            std::string output  = manifest.dump( kManifestIndent );
            output             += '\n';
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
