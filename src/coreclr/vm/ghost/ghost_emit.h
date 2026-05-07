// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// ghost_emit.h — Tier 0 emit shim for the Ghost-CLR fork.
//
// Tier 0 scope: BootStart / BootReady only, written as raw 64-byte
// `ghost_punch_card` records to a single sink file. This is the absolute
// minimum needed to validate the emit loop end-to-end against the canonical
// fixtures shipped under
//   Ghost.Protocol.Conformance/Fixtures/Tier0/*.punch
//
// Subsequent tiers will replace the synchronous file write with the punch
// ring + JSONL writer described in docs/GHOST_CLR_GAMEPLAN.md. The wire bytes
// produced here MUST stay byte-compatible with the protocol — that contract
// is enforced by `ghost_punch.h`'s static_asserts and by the C# header
// conformance test.

#ifndef GHOST_EMIT_H_
#define GHOST_EMIT_H_

#include <stdint.h>

// Sink path resolution order:
//   1. environment variable GHOST_TIER0_PUNCH_FILE (absolute or relative path)
//   2. <cwd>/.ghost/clr-tier0.punch
//
// The sink is opened lazily on the first emit and truncated. Failures are
// silently swallowed — Tier 0 is a development-time validation loop, not a
// production telemetry path.
namespace GhostTier0
{
    // Boot lifecycle emits (ceemain.cpp call sites).
    void EmitBootStart();
    void EmitBootReady(uint16_t durationMs);

    // Test/diagnostics: flush + close the sink. Safe to call multiple times.
    void Shutdown();
}

#endif // GHOST_EMIT_H_
