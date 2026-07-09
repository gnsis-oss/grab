#ifndef GRAB_RESULT_HPP
#define GRAB_RESULT_HPP

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
        environment,
        permission,
        target,
        protocol,
        usage,
        internal_fault,
    };

    // Numeric values are stable within a major version (spec section 8). The
    // high byte encodes the category.
    enum class ErrorCode : std::uint16_t
    {
        environment_changed    = 0X01'00,
        display_unavailable    = 0X01'01,
        device_inaccessible    = 0X01'02,

        permission_needed      = 0X02'00,
        permission_denied      = 0X02'01,

        window_not_found       = 0X03'00,
        stale_window           = 0X03'01,
        geometry_untrusted     = 0X03'02,

        capability_unavailable = 0X04'00,
        provider_failed        = 0X04'01,
        protocol_error         = 0X04'02,

        invalid_argument       = 0X05'00,
        illegal_from_callback  = 0X05'01,
        unsupported_character  = 0X05'02,
        session_closed         = 0X05'03,
        session_exists         = 0X05'04,
        session_not_found      = 0X05'05,
        session_dead           = 0X05'06,

        internal_fault         = 0X06'00,
    };

    namespace detail
    {

        inline constexpr std::size_t kErrorCodeCount = 19U;

        inline constexpr auto        kErrorCodeNames = EnumTable{
            std::to_array( {
                enum_entry( ErrorCode::environment_changed, "environment_changed" ),
                enum_entry( ErrorCode::display_unavailable, "display_unavailable" ),
                enum_entry( ErrorCode::device_inaccessible, "device_inaccessible" ),
                enum_entry( ErrorCode::permission_needed, "permission_needed" ),
                enum_entry( ErrorCode::permission_denied, "permission_denied" ),
                enum_entry( ErrorCode::window_not_found, "window_not_found" ),
                enum_entry( ErrorCode::stale_window, "stale_window" ),
                enum_entry( ErrorCode::geometry_untrusted, "geometry_untrusted" ),
                enum_entry( ErrorCode::capability_unavailable,
                            "capability_unavailable" ),
                enum_entry( ErrorCode::provider_failed, "provider_failed" ),
                enum_entry( ErrorCode::protocol_error, "protocol_error" ),
                enum_entry( ErrorCode::invalid_argument, "invalid_argument" ),
                enum_entry( ErrorCode::illegal_from_callback, "illegal_from_callback" ),
                enum_entry( ErrorCode::unsupported_character, "unsupported_character" ),
                enum_entry( ErrorCode::session_closed, "session_closed" ),
                enum_entry( ErrorCode::session_exists, "session_exists" ),
                enum_entry( ErrorCode::session_not_found, "session_not_found" ),
                enum_entry( ErrorCode::session_dead, "session_dead" ),
                enum_entry( ErrorCode::internal_fault, "internal_fault" ),
            } ),
        };
        static_assert( enum_table_has_count( kErrorCodeNames,
                                             kErrorCodeCount ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr ErrorCategory
    category_of( ErrorCode code ) noexcept
    {
        constexpr std::uint16_t kCategoryShift          = 8U;
        constexpr std::uint16_t kEnvironmentCategoryTag = 0X01U;
        constexpr std::uint16_t kPermissionCategoryTag  = 0X02U;
        constexpr std::uint16_t kTargetCategoryTag      = 0X03U;
        constexpr std::uint16_t kProtocolCategoryTag    = 0X04U;
        constexpr std::uint16_t kUsageCategoryTag       = 0X05U;

        switch( static_cast<std::uint16_t>( code ) >> kCategoryShift )
        {
            case kEnvironmentCategoryTag :
                return ErrorCategory::environment;
            case kPermissionCategoryTag :
                return ErrorCategory::permission;
            case kTargetCategoryTag :
                return ErrorCategory::target;
            case kProtocolCategoryTag :
                return ErrorCategory::protocol;
            case kUsageCategoryTag :
                return ErrorCategory::usage;
            default :
                return ErrorCategory::internal_fault;
        }
    }

    [[nodiscard]]
    constexpr std::string_view
    name_of( ErrorCode code ) noexcept
    {
        return detail::kErrorCodeNames.text_of( code, "internal_fault" );
    }

    struct ProviderAttempt
    {
            std::string provider;
            std::string reason;
    };

    struct Error
    {
            ErrorCode   code = ErrorCode::internal_fault;
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

#endif
