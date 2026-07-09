#pragma once

#include "grab/image.hpp"
#include "grab/result.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace grab::codec
{

    [[nodiscard]]
    grab::Result<std::vector<std::byte>>
    encode_png( const grab::Image& image );

    [[nodiscard]]
    grab::Result<grab::Image>
    decode_png( std::span<const std::byte> bytes );

}    // namespace grab::codec
