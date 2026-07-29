#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Live AT-SPI2 tree enumeration over the accessibility bus. Produces the flat
// AtspiAccessible vector the AtspiTreeSource consumes, walking the real
// document tree of every running application via raw libdbus. This is the
// composition layer the tree source was always designed to receive (its
// AccessibleEnumerator is injected); until now the default was empty, so no
// grab path could read a document tree.

#include "drivers/semantic/atspi/atspi_tree_source.hpp"

#include <cstddef>

namespace grab::drivers::semantic::atspi
{

    // Default ceiling on nodes returned by one walk. A large document tree is a
    // few thousand nodes; this leaves generous headroom while still bounding a
    // pathological or adversarial page.
    inline constexpr std::size_t defaultMaxNodes = 20'000U;

    struct EnumeratorOptions
    {
            // Hard ceiling on nodes returned by one walk. When the walk reaches
            // the cap it stops descending rather than growing without bound —
            // never silently unbounded.
            std::size_t max_nodes{ defaultMaxNodes };
    };

    // Builds an AccessibleEnumerator that connects to the accessibility bus on
    // first use (and reconnects if the bus went away), then walks the tree on
    // every invocation. The walk is synchronous and blocking: it is meant for
    // the per-page snapshot cadence of a visual crawler, not a hot loop.
    //
    // The enumerator returns an error only when the a11y bus itself is
    // unreachable; a per-node D-Bus failure is logged and that node is skipped,
    // so one broken accessible never sinks the whole harvest.
    [[nodiscard]]
    AtspiTreeSource::AccessibleEnumerator
    make_dbus_enumerator( EnumeratorOptions options = {} );

}    // namespace grab::drivers::semantic::atspi
