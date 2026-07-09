#include "grab/result.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>

namespace
{

    constexpr auto kEnvironmentChangedCode    = grab::ErrorCode::environment_changed;
    constexpr auto kEnvironmentCategory       = grab::ErrorCategory::environment;
    constexpr auto kPermissionDeniedCode      = grab::ErrorCode::permission_denied;
    constexpr auto kPermissionCategory        = grab::ErrorCategory::permission;
    constexpr auto kStaleWindowCode           = grab::ErrorCode::stale_window;
    constexpr auto kTargetCategory            = grab::ErrorCategory::target;
    constexpr auto kCapabilityUnavailableCode = grab::ErrorCode::capability_unavailable;
    constexpr auto kProtocolCategory          = grab::ErrorCategory::protocol;
    constexpr auto kIllegalFromCallbackCode   = grab::ErrorCode::illegal_from_callback;
    constexpr auto kUsageCategory             = grab::ErrorCategory::usage;
    constexpr auto kInternalFaultCode         = grab::ErrorCode::internal_fault;
    constexpr auto kInternalFaultCategory     = grab::ErrorCategory::internal_fault;
    constexpr auto kUnsupportedCharacterCode  = grab::ErrorCode::unsupported_character;
    constexpr std::string_view kStaleWindowName          = "stale_window";
    constexpr std::string_view kUnsupportedCharacterName = "unsupported_character";
    constexpr int              kAnswer                   = 42;
    constexpr auto             kWindowNotFoundCode   = grab::ErrorCode::window_not_found;
    constexpr std::string_view kMissingWindowMessage = "no such window";

}    // namespace

TEST( ErrorModel,
      CodesMapToTheirCategory )
{
    EXPECT_EQ( grab::category_of( kEnvironmentChangedCode ), kEnvironmentCategory );
    EXPECT_EQ( grab::category_of( kPermissionDeniedCode ), kPermissionCategory );
    EXPECT_EQ( grab::category_of( kStaleWindowCode ), kTargetCategory );
    EXPECT_EQ( grab::category_of( kCapabilityUnavailableCode ), kProtocolCategory );
    EXPECT_EQ( grab::category_of( kIllegalFromCallbackCode ), kUsageCategory );
    EXPECT_EQ( grab::category_of( kInternalFaultCode ), kInternalFaultCategory );
}

TEST( ErrorModel,
      NamesAreStableStrings )
{
    EXPECT_EQ( grab::name_of( kStaleWindowCode ), kStaleWindowName );
    EXPECT_EQ( grab::name_of( kUnsupportedCharacterCode ), kUnsupportedCharacterName );
}

TEST( ErrorModel,
      ResultCarriesValueOrError )
{
    grab::Result<int> good = kAnswer;
    ASSERT_TRUE( good.has_value() );
    EXPECT_EQ( *good, kAnswer );

    grab::Result<int> bad =
        grab::fail( kWindowNotFoundCode, std::string{ kMissingWindowMessage } );
    ASSERT_FALSE( bad.has_value() );
    EXPECT_EQ( bad.error().code, kWindowNotFoundCode );
    EXPECT_EQ( bad.error().message, std::string{ kMissingWindowMessage } );
}
