#pragma once

#include "grab/result.hpp"
#include "session/record.hpp"

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

namespace grab::session
{

    class SessionRegistry
    {
        public:

            explicit SessionRegistry( std::filesystem::path root );

            [[nodiscard]]
            grab::Result<void>
            create( const SessionRecord& record );

            [[nodiscard]]
            grab::Result<void>
            write( const SessionRecord& record );

            [[nodiscard]]
            grab::Result<SessionRecord>
            read( std::string_view name );

            [[nodiscard]]
            grab::Result<int>
            acquire_liveness_lock( std::string_view name );

            [[nodiscard]]
            bool
            is_live( std::string_view name ) const;

            [[nodiscard]]
            std::vector<SessionRecord>
            list();

            [[nodiscard]]
            grab::Result<void>
            remove( std::string_view name );

            [[nodiscard]]
            std::size_t
            reap_dead();

            [[nodiscard]]
            static grab::Result<std::filesystem::path>
            default_root();

        private:

            std::filesystem::path registry_root;

            [[nodiscard]]
            std::filesystem::path
            json_path( std::string_view name ) const;

            [[nodiscard]]
            std::filesystem::path
            lock_path( std::string_view name ) const;
    };

}    // namespace grab::session
