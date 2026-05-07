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

    // GC pause pair (gcenv.ee.cpp call sites). EmitGcSuspend assigns a fresh
    // CorrelationId and records the start tick on a process-wide slot;
    // EmitGcResume reads that slot to compute DurationMs and emit with the
    // same CorrelationId, so suspend/resume records can be joined by id.
    //   SourceHash    = FNV-1a("CoreCLR")
    //   TargetHash    = FNV-1a("GCHeap::SuspendEE" | "GCHeap::RestartEE")
    //   Detail        = SUSPEND_REASON (suspend only)
    void EmitGcSuspend(uint32_t reason);
    void EmitGcResume();

    // GC collection completion (DiagGCEnd call site). Per the Tier 0 wire
    // contract:
    //   SourceHash = FNV-1a("CoreCLR")
    //   TargetHash = FNV-1a("GCHeap::GarbageCollectGeneration")
    //   Magnitude  = generation
    //   Detail     = (generation & 0xF) | ((reason & 0xF) << 4)
    void EmitGcCollection(uint32_t generation, uint32_t reason);

    // Lock contention stop (nativeeventsource.cpp call site). Per the Tier 0
    // wire contract:
    //   SourceHash    = FNV-1a("Monitor")
    //   TargetHash    = FNV-1a(hex(lockId))   (lock identity, opaque at Tier 0)
    //   CorrelationId = lockId
    //   DurationMs    = saturating(durationNs / 1e6)
    void EmitContention(uint64_t lockId, double durationNs);

    // ThreadPool worker count adjustment (nativeeventsource.cpp call site).
    //   SourceHash = FNV-1a("CoreCLR")
    //   TargetHash = FNV-1a("ThreadpoolMgr::AdjustMaxWorkersActive")
    //   Detail     = new worker count (saturating uint16)
    //   Magnitude  = adjustment reason (low byte)
    void EmitThreadAdjust(uint32_t newWorkerCount, uint32_t reason);

    // ---- Tier 1: managed user-punch entrypoint ------------------------------
    //
    // Backs the System.Private.CoreLib `Ghost.Runtime.Punch(opCode, magnitude,
    // detail)` API via a QCall in nativeeventsource.cpp. The caller chooses
    // the opCode (typically Custom1/Custom2 from GhostOpCodes, but any byte is
    // wire-legal); the runtime stamps tick / process id / thread id and the
    // OriginRuntime=Server marker the same way every other Tier 0 emitter
    // does. CorrelationId is 0 at Tier 1 — correlation propagation lights up
    // with ExecutionContext flow in a later slice.
    //
    //   SourceHash = FNV-1a("UserCode")
    //   TargetHash = 0    (reserved for a future overload taking a name hash)
    //   Magnitude  = caller-supplied
    //   Detail     = caller-supplied
    //
    // Must be cheap: this is the substrate the Tier 1 JIT intrinsic will
    // replace with an inline ring write, so the QCall version is the honest
    // pre-intrinsic baseline measured by samples/Tier0Bench.
    void EmitUserPunch(uint8_t opCode, uint8_t magnitude, uint16_t detail);

    // Test/diagnostics: flush + close the sink. Safe to call multiple times.
    void Shutdown();
}

#endif // GHOST_EMIT_H_
