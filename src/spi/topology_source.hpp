#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

namespace grab::spi
{

    class TopologySource
    {
        public:

            TopologySource()                        = default;
            virtual ~TopologySource()               = default;
            TopologySource( const TopologySource& ) = delete;
            TopologySource&
            operator=( const TopologySource& ) = delete;
            TopologySource( TopologySource&& ) = delete;
            TopologySource&
            operator=( TopologySource&& ) = delete;
    };

}    // namespace grab::spi
