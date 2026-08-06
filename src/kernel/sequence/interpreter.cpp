#include "grab/result.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/sequence.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace grab::kernel::sequence
{

    // PHASE 0 STUB. The interpreter unit replaces this file.
    grab::Result<Sequence>
    parse( std::string_view json )
    {
        ( void )json;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence parse is not implemented yet" );
    }

    grab::Result<Sequence>
    load( const std::filesystem::path& path )
    {
        ( void )path;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence load is not implemented yet" );
    }

    grab::Result<std::string>
    to_json( const Sequence& sequence )
    {
        ( void )sequence;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence to_json is not implemented yet" );
    }

}    // namespace grab::kernel::sequence
