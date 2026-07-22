#pragma once
// ┌──────────────────────────────────────────────────────────────┐
// │  tag/ns.h — RFC 9562 namespace constants, nil, and max       │
// └──────────────────────────────────────────────────────────────┘

#include <tag/tag.hpp>

namespace tag::ns
{

    // RFC 9562 §6.6, Table 3
    constexpr Id<128> dns{
        std::array<uint8_t, 16>{
                                0X6B, 0XA7,
                                0XB8, 0X10,
                                0X9D, 0XAD,
                                0X11, 0XD1,
                                0X80, 0XB4,
                                0X00, 0XC0,
                                0X4F, 0XD4,
                                0X30, 0XC8
        }
    };

    constexpr Id<128> url{
        std::array<uint8_t, 16>{
                                0X6B, 0XA7,
                                0XB8, 0X11,
                                0X9D, 0XAD,
                                0X11, 0XD1,
                                0X80, 0XB4,
                                0X00, 0XC0,
                                0X4F, 0XD4,
                                0X30, 0XC8
        }
    };

    constexpr Id<128> oid{
        std::array<uint8_t, 16>{
                                0X6B, 0XA7,
                                0XB8, 0X12,
                                0X9D, 0XAD,
                                0X11, 0XD1,
                                0X80, 0XB4,
                                0X00, 0XC0,
                                0X4F, 0XD4,
                                0X30, 0XC8
        }
    };

    constexpr Id<128> x500{
        std::array<uint8_t, 16>{
                                0X6B, 0XA7,
                                0XB8, 0X14,
                                0X9D, 0XAD,
                                0X11, 0XD1,
                                0X80, 0XB4,
                                0X00, 0XC0,
                                0X4F, 0XD4,
                                0X30, 0XC8
        }
    };

}    // namespace tag::ns

namespace tag
{

    template<unsigned N>
    constexpr Id<N>   nil{};

    constexpr Id<128> max{
        std::array<uint8_t, 16>{
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF,
                                0XFF, 0XFF
        }
    };

}    // namespace tag
