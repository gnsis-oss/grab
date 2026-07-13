#pragma once
// ┌───────────────────────────────────────────────────────────────────────┐
// │  tag/peer.hpp — tag::Peer logical runtime-entity handle               │
// └───────────────────────────────────────────────────────────────────────┘
//
// A Peer names an abstract runtime entity — not a network address.
// It consists of two UUIDs (site + host), a 16-bit sub-locale index,
// and 16 bits of flags.
//
// Designed to replace com::locale_id; used by crdt (vclock replica id),
// tick (HybridStamp), link (routing), truce (replica identity) and
// yoke (worker addressing). See Chapel chpl_localeID_t, HPX gid_type,
// CAF node_id for prior art.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <out/put.hpp>
#include <out/traits.hpp>
#include <span>
#include <tag/tag.hpp>

namespace tag
{

    // ── Uuid alias ─────────────────────────────────────────────────────────────

    inline constexpr unsigned uuid_bits = 128;
    using Uuid                          = Id<uuid_bits>;

    // ── Peer ───────────────────────────────────────────────────────────────────

    // Field names follow the ticket's public API (see tag workspace
    // peer_and_contentid_ticket.md). NOLINT silences the project-wide
    // MemberSuffix rule for this documented vocabulary struct.

    struct Peer
    {
            // NOLINTBEGIN(readability-identifier-naming)
            Uuid     site;
            Uuid     host;
            uint16_t subloc{};
            uint16_t flags{};
            // NOLINTEND(readability-identifier-naming)

            [[nodiscard]]
            constexpr bool
            operator==( const Peer& ) const noexcept = default;

            [[nodiscard]]
            constexpr auto
            operator<=>( const Peer& ) const noexcept = default;

            [[nodiscard]]
            constexpr bool
            nil() const noexcept
            {
                return site.nil() && host.nil() && subloc == 0 && flags == 0;
            }
    };

    // Wire layout: site(16) + host(16) + subloc(2) + flags(2) = 36 bytes.
    inline constexpr std::size_t uuid_bytes     = 16;
    inline constexpr std::size_t peer_wire_size = 36;

    namespace detail
    {

        inline constexpr std::size_t peer_off_site   = 0;
        inline constexpr std::size_t peer_off_host   = 16;
        inline constexpr std::size_t peer_off_subloc = 32;
        inline constexpr std::size_t peer_off_flags  = 34;
        inline constexpr unsigned    byte_shift      = 8;
        inline constexpr uint16_t    byte_mask       = 0XFFU;

    }    // namespace detail

    // ── encode / decode ────────────────────────────────────────────────────────

    [[nodiscard]]
    inline std::array<std::byte,
                      peer_wire_size>
    encode( const Peer& p ) noexcept
    {
        std::array<std::byte, peer_wire_size> out{};

        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::memcpy( out.data() + detail::peer_off_site, p.site.bytes(), uuid_bytes );
        std::memcpy( out.data() + detail::peer_off_host, p.host.bytes(), uuid_bytes );
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

        const auto sub_hi = static_cast<unsigned>( p.subloc >> detail::byte_shift );
        const auto sub_lo = static_cast<unsigned>( p.subloc );
        const auto flg_hi = static_cast<unsigned>( p.flags >> detail::byte_shift );
        const auto flg_lo = static_cast<unsigned>( p.flags );

        out.at( detail::peer_off_subloc ) =
            static_cast<std::byte>( sub_hi & detail::byte_mask );
        out.at( detail::peer_off_subloc + 1 ) =
            static_cast<std::byte>( sub_lo & detail::byte_mask );
        out.at( detail::peer_off_flags ) =
            static_cast<std::byte>( flg_hi & detail::byte_mask );
        out.at( detail::peer_off_flags + 1 ) =
            static_cast<std::byte>( flg_lo & detail::byte_mask );

        return out;
    }

    [[nodiscard]]
    inline out::Put<Peer,
                    out::Error>
    decode( std::span<const std::byte,
                      peer_wire_size> buf ) noexcept
    {
        Peer                            p{};

        std::array<uint8_t, uuid_bytes> site_raw{};
        std::array<uint8_t, uuid_bytes> host_raw{};

        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        std::memcpy( site_raw.data(), buf.data() + detail::peer_off_site, uuid_bytes );
        std::memcpy( host_raw.data(), buf.data() + detail::peer_off_host, uuid_bytes );
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

        p.site            = Uuid{ site_raw };
        p.host            = Uuid{ host_raw };

        const auto sub_hi = std::to_integer<uint8_t>( buf[detail::peer_off_subloc] );
        const auto sub_lo = std::to_integer<uint8_t>( buf[detail::peer_off_subloc + 1] );
        const auto flg_hi = std::to_integer<uint8_t>( buf[detail::peer_off_flags] );
        const auto flg_lo = std::to_integer<uint8_t>( buf[detail::peer_off_flags + 1] );

        p.subloc          = static_cast<uint16_t>( ( static_cast<unsigned>( sub_hi )
                                                     << detail::byte_shift ) |
                                                   static_cast<unsigned>( sub_lo ) );
        p.flags           = static_cast<uint16_t>( ( static_cast<unsigned>( flg_hi )
                                                     << detail::byte_shift ) |
                                                   static_cast<unsigned>( flg_lo ) );

        return p;
    }

    // ── Nil sentinel ───────────────────────────────────────────────────────────

    inline constexpr Peer nil_peer{};

}    // namespace tag

// ── std::hash ──────────────────────────────────────────────────────────────

template<>
struct std::hash<tag::Peer>
{
        [[nodiscard]]
        std::size_t
        operator()( const tag::Peer& p ) const noexcept
        {
            // hash_combine-style mixing (boost-like).  The magic constant
            // 0x9E3779B97F4A7C15 is 2^64 / golden-ratio — a well-known
            // bit-dispersion seed that avoids clustering.
            auto mix = []( std::size_t seed, std::size_t v ) noexcept
            {
                constexpr std::size_t golden  = 0X9E'37'79'B9'7F'4A'7C'15ULL;
                constexpr unsigned    l_shift = 6;
                constexpr unsigned    r_shift = 2;
                seed ^= v + golden + ( seed << l_shift ) + ( seed >> r_shift );
                return seed;
            };

            std::size_t h = std::hash<tag::Uuid>{}( p.site );
            h             = mix( h, std::hash<tag::Uuid>{}( p.host ) );
            h             = mix( h, std::hash<uint16_t>{}( p.subloc ) );
            h             = mix( h, std::hash<uint16_t>{}( p.flags ) );
            return h;
        }
};
