#pragma once
// Temporary relocation shim (W4.1): src/platform/x11 was moved to
// src/drivers/desktop/x11. This keeps the fenced input-purge-worktree includers
// (src/input, src/event, tests/input) building unchanged. Remove once those
// includers are repointed. See docs/superpowers/plans/2026-07-16-wave4-observation.md
#include "drivers/desktop/x11/protocol.hpp"
