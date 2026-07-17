#include "grab/result.hpp"

#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace
{

    constexpr auto environmentChangedCode      = grab::ErrorCode::EnvironmentChanged;
    constexpr auto environmentCategory         = grab::ErrorCategory::Environment;
    constexpr auto permissionDeniedCode        = grab::ErrorCode::PermissionDenied;
    constexpr auto permissionCategory          = grab::ErrorCategory::Permission;
    constexpr auto staleWindowCode             = grab::ErrorCode::StaleWindow;
    constexpr auto targetCategory              = grab::ErrorCategory::Target;
    constexpr auto capabilityUnavailableCode   = grab::ErrorCode::CapabilityUnavailable;
    constexpr auto protocolCategory            = grab::ErrorCategory::Protocol;
    constexpr auto illegalFromCallbackCode     = grab::ErrorCode::IllegalFromCallback;
    constexpr auto usageCategory               = grab::ErrorCategory::Usage;
    constexpr auto internalFaultCode           = grab::ErrorCode::InternalFault;
    constexpr auto internalFaultCategory       = grab::ErrorCategory::InternalFault;
    constexpr auto unsupportedCharacterCode    = grab::ErrorCode::UnsupportedCharacter;
    constexpr std::string_view staleWindowName = "stale_window";
    constexpr std::string_view unsupportedCharacterName = "unsupported_character";
    constexpr int              answer                   = 42;
    constexpr auto             windowNotFoundCode   = grab::ErrorCode::WindowNotFound;
    constexpr std::string_view missingWindowMessage = "no such window";
    constexpr std::size_t      errorDescriptorCount = 46U;

}    // namespace

TEST( ErrorModel,
      CodesMapToTheirCategory )
{
    EXPECT_EQ( grab::category_of( environmentChangedCode ), environmentCategory );
    EXPECT_EQ( grab::category_of( permissionDeniedCode ), permissionCategory );
    EXPECT_EQ( grab::category_of( staleWindowCode ), targetCategory );
    EXPECT_EQ( grab::category_of( capabilityUnavailableCode ), protocolCategory );
    EXPECT_EQ( grab::category_of( illegalFromCallbackCode ), usageCategory );
    EXPECT_EQ( grab::category_of( internalFaultCode ), internalFaultCategory );
}

TEST( ErrorModel,
      NamesAreStableStrings )
{
    EXPECT_EQ( grab::name_of( staleWindowCode ), staleWindowName );
    EXPECT_EQ( grab::name_of( unsupportedCharacterCode ), unsupportedCharacterName );
}

TEST( ErrorModel,
      DescriptorRegistryIsCompleteAndDrivesAccessors )
{
    const auto& descriptors = grab::error_descriptors();
    ASSERT_EQ( descriptors.size(), errorDescriptorCount );

    for( const auto& descriptor : descriptors )
    {
        EXPECT_EQ( grab::name_of( descriptor.code ), descriptor.name );
        EXPECT_EQ( grab::category_of( descriptor.code ), descriptor.category );
        EXPECT_EQ( grab::default_disposition_of( descriptor.code ),
                   descriptor.default_disposition );
        EXPECT_EQ( grab::retry_class_of( descriptor.code ), descriptor.retry );
    }
}

TEST( ErrorModel,
      ResultCarriesValueOrError )
{
    grab::Result<int> good = answer;
    ASSERT_TRUE( good.has_value() );
    EXPECT_EQ( *good, answer );

    grab::Result<int> bad =
        grab::fail( windowNotFoundCode, std::string{ missingWindowMessage } );
    ASSERT_FALSE( bad.has_value() );
    EXPECT_EQ( bad.error().code, windowNotFoundCode );
    EXPECT_EQ( bad.error().message, std::string{ missingWindowMessage } );
}
