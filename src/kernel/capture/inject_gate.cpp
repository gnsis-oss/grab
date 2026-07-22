#include "kernel/capture/inject_gate.hpp"

#include <memory>
#include <utility>

namespace grab::kernel
{

    InjectGate::Token::Token( std::shared_ptr<Lane> lane ) :
        lane_{ std::move( lane ) },
        lock_{ lane_->mutex }
    {
    }

    std::shared_ptr<InjectGate::Lane>
    InjectGate::lane_for( TargetId target )
    {
        const std::scoped_lock lock{ lanes_mutex_ };
        auto [iterator, inserted] = lanes_.try_emplace( target );
        if( inserted )
        {
            iterator->second = std::make_shared<Lane>();
        }
        return iterator->second;
    }

    InjectGate::CaptureToken
    InjectGate::acquire_capture( TargetId target )
    {
        return CaptureToken{ lane_for( target ) };
    }

    InjectGate::InjectionToken
    InjectGate::acquire_injection( TargetId target )
    {
        return InjectionToken{ lane_for( target ) };
    }

}    // namespace grab::kernel
