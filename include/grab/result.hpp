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
        StaleShape            = 0X03'0D,
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

    struct ErrorDescriptor
    {
            ErrorCode        code;
            std::string_view name;
            ErrorCategory    category;
            ErrorDisposition default_disposition{ ErrorDisposition::Fatal };
            RetryClass       retry{ RetryClass::Never };
    };

    namespace detail
    {

        inline constexpr std::size_t errorCodeCount = 46U;

        [[nodiscard]]
        constexpr ErrorDescriptor
        error_descriptor( ErrorCode        code,
                          std::string_view name,
                          ErrorCategory    category,
                          ErrorDisposition default_disposition,
                          RetryClass       retry ) noexcept
        {
            return ErrorDescriptor{
                .code                = code,
                .name                = name,
                .category            = category,
                .default_disposition = default_disposition,
                .retry               = retry,
            };
        }

        inline constexpr auto errorDescriptors = std::to_array<ErrorDescriptor>( {
            error_descriptor( ErrorCode::EnvironmentChanged,
                              "environment_changed",
                              ErrorCategory::Environment,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::DisplayUnavailable,
                              "display_unavailable",
                              ErrorCategory::Environment,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::DeviceInaccessible,
                              "device_inaccessible",
                              ErrorCategory::Environment,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::TopologyChanged,
                              "topology_changed",
                              ErrorCategory::Environment,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::PermissionNeeded,
                              "permission_needed",
                              ErrorCategory::Permission,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PermissionDenied,
                              "permission_denied",
                              ErrorCategory::Permission,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::LeaseClosed,
                              "lease_closed",
                              ErrorCategory::Permission,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::LeaseRevoked,
                              "lease_revoked",
                              ErrorCategory::Permission,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::OwnershipRequired,
                              "ownership_required",
                              ErrorCategory::Permission,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::WindowNotFound,
                              "window_not_found",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::StaleWindow,
                              "stale_window",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::GeometryUntrusted,
                              "geometry_untrusted",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::StaleNode,
                              "stale_node",
                              ErrorCategory::Target,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::StaleShape,
                              "stale_shape",
                              ErrorCategory::Target,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::TreeResynced,
                              "tree_resynced",
                              ErrorCategory::Target,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::RuntimeRestarted,
                              "runtime_restarted",
                              ErrorCategory::Target,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::TargetDetached,
                              "target_detached",
                              ErrorCategory::Target,
                              ErrorDisposition::RetrySame,
                              RetryClass::ResolveOnly ),
            error_descriptor( ErrorCode::NoMatch,
                              "no_match",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::AmbiguousMatch,
                              "ambiguous_match",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PropertyAbsent,
                              "property_absent",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PropertyUnsupported,
                              "property_unsupported",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PropertyUncached,
                              "property_uncached",
                              ErrorCategory::Target,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PropertyBackendFailed,
                              "property_backend_failed",
                              ErrorCategory::Target,
                              ErrorDisposition::FallbackNext,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::CapabilityUnavailable,
                              "capability_unavailable",
                              ErrorCategory::Protocol,
                              ErrorDisposition::FallbackNext,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::ProviderFailed,
                              "provider_failed",
                              ErrorCategory::Protocol,
                              ErrorDisposition::FallbackNext,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::ProtocolError,
                              "protocol_error",
                              ErrorCategory::Protocol,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::InvalidArgument,
                              "invalid_argument",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::IllegalFromCallback,
                              "illegal_from_callback",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::UnsupportedCharacter,
                              "unsupported_character",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::SessionClosed,
                              "session_closed",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::SessionExists,
                              "session_exists",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::SessionNotFound,
                              "session_not_found",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::SessionDead,
                              "session_dead",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::DeadlineExceeded,
                              "deadline_exceeded",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::Cancelled,
                              "cancelled",
                              ErrorCategory::Usage,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::InternalFault,
                              "internal_fault",
                              ErrorCategory::InternalFault,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::NotActionable,
                              "not_actionable",
                              ErrorCategory::Action,
                              ErrorDisposition::FallbackNext,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::Occluded,
                              "occluded",
                              ErrorCategory::Action,
                              ErrorDisposition::FallbackNext,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::RouteUnavailable,
                              "route_unavailable",
                              ErrorCategory::Action,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::PossiblyCommitted,
                              "possibly_committed",
                              ErrorCategory::Action,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::VerificationFailed,
                              "verification_failed",
                              ErrorCategory::Action,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::NeutralizationFailed,
                              "neutralization_failed",
                              ErrorCategory::Action,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::QueueGap,
                              "queue_gap",
                              ErrorCategory::Stream,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::ResyncRequired,
                              "resync_required",
                              ErrorCategory::Stream,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::SubscriptionGone,
                              "subscription_gone",
                              ErrorCategory::Stream,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
            error_descriptor( ErrorCode::Overflowed,
                              "overflowed",
                              ErrorCategory::Stream,
                              ErrorDisposition::Fatal,
                              RetryClass::Never ),
        } );

        static_assert( errorDescriptors.size() == errorCodeCount );

        [[nodiscard]]
        consteval bool
        error_descriptors_are_unique() noexcept
        {
            for( const auto& descriptor : errorDescriptors )
            {
                if( descriptor.name.empty() )
                {
                    return false;
                }
                std::size_t code_matches = 0U;
                std::size_t name_matches = 0U;
                for( const auto& candidate : errorDescriptors )
                {
                    code_matches += candidate.code == descriptor.code ? 1U : 0U;
                    name_matches += candidate.name == descriptor.name ? 1U : 0U;
                }
                if( code_matches != 1U || name_matches != 1U )
                {
                    return false;
                }
            }
            return true;
        }

        static_assert( error_descriptors_are_unique() );

    }    // namespace detail

    [[nodiscard]]
    constexpr const std::array<ErrorDescriptor,
                               detail::errorCodeCount>&
    error_descriptors() noexcept
    {
        return detail::errorDescriptors;
    }

    [[nodiscard]]
    constexpr ErrorCategory
    category_of( ErrorCode code ) noexcept
    {
        for( const auto& descriptor : detail::errorDescriptors )
        {
            if( descriptor.code == code )
            {
                return descriptor.category;
            }
        }
        return ErrorCategory::InternalFault;
    }

    [[nodiscard]]
    constexpr std::string_view
    name_of( ErrorCode code ) noexcept
    {
        for( const auto& descriptor : detail::errorDescriptors )
        {
            if( descriptor.code == code )
            {
                return descriptor.name;
            }
        }
        return "internal_fault";
    }

    [[nodiscard]]
    constexpr ErrorDisposition
    default_disposition_of( ErrorCode code ) noexcept
    {
        for( const auto& descriptor : detail::errorDescriptors )
        {
            if( descriptor.code == code )
            {
                return descriptor.default_disposition;
            }
        }
        return ErrorDisposition::Fatal;
    }

    [[nodiscard]]
    constexpr RetryClass
    retry_class_of( ErrorCode code ) noexcept
    {
        for( const auto& descriptor : detail::errorDescriptors )
        {
            if( descriptor.code == code )
            {
                return descriptor.retry;
            }
        }
        return RetryClass::Never;
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
