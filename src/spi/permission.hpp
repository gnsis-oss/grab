#pragma once

#include "grab/result.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace grab::core
{

    enum class PermissionState : std::uint8_t
    {
        Granted,
        Needed,
        Denied,
    };

    class PermissionBroker
    {
        public:

            PermissionBroker()                          = default;
            PermissionBroker( const PermissionBroker& ) = delete;
            PermissionBroker&
            operator=( const PermissionBroker& )   = delete;
            PermissionBroker( PermissionBroker&& ) = delete;
            PermissionBroker&
            operator=( PermissionBroker&& ) = delete;
            virtual ~PermissionBroker()     = default;

            [[nodiscard]]
            virtual PermissionState
            query( std::string_view permission ) = 0;

            [[nodiscard]]
            virtual Result<void>
            request( std::string_view permission ) = 0;
    };

    // Phase 1: X11 backends need no permissions.
    class NoPermissionBroker final : public PermissionBroker
    {
        public:

            [[nodiscard]]
            PermissionState
            query( std::string_view permission ) override;

            [[nodiscard]]
            Result<void>
            request( std::string_view permission ) override;
    };

    struct StateDir
    {
            using GetEnv = std::function<std::optional<std::string>( std::string_view )>;

            [[nodiscard]]
            static std::filesystem::path
            resolve( const GetEnv& get_env );

            [[nodiscard]]
            static Result<void>
            write_atomic( const std::filesystem::path& file,
                          std::string_view             contents );
    };

}    // namespace grab::core
