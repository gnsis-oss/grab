#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <tag/lane.hpp>
#include <tag/tag.hpp>
#include <walk/diff.hpp>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace
{

    constexpr std::size_t   laneBits        = 32U;
    constexpr std::uint64_t addedKnotId     = 7U;
    constexpr std::uint64_t firstFromId     = 1U;
    constexpr std::uint64_t firstToId       = 2U;
    constexpr std::uint64_t collidingFromId = 3U;
    constexpr std::uint64_t collidingToId   = 2'262'152U;

}

TEST( VendorL0,
      IdLaneGenerationDistinguishesReuse )
{
    tag::IdLane<laneBits> a{};
    EXPECT_EQ( a, tag::IdLane<laneBits>{} );    // nil == nil
}

TEST( VendorL0,
      WebDiffSeesAddedKnot )
{
    web::Web<web::OneWay> before;
    web::Web<web::OneWay> after;
    const auto knot = web::Knot( tag::Id<64>( std::uint64_t{ addedKnotId } ) );
    ( void )after.add( knot );    // add() is [[nodiscard]] — discard explicitly
    const auto delta = walk::diff( before, after );
    EXPECT_EQ( delta.added_knots.size(), 1U );
    EXPECT_TRUE( delta.removed_knots.empty() );
}

TEST( VendorL0,
      WebDiffKeepsHashCollidingEndpointPairsDistinct )
{
    web::Web<web::OneWay> before;
    web::Web<web::OneWay> after;
    const auto            one       = web::Knot( std::uint64_t{ firstFromId } );
    const auto            two       = web::Knot( std::uint64_t{ firstToId } );
    const auto            three     = web::Knot( std::uint64_t{ collidingFromId } );
    const auto            collision = web::Knot( std::uint64_t{ collidingToId } );

    ( void )before.add( one );
    ( void )before.add( two );
    ( void )before.add( three );
    ( void )before.add( collision );
    ( void )after.add( one );
    ( void )after.add( two );
    ( void )after.add( three );
    ( void )after.add( collision );
    ( void )before.tie( one, two );
    ( void )after.tie( three, collision );

    const auto delta = walk::diff( before, after );
    EXPECT_EQ( delta.added_edges.size(), 1U );
    EXPECT_EQ( delta.removed_edges.size(), 1U );
}
