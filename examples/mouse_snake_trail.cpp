#include "grab/result.hpp"
#include "grab/session.hpp"

#include <iostream>
#include <utility>

namespace
{

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }
        ( *session )->close();
        return {};
    }

}    // namespace

int
main()
{
    auto result = run();
    if( !result.has_value() )
    {
        std::cerr << "mouse_snake_trail: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
