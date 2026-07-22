#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include <array>
#include <compare>
#include <cstdint>
#include <string>

namespace grab
{

    // 16-byte RFC 9562 UUID value (v7 for operations: time-ordered in JSONL).
    struct Uuid
    {
            std::array<std::uint8_t, 16>
                bytes{};    // NOLINT(cppcoreguidelines-avoid-magic-numbers)
            [[nodiscard]]
            bool
            is_nil() const;
            [[nodiscard]]
            std::string
            to_string() const;    // 8-4-4-4-12 lowercase
            friend auto
            operator<=>( const Uuid&,
                         const Uuid& ) = default;
    };

    struct OperationId
    {
            Uuid value{};
            friend auto
            operator<=>( const OperationId&,
                         const OperationId& ) = default;
    };

    struct SubscriptionId
    {
            Uuid value{};
            friend auto
            operator<=>( const SubscriptionId&,
                         const SubscriptionId& ) = default;
    };

    // Identifies one attached runtime instance; bumps on runtime restart.
    struct RuntimeId
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const RuntimeId&,
                         const RuntimeId& ) = default;
    };

    // Monotonic generation counters (bumped on restart/resync; never reused
    // within a session). Wrappers, not raw ints, so domains cannot mix.
    struct DisplayGeneration
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const DisplayGeneration&,
                         const DisplayGeneration& ) = default;
    };

    struct TreeEpoch
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const TreeEpoch&,
                         const TreeEpoch& ) = default;
    };

    struct NodeGeneration
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const NodeGeneration&,
                         const NodeGeneration& ) = default;
    };

    struct CaptureGeneration
    {
            std::uint32_t value{};
            friend auto
            operator<=>( const CaptureGeneration&,
                         const CaptureGeneration& ) = default;
    };

    struct WindowRef
    {
            DisplayGeneration display_generation{};
            std::uint32_t
                xid{};    // opaque native window token (canonical plan R4);
                          // meaning is driver-internal, callers never interpret it
            friend auto
            operator<=>( const WindowRef&,
                         const WindowRef& ) = default;
    };

    struct WidgetRef    // runtime-scoped per canonical NodeRef contract
    {
            RuntimeId      runtime{};
            std::uint32_t  tree{};
            TreeEpoch      epoch{};
            std::uint64_t  node{};
            NodeGeneration generation{};
            friend auto
            operator<=>( const WidgetRef&,
                         const WidgetRef& ) = default;
    };

    struct FrameId
    {
            std::uint64_t value{};
            friend auto
            operator<=>( const FrameId&,
                         const FrameId& ) = default;
    };

    struct FrameRef    // canonical plan R4: capture evidence identity
    {
            CaptureGeneration capture_generation{};
            FrameId           frame{};
            // the frame's coordinate space travels in Frame metadata (§3.11);
            // FrameRef alone is the storable/wire identity
            friend auto
            operator<=>( const FrameRef&,
                         const FrameRef& ) = default;
    };

    inline bool
    Uuid::is_nil() const
    {
        return bytes == decltype( bytes ){};
    }

    inline std::string
    Uuid::to_string() const
    {
        constexpr std::array<char, 16> hexDigits{
            '0',
            '1',
            '2',
            '3',
            '4',
            '5',
            '6',
            '7',
            '8',
            '9',
            'a',
            'b',
            'c',
            'd',
            'e',
            'f',
        };
        constexpr std::size_t  firstHyphenOffset  = 4U;
        constexpr std::size_t  secondHyphenOffset = 6U;
        constexpr std::size_t  thirdHyphenOffset  = 8U;
        constexpr std::size_t  fourthHyphenOffset = 10U;
        constexpr std::size_t  highNibbleShift    = 4U;
        constexpr std::uint8_t lowNibbleMask      = 0X0FU;
        constexpr std::size_t  hyphenCount        = 4U;

        std::string            result;
        result.reserve( ( bytes.size() * 2U ) + hyphenCount );

        std::size_t byteIndex = 0U;
        for( const auto byte : bytes )
        {
            if( byteIndex ==
                firstHyphenOffset ||
                byteIndex ==
                secondHyphenOffset ||
                byteIndex ==
                thirdHyphenOffset ||
                byteIndex == fourthHyphenOffset )
            {
                result.push_back( '-' );
            }

            const auto highNibble = static_cast<std::size_t>( byte >> highNibbleShift );
            const auto lowNibble  = static_cast<std::size_t>( byte & lowNibbleMask );
            result.push_back( hexDigits.at( highNibble ) );
            result.push_back( hexDigits.at( lowNibble ) );
            ++byteIndex;
        }

        return result;
    }

}    // namespace grab
