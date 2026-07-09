#ifndef GRAB_CODEC_PNG_HPP
#define GRAB_CODEC_PNG_HPP

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

#endif    // GRAB_CODEC_PNG_HPP
