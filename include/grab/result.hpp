#pragma once

#include "grab/enum_table.hpp"
#include "grab/trace.hpp"

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
        Action,
        Stream,
    };

    // Numeric values are stable within a major version (spec section 8). The
    // high byte encodes the category.
    enum class ErrorCode : std::uint16_t
    {
        EnvironmentChanged    = 0X01'00,
        DisplayUnavailable    = 0X01'01,
        DeviceInaccessible    = 0X01'02,
        TopologyChanged       = 0X01'03,

        PermissionNeeded      = 0X02'00,
        PermissionDenied      = 0X02'01,
        LeaseClosed           = 0X02'02,
        LeaseRevoked          = 0X02'03,
        OwnershipRequired     = 0X02'04,

        WindowNotFound        = 0X03'00,
        StaleWindow           = 0X03'01,
        GeometryUntrusted     = 0X03'02,
        StaleNode             = 0X03'03,
        TreeResynced          = 0X03'04,
        RuntimeRestarted      = 0X03'05,
        TargetDetached        = 0X03'06,
        NoMatch               = 0X03'07,
        AmbiguousMatch        = 0X03'08,
        PropertyAbsent        = 0X03'09,
        PropertyUnsupported   = 0X03'0A,
        PropertyUncached      = 0X03'0B,
        PropertyBackendFailed = 0X03'0C,

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
        DeadlineExceeded      = 0X05'07,
        Cancelled             = 0X05'08,

        InternalFault         = 0X06'00,

        NotActionable         = 0X07'00,
        Occluded              = 0X07'01,
        RouteUnavailable      = 0X07'02,
        PossiblyCommitted     = 0X07'03,
        VerificationFailed    = 0X07'04,
        NeutralizationFailed  = 0X07'05,

        QueueGap              = 0X08'00,
        ResyncRequired        = 0X08'01,
        SubscriptionGone      = 0X08'02,
        Overflowed            = 0X08'03,
    };

    namespace detail
    {

        inline constexpr std::size_t errorCodeCount = 45U;

        inline constexpr auto        errorCodeNames = EnumTable{
            std::to_array( {
                enum_entry( ErrorCode::EnvironmentChanged, "environment_changed" ),
                enum_entry( ErrorCode::DisplayUnavailable, "display_unavailable" ),
                enum_entry( ErrorCode::DeviceInaccessible, "device_inaccessible" ),
                enum_entry( ErrorCode::TopologyChanged, "topology_changed" ),
                enum_entry( ErrorCode::PermissionNeeded, "permission_needed" ),
                enum_entry( ErrorCode::PermissionDenied, "permission_denied" ),
                enum_entry( ErrorCode::LeaseClosed, "lease_closed" ),
                enum_entry( ErrorCode::LeaseRevoked, "lease_revoked" ),
                enum_entry( ErrorCode::OwnershipRequired, "ownership_required" ),
                enum_entry( ErrorCode::WindowNotFound, "window_not_found" ),
                enum_entry( ErrorCode::StaleWindow, "stale_window" ),
                enum_entry( ErrorCode::GeometryUntrusted, "geometry_untrusted" ),
                enum_entry( ErrorCode::StaleNode, "stale_node" ),
                enum_entry( ErrorCode::TreeResynced, "tree_resynced" ),
                enum_entry( ErrorCode::RuntimeRestarted, "runtime_restarted" ),
                enum_entry( ErrorCode::TargetDetached, "target_detached" ),
                enum_entry( ErrorCode::NoMatch, "no_match" ),
                enum_entry( ErrorCode::AmbiguousMatch, "ambiguous_match" ),
                enum_entry( ErrorCode::PropertyAbsent, "property_absent" ),
                enum_entry( ErrorCode::PropertyUnsupported, "property_unsupported" ),
                enum_entry( ErrorCode::PropertyUncached, "property_uncached" ),
                enum_entry( ErrorCode::PropertyBackendFailed,
                            "property_backend_failed" ),
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
                enum_entry( ErrorCode::DeadlineExceeded, "deadline_exceeded" ),
                enum_entry( ErrorCode::Cancelled, "cancelled" ),
                enum_entry( ErrorCode::InternalFault, "internal_fault" ),
                enum_entry( ErrorCode::NotActionable, "not_actionable" ),
                enum_entry( ErrorCode::Occluded, "occluded" ),
                enum_entry( ErrorCode::RouteUnavailable, "route_unavailable" ),
                enum_entry( ErrorCode::PossiblyCommitted, "possibly_committed" ),
                enum_entry( ErrorCode::VerificationFailed, "verification_failed" ),
                enum_entry( ErrorCode::NeutralizationFailed, "neutralization_failed" ),
                enum_entry( ErrorCode::QueueGap, "queue_gap" ),
                enum_entry( ErrorCode::ResyncRequired, "resync_required" ),
                enum_entry( ErrorCode::SubscriptionGone, "subscription_gone" ),
                enum_entry( ErrorCode::Overflowed, "overflowed" ),
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
        constexpr std::uint16_t actionCategoryTag      = 0X07U;
        constexpr std::uint16_t streamCategoryTag      = 0X08U;

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
            case actionCategoryTag :
                return ErrorCategory::Action;
            case streamCategoryTag :
                return ErrorCategory::Stream;
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
            ErrorDisposition             disposition{ ErrorDisposition::Fatal };
            std::vector<DiagnosticEntry>
                diagnostics{};    // NOLINT(readability-redundant-member-init)
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
