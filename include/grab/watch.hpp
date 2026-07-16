#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Public watch vocabulary: re-exports Subscription, SubscriptionScope,
// QueueOptions, SubscriptionEvent, and QueueGapMarker from grab/event_bus.hpp.
// Internalizing event_bus.hpp is Wave-6 scope; until then this is a thin alias header.
#include "grab/event_bus.hpp"
