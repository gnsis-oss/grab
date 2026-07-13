#include "core/id_factory.hpp"
#include "grab/ids.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <tag/gen.hpp>
#include <tag/rng.hpp>

namespace grab::detail
{

    namespace
    {

        constexpr std::size_t   timestampByteCount = 6U;
        constexpr std::size_t   bitsPerByte        = 8U;
        constexpr std::uint64_t byteMask           = 0XFFU;
        constexpr std::uint64_t timestampMaximum   = ( std::uint64_t{ 1U } << 48U ) - 1U;
        constexpr std::uint16_t counterMaximum     = 0X0F'FFU;
        constexpr std::size_t   versionByteIndex   = 6U;
        constexpr std::size_t   counterLowByteIndex = 7U;
        constexpr std::size_t   counterHighShift    = 8U;
        constexpr std::uint16_t counterHighMask     = 0X0FU;
        constexpr std::uint8_t  versionSevenBits    = 0X70U;

        [[nodiscard]]
        std::uint64_t
        system_milliseconds() noexcept
        {
            const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
            const auto count =
                std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count();
            if( count <= 0 )
            {
                return 0U;
            }
            return static_cast<std::uint64_t>( count );
        }

    }    // namespace

    class IdFactory::Impl
    {
        public:

            explicit Impl( Clock* clock ) :
                clock_{ clock == nullptr ? system_milliseconds : clock }
            {
            }

            [[nodiscard]]
            Uuid
            next_uuid()
            {
                const std::scoped_lock lock{ mutex_ };

                advance_state( clock_() );
                const auto generated = tag::timed( rng_ );

                Uuid       result;
                std::copy_n( generated.bytes(),
                             result.bytes.size(),
                             result.bytes.begin() );
                write_timestamp( result );
                write_counter( result );
                return result;
            }

            [[nodiscard]]
            FrameId
            next_frame_id() noexcept
            {
                const std::scoped_lock lock{ mutex_ };
                if( nextFrameId_ == std::numeric_limits<std::uint64_t>::max() )
                {
                    std::terminate();
                }
                const FrameId result{ .value = nextFrameId_ };
                ++nextFrameId_;
                return result;
            }

        private:

            void
            advance_state( std::uint64_t observedMilliseconds )
            {
                const auto currentMilliseconds = observedMilliseconds > timestampMaximum
                                                   ? timestampMaximum
                                                   : observedMilliseconds;

                if( !hasLast_ || currentMilliseconds > lastMilliseconds_ )
                {
                    lastMilliseconds_ = currentMilliseconds;
                    counter_          = 0U;
                    hasLast_          = true;
                    return;
                }

                if( counter_ < counterMaximum )
                {
                    ++counter_;
                    return;
                }

                if( lastMilliseconds_ == timestampMaximum )
                {
                    std::terminate();
                }

                ++lastMilliseconds_;
                counter_ = 0U;
            }

            void
            write_timestamp( Uuid& value ) const
            {
                for( std::size_t index = 0U; index < timestampByteCount; ++index )
                {
                    const auto shift = ( timestampByteCount - 1U - index ) * bitsPerByte;
                    value.bytes.at( index ) =
                        static_cast<std::uint8_t>( ( lastMilliseconds_ >> shift ) &
                                                   byteMask );
                }
            }

            void
            write_counter( Uuid& value ) const
            {
                const auto counterHigh =
                    static_cast<std::uint8_t>( ( counter_ >> counterHighShift ) &
                                               counterHighMask );
                value.bytes.at( versionByteIndex ) =
                    static_cast<std::uint8_t>( versionSevenBits | counterHigh );
                value.bytes.at( counterLowByteIndex ) =
                    static_cast<std::uint8_t>( counter_ & byteMask );
            }

            Clock*        clock_;
            tag::FastRng  rng_;
            std::mutex    mutex_;
            std::uint64_t lastMilliseconds_ = 0U;
            std::uint16_t counter_          = 0U;
            bool          hasLast_          = false;
            std::uint64_t nextFrameId_      = 1U;
    };

    namespace
    {

        IdFactory&
        process_factory()
        {
            // The free-function API requires one process-wide monotonic sequence.
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
            static IdFactory factory{ system_milliseconds };
            return factory;
        }

    }    // namespace

    IdFactory::IdFactory( Clock* clock ) :
        impl_{ std::make_unique<Impl>( clock ) }
    {
    }

    IdFactory::~IdFactory() = default;

    OperationId
    IdFactory::next_operation_id()
    {
        return OperationId{ .value = impl_->next_uuid() };
    }

    SubscriptionId
    IdFactory::next_subscription_id()
    {
        return SubscriptionId{ .value = impl_->next_uuid() };
    }

    FrameId
    IdFactory::next_frame_id() noexcept
    {
        return impl_->next_frame_id();
    }

    OperationId
    next_operation_id()
    {
        return process_factory().next_operation_id();
    }

    SubscriptionId
    next_subscription_id()
    {
        return process_factory().next_subscription_id();
    }

    FrameId
    next_frame_id() noexcept
    {
        return process_factory().next_frame_id();
    }

}    // namespace grab::detail
