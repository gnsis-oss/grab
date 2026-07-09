#pragma once

#include "core/provider.hpp"
#include "grab/capability.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace grab::test
{

    class FakeSessionProvider final : public grab::session::SessionProvider
    {
        public:

            explicit FakeSessionProvider( std::string name = "fake" ) :
                provider_info{
                    std::move( name ),
                    {},
                    quality,
                }
            {
            }

            [[nodiscard]]
            const grab::core::ProviderInfo&
            info() const noexcept override
            {
                return provider_info;
            }

            [[nodiscard]]
            grab::Availability
            probe( const grab::core::Environment& /*env*/,
                   grab::SessionMode mode ) const override
            {
                return mode_availability.at( mode_index( mode ) );
            }

            [[nodiscard]]
            grab::Result<grab::session::SessionRuntime>
            create( const grab::SessionDesc& desc ) const override
            {
                ++create_call_count;
                if( create_failure.has_value() )
                {
                    auto failure = std::move( *create_failure );
                    create_failure.reset();
                    return std::unexpected( std::move( failure ) );
                }

                return grab::session::SessionRuntime{
                    .endpoint       = std::string{ endpoint_prefix } + desc.name,
                    .control_socket = {},
                    .supervisor_pid = no_supervisor_pid,
                };
            }

            [[nodiscard]]
            grab::Result<void>
            destroy( const grab::session::SessionRuntime& /*runtime*/ ) const override
            {
                ++destroy_call_count;
                return {};
            }

            void
            fail_next_create( grab::ErrorCode code,
                              std::string     message )
            {
                create_failure = grab::Error{
                    .code       = code,
                    .message    = std::move( message ),
                    .capability = {},
                    .target     = {},
                    .attempts   = {},
                };
            }

            void
            set_availability( grab::SessionMode  mode,
                              grab::Availability new_availability )
            {
                mode_availability.at( mode_index( mode ) ) =
                    std::move( new_availability );
            }

            [[nodiscard]]
            std::size_t
            create_calls() const noexcept
            {
                return create_call_count;
            }

            [[nodiscard]]
            std::size_t
            destroy_calls() const noexcept
            {
                return destroy_call_count;
            }

        private:

            static constexpr int         quality = 0;
            static constexpr grab::Pid   no_supervisor_pid{};
            static constexpr const char* endpoint_prefix = ":fake-";
            static constexpr std::size_t mode_count =
                static_cast<std::size_t>( grab::SessionMode::count );

            [[nodiscard]]
            static constexpr std::size_t
            mode_index( grab::SessionMode mode ) noexcept
            {
                return static_cast<std::size_t>( mode );
            }

            grab::core::ProviderInfo                   provider_info;
            std::array<grab::Availability, mode_count> mode_availability{
                default_availability(),
                default_availability(),
            };
            mutable std::size_t                create_call_count  = 0U;
            mutable std::size_t                destroy_call_count = 0U;
            mutable std::optional<grab::Error> create_failure;

            [[nodiscard]]
            static constexpr grab::Availability
            default_availability()
            {
                return grab::Availability{
                    .state   = grab::AvailabilityState::available,
                    .reason  = {},
                    .quality = quality,
                };
            }
    };

}    // namespace grab::test
