#ifndef CORE_MONITOR_HPP
#define CORE_MONITOR_HPP

#include "core/environment.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

namespace grab::core
{

    class EnvironmentMonitor
    {
        public:

            using Listener = std::function<void( const Environment& )>;

            explicit EnvironmentMonitor( Environment initial );

            [[nodiscard]]
            Environment
            current() const;

            std::uint64_t
            update( Environment next );

            [[nodiscard]]
            std::uint64_t
            subscribe( Listener listener );

            // Prevents future notifications, but a callback already snapshotted by
            // an in-flight update() may still run once after unsubscribe() returns.
            void
            unsubscribe( std::uint64_t id );

        private:

            mutable std::mutex                mutex_;
            Environment                       environment_;
            std::map<std::uint64_t, Listener> listeners_;
            std::uint64_t                     next_listener_id_ = 1;
    };

}    // namespace grab::core

#endif    // CORE_MONITOR_HPP
