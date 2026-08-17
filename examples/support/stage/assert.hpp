#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ladder::view::stage
{

    // ── What a rung claims, and whether the claim held ───────────────────────
    //
    // One type, deliberately stringly-typed in expected_/actual_. A rung checks
    // colours, rects, a11y names, clipboard text, scroll offsets and durations;
    // a variant over all of those buys nothing, because the only consumer is a
    // human reading a scorecard and a diff comparing two runs.
    //
    // observer_ is not decoration. The design's second principle is that every
    // geometric claim is checked on two INDEPENDENT paths — pixels through the
    // fiducial fit, semantics through AT-SPI — so a scorecard has to record
    // which path produced each number. Two passes from the same observer are
    // one observation written down twice.

    struct Assertion
    {
            std::string observer_;    // "a11y" | "pixel" | "device" | "clock" | …
            std::string name_;        // "click_point_inside_rect"
            std::string expected_;    // "(412,268) in (380,240 240x90)"
            std::string actual_;      // "(412,268)"
            bool        pass_ = false;
    };

    // An ordered list of assertions plus the identity of the run that produced
    // them. Ordered because the order a rung checks things in is itself
    // diagnostic: the first failure usually explains the rest.
    class Scorecard
    {
        public:

            Scorecard() = default;

            explicit Scorecard( std::string rung ) :
                rung_( std::move( rung ) )
            {
            }

            void
            note( Assertion assertion )
            {
                if( !assertion.pass_ )
                {
                    ++failed_;
                }
                entries_.push_back( std::move( assertion ) );
            }

            // Convenience for the common shape: a named claim with an expected
            // and an actual, passing when they match as text.
            void
            note( std::string observer,
                  std::string name,
                  std::string expected,
                  std::string actual )
            {
                const bool pass = expected == actual;
                note( Assertion{
                    .observer_ = std::move( observer ),
                    .name_     = std::move( name ),
                    .expected_ = std::move( expected ),
                    .actual_   = std::move( actual ),
                    .pass_     = pass
                } );
            }

            [[nodiscard]]
            const std::vector<Assertion>&
            entries() const noexcept
            {
                return entries_;
            }

            [[nodiscard]]
            const std::string&
            rung() const noexcept
            {
                return rung_;
            }

            [[nodiscard]]
            std::size_t
            failed() const noexcept
            {
                return failed_;
            }

            [[nodiscard]]
            std::size_t
            passed() const noexcept
            {
                return entries_.size() - failed_;
            }

            // An EMPTY scorecard is a failure, not a pass.
            //
            // A rung that checked nothing has demonstrated nothing, and the
            // shape of bug this whole effort exists to kill is the one that
            // reports success without doing the work. "No assertions ran" and
            // "every assertion passed" must never print the same verdict.
            [[nodiscard]]
            bool
            pass() const noexcept
            {
                return !entries_.empty() && failed_ == 0U;
            }

        private:

            std::string            rung_;
            std::vector<Assertion> entries_;
            std::size_t            failed_ = 0U;
    };

}    // namespace ladder::view::stage
