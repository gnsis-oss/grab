#pragma once

#include "core/provider.hpp"

#include <memory>
#include <vector>

namespace grab::core
{

    class Registry
    {
        public:

            explicit Registry( std::vector<std::unique_ptr<Provider>> providers );

            [[nodiscard]]
            std::vector<const Provider*>
            providers_for( Capability capability ) const;

            [[nodiscard]]
            std::vector<const Provider*>
            all() const;

        private:

            std::vector<std::unique_ptr<Provider>> providers_;
    };

    class RegistryBuilder
    {
        public:

            // Precondition: provider != nullptr.
            RegistryBuilder&
            add( std::unique_ptr<Provider> provider );

            [[nodiscard]]
            Registry
            build() &&;

        private:

            std::vector<std::unique_ptr<Provider>> providers_;
    };

    // All compiled-in production providers. Phase 1A: empty; the XCB backend
    // plan registers providers here.
    [[nodiscard]]
    Registry
    builtin_registry();

}    // namespace grab::core
