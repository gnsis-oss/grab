#pragma once

#include "core/environment.hpp"
#include "grab/capability.hpp"
#include "kernel/routing/registry.hpp"

#include <string>
#include <vector>

namespace grab::core
{

    struct CapabilityReport
    {
            std::string       id;
            std::string       provider;
            AvailabilityState state = AvailabilityState::Unavailable;
            std::string       reason;
            std::string       remediation;
    };

    struct DoctorReport
    {
            Environment                   environment;
            std::vector<CapabilityReport> capabilities;
    };

    [[nodiscard]]
    DoctorReport
    run_doctor( const Registry&    registry,
                const Environment& env );

    [[nodiscard]]
    std::string
    to_json( const DoctorReport& report );

    [[nodiscard]]
    std::string
    to_text( const DoctorReport& report );

    [[nodiscard]]
    int
    doctor_exit_code( const DoctorReport& report );

}    // namespace grab::core
