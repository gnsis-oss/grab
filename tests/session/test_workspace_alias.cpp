#include "grab/session.hpp"
#include "grab/workspace.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <type_traits>
// clang-format on

static_assert( std::is_class_v<grab::Workspace> );

#if defined( __clang__ ) || defined( __GNUC__ )
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

static_assert( std::is_same_v<grab::SessionDesc,
                              grab::WorkspaceDesc> );
static_assert( std::is_same_v<grab::SessionMode,
                              grab::WorkspaceMode> );
static_assert( std::is_same_v<grab::SessionState,
                              grab::WorkspaceState> );
static_assert( std::is_same_v<grab::SessionGeometry,
                              grab::WorkspaceGeometry> );

#if defined( __clang__ ) || defined( __GNUC__ )
    #pragma GCC diagnostic pop
#endif

TEST( WorkspaceAliases,
      DeprecatedSessionNamesReferToWorkspaceTypes )
{
    SUCCEED();
}
