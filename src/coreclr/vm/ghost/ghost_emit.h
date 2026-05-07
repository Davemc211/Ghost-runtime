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

    // Boot phase milestone emit (ceemain.cpp call sites between BootStart
    // and BootReady). `phaseName` identifies the phase (e.g.
    // "SystemDomain::Init") and is hashed into TargetHash. `elapsedMs` is
    // the time since BootStart so each phase carries a cumulative duration.
    // Shares the boot CorrelationId and is assigned the next corr_sequence.
    void EmitBootPhase(const char* phaseName, uint32_t elapsedMs);

    void EmitBootReady(uint16_t durationMs);

    // Assembly load emit (Assembly::Init call site). `simpleName` is the
    // assembly's simple name (no version/culture/PKT) — the Tier 0 wire
    // contract hashes only the simple name into TargetHash. May be called
    // concurrently from multiple threads; the implementation serializes
    // writes and the per-record corr_sequence counter.
    void EmitAssemblyLoad(const char* simpleName);

    // JIT compile emit (prestub.cpp call site, after JitCompileCodeLocked
    // returns successfully). Per the Tier 0 wire contract:
    //   SourceHash    = FNV-1a(<module simple name>)
    //   TargetHash    = FNV-1a(<method name>)
    //   Detail        = native code size in bytes (saturating uint16)
    //   DurationMs    = JIT elapsed time in ms (saturating uint16)
    //   CorrelationId = 0 (each compile is independent)
    // High-frequency call site — must be cheap and never throw.
    void EmitJitCompile(const char* moduleSimpleName,
                        const char* methodName,
                        uint32_t    nativeCodeSize,
                        uint32_t    elapsedMs);

    // Test/diagnostics: flush + close the sink. Safe to call multiple times.
    void Shutdown();
}

#endif // GHOST_EMIT_H_
