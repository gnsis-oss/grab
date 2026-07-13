#include "core/id_factory.hpp"
#include "grab/context.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace grab
{

    Deadline
    Deadline::after( std::chrono::nanoseconds budget )
    {
        return Deadline{ .at = std::chrono::steady_clock::now() + budget };
    }

    Deadline
    Deadline::unbounded()
    {
        return {};
    }

    std::chrono::nanoseconds
    Deadline::remaining() const
    {
        const auto now = std::chrono::steady_clock::now();
        if( now >= at )
        {
            return std::chrono::nanoseconds::zero();
        }

        return std::chrono::duration_cast<std::chrono::nanoseconds>( at - now );
    }

    bool
    Deadline::expired() const
    {
        return std::chrono::steady_clock::now() >= at;
    }

    DiagnosticLog::DiagnosticLog( std::size_t capacity ) :
        capacity_{ capacity }
    {
        ring_.reserve( capacity_ );
    }

    void
    DiagnosticLog::note( std::string message )
    {
        DiagnosticEntry entry{
            .at      = std::chrono::steady_clock::now(),
            .message = std::move( message ),
        };

        if( capacity_ == 0U )
        {
            ++dropped_;
            return;
        }

        if( ring_.size() < capacity_ )
        {
            ring_.push_back( std::move( entry ) );
            next_ = ring_.size() % capacity_;
            return;
        }

        ring_.at( next_ ) = std::move( entry );
        next_             = ( next_ + 1U ) % capacity_;
        ++dropped_;
    }

    std::vector<DiagnosticEntry>
    DiagnosticLog::snapshot() const
    {
        if( ring_.size() < capacity_ )
        {
            return ring_;
        }

        std::vector<DiagnosticEntry> entries;
        entries.reserve( ring_.size() );
        for( std::size_t offset = 0U; offset < ring_.size(); ++offset )
        {
            const auto index = ( next_ + offset ) % capacity_;
            entries.push_back( ring_.at( index ) );
        }
        return entries;
    }

    std::size_t
    DiagnosticLog::dropped() const
    {
        return dropped_;
    }

    Result<void>
    OperationContext::check() const
    {
        if( stop.stop_requested() )
        {
            return fail( ErrorCode::Cancelled, "operation cancelled" );
        }

        if( deadline.expired() )
        {
            return fail( ErrorCode::DeadlineExceeded, "operation deadline exceeded" );
        }

        return {};
    }

    void
    OperationContext::note( std::string message ) const
    {
        if( log != nullptr )
        {
            log->note( std::move( message ) );
        }
    }

    OperationContext
    OperationContext::nested() const
    {
        return OperationContext{
            .deadline      = deadline,
            .stop          = stop,
            .operation     = detail::next_operation_id(),
            .causal_parent = operation,
            .log           = log,
        };
    }

}    // namespace grab
