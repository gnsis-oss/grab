#pragma once

#include "grab/enum_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab
{

    enum class ErrorCategory : std::uint8_t
    {
        Environment,
        Permission,
        Target,
        Protocol,
        Usage,
        InternalFault,
    };

    // Numeric values are stable within a major version (spec section 8). The
    // high byte encodes the category.
    enum class ErrorCode : std::uint16_t
    {
        EnvironmentChanged    = 0X01'00,
        DisplayUnavailable    = 0X01'01,
        DeviceInaccessible    = 0X01'02,

        PermissionNeeded      = 0X02'00,
        PermissionDenied      = 0X02'01,

        WindowNotFound        = 0X03'00,
        StaleWindow           = 0X03'01,
        GeometryUntrusted     = 0X03'02,

        CapabilityUnavailable = 0X04'00,
        ProviderFailed        = 0X04'01,
        ProtocolError         = 0X04'02,

        InvalidArgument       = 0X05'00,
        IllegalFromCallback   = 0X05'01,
        UnsupportedCharacter  = 0X05'02,
        SessionClosed         = 0X05'03,
        SessionExists         = 0X05'04,
        SessionNotFound       = 0X05'05,
        SessionDead           = 0X05'06,

        InternalFault         = 0X06'00,
    };

    namespace detail
    {

        inline constexpr std::size_t errorCodeCount = 19U;

        inline constexpr auto        errorCodeNames = EnumTable{
            std::to_array( {
                enum_entry( ErrorCode::EnvironmentChanged, "environment_changed" ),
                enum_entry( ErrorCode::DisplayUnavailable, "display_unavailable" ),
                enum_entry( ErrorCode::DeviceInaccessible, "device_inaccessible" ),
                enum_entry( ErrorCode::PermissionNeeded, "permission_needed" ),
                enum_entry( ErrorCode::PermissionDenied, "permission_denied" ),
                enum_entry( ErrorCode::WindowNotFound, "window_not_found" ),
                enum_entry( ErrorCode::StaleWindow, "stale_window" ),
                enum_entry( ErrorCode::GeometryUntrusted, "geometry_untrusted" ),
                enum_entry( ErrorCode::CapabilityUnavailable, "capability_unavailable" ),
                enum_entry( ErrorCode::ProviderFailed, "provider_failed" ),
                enum_entry( ErrorCode::ProtocolError, "protocol_error" ),
                enum_entry( ErrorCode::InvalidArgument, "invalid_argument" ),
                enum_entry( ErrorCode::IllegalFromCallback, "illegal_from_callback" ),
                enum_entry( ErrorCode::UnsupportedCharacter, "unsupported_character" ),
                enum_entry( ErrorCode::SessionClosed, "session_closed" ),
                enum_entry( ErrorCode::SessionExists, "session_exists" ),
                enum_entry( ErrorCode::SessionNotFound, "session_not_found" ),
                enum_entry( ErrorCode::SessionDead, "session_dead" ),
                enum_entry( ErrorCode::InternalFault, "internal_fault" ),
            } ),
        };
        static_assert( enum_table_has_count( errorCodeNames,
                                             errorCodeCount ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr ErrorCategory
    category_of( ErrorCode code ) noexcept
    {
        constexpr std::uint16_t categoryShift          = 8U;
        constexpr std::uint16_t environmentCategoryTag = 0X01U;
        constexpr std::uint16_t permissionCategoryTag  = 0X02U;
        constexpr std::uint16_t targetCategoryTag      = 0X03U;
        constexpr std::uint16_t protocolCategoryTag    = 0X04U;
        constexpr std::uint16_t usageCategoryTag       = 0X05U;

        switch( static_cast<std::uint16_t>( code ) >> categoryShift )
        {
            case environmentCategoryTag :
                return ErrorCategory::Environment;
            case permissionCategoryTag :
                return ErrorCategory::Permission;
            case targetCategoryTag :
                return ErrorCategory::Target;
            case protocolCategoryTag :
                return ErrorCategory::Protocol;
            case usageCategoryTag :
                return ErrorCategory::Usage;
            default :
                return ErrorCategory::InternalFault;
        }
    }

    [[nodiscard]]
    constexpr std::string_view
    name_of( ErrorCode code ) noexcept
    {
        return detail::errorCodeNames.text_of( code, "internal_fault" );
    }

    struct ProviderAttempt
    {
            std::string provider;
            std::string reason;
    };

    struct Error
    {
            ErrorCode   code = ErrorCode::InternalFault;
            std::string message;
            std::string capability;                   // empty when not capability-bound
            std::string target;
            std::vector<ProviderAttempt> attempts;    // provider chain that was tried
    };

    template<typename T>
    using Result = std::expected<T, Error>;

    [[nodiscard]]
    inline std::unexpected<Error>
    fail( ErrorCode   code,
          std::string message )
    {
        return std::unexpected( Error{
            .code       = code,
            .message    = std::move( message ),
            .capability = {},
            .target     = {},
            .attempts   = {},
        } );
    }

}    // namespace grab
