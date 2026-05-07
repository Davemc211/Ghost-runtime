// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// ghost_emit.cpp — Tier 0 emitter implementation. See ghost_emit.h for scope.

#include "common.h"
#include "ghost_emit.h"

// The protocol header is the single source of truth for the wire layout.
// A copy lives under src/coreclr/vm/ghost/ alongside this file so the runtime
// build is self-contained; drift against the canonical
// GhostDebugger/Ghost.Protocol/Native/ghost_punch.h is enforced by the
// GhostNativeHeaderLayoutTests / GhostVendoredHeaderSyncTests in the
// Ghost.Protocol.Conformance suite.
#include "ghost_punch.h"

#include <stdio.h>
#include <string.h>

namespace
{
    // FNV-1a 32-bit, uppercase-folded ASCII — must match Ghost.Protocol/GhostHash.cs.
    uint32_t GhostFnv1aUpper(const char* s)
    {
        uint32_t hash = 0x811C9DC5u;
        if (s == nullptr)
            return hash;
        for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        {
            unsigned char c = *p;
            if (c >= 'a' && c <= 'z')
                c = (unsigned char)(c - ('a' - 'A'));
            hash ^= c;
            hash *= 0x01000193u;
        }
        return hash;
    }

    // Single sink, opened lazily, never reopened.
    FILE*    s_sink = nullptr;
    bool     s_sinkAttempted = false;
    uint64_t s_bootBaselineTicks = 0;
    uint16_t s_corrSequence = 0;

    // Tier 0 emitters can fire from multiple threads (e.g. AssemblyLoad on
    // worker threads after EE startup). Serialize sink writes and the
    // s_corrSequence counter with a CRST. Created lazily under InitOnce.
    CrstStatic s_sinkLock;
    bool       s_sinkLockInit = false;

    void EnsureSinkLock()
    {
        if (s_sinkLockInit)
            return;
        // CrstStatic::Init is idempotent w.r.t. our own usage but not racy-safe;
        // gate via InterlockedCompareExchange to make first-use single-shot.
        static LONG s_initOnce = 0;
        if (InterlockedCompareExchange(&s_initOnce, 1, 0) == 0)
        {
            s_sinkLock.Init(CrstLeafLock, CRST_UNSAFE_ANYMODE);
            s_sinkLockInit = true;
        }
        else
        {
            // Spin briefly for the winner to finish init. Tier 0 only.
            while (!s_sinkLockInit) { YieldProcessor(); }
        }
    }

    // Boot id = monotonic constant for the run; the three boot punches share it.
    // Low 16 bits = a marker; high bits = process id so cross-process collisions
    // can't fool the validator.
    uint64_t BootCorrelationId()
    {
        return ((uint64_t)GetCurrentProcessId() << 32) | 0xB007ULL;
    }

    uint32_t TickSinceBoot()
    {
        ULONGLONG now = GetTickCount64();
        if (s_bootBaselineTicks == 0)
            s_bootBaselineTicks = now;
        ULONGLONG delta = now - s_bootBaselineTicks;
        if (delta > 0xFFFFFFFFULL)
            delta = 0xFFFFFFFFULL;
        return (uint32_t)delta;
    }

    void OpenSinkOnce()
    {
        if (s_sinkAttempted)
            return;
        s_sinkAttempted = true;

        const char* envPath = getenv("GHOST_TIER0_RUNTIME_PUNCH");
        if (envPath == nullptr)
            envPath = getenv("GHOST_TIER0_PUNCH_FILE");
        const char* path = envPath != nullptr ? envPath : ".ghost/clr-tier0.punch";

        // Best-effort directory creation when using the default path. We don't
        // pull in mkdir helpers from the PAL just for this — if the directory
        // doesn't exist, fopen returns NULL and we silently disable.
        s_sink = fopen(path, "wb");
    }

    void WriteCard(const ghost_punch_card& card)
    {
        // Caller holds s_sinkLock.
        OpenSinkOnce();
        if (s_sink == nullptr)
            return;

        // Wire is 64 bytes, packed, little-endian. Host is little-endian on
        // every CoreCLR target; if that ever changes, swap here.
        fwrite(&card, 1, sizeof(card), s_sink);
        fflush(s_sink);
    }

    void FillCommon(ghost_punch_card& card, uint8_t opCode, const char* targetName)
    {
        // Caller holds s_sinkLock so the s_corrSequence increment is safe.
        memset(&card, 0, sizeof(card));
        card.tick           = TickSinceBoot();
        card.op_code        = opCode;
        card.source_hash    = GhostFnv1aUpper("CoreCLR");
        card.target_hash    = GhostFnv1aUpper(targetName);
        card.correlation_id = BootCorrelationId();
        card.corr_sequence  = s_corrSequence++;
        card.process_id     = (uint16_t)GetCurrentProcessId();
        card.thread_id      = (uint16_t)GetCurrentThreadId();
        card.extra_detail   = GHOST_ORIGIN_BITS(GHOST_ORIGIN_SERVER);
    }
}

namespace GhostTier0
{
    void EmitBootStart()
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_BOOT_START, "EEStartup");
        // BootStart resets the correlation sequence — it's the first event in
        // the boot scenario. FillCommon already advanced s_corrSequence to 1;
        // overwrite this card's slot with 0 so BootStart is sequence 0 by
        // protocol contract.
        card.corr_sequence = 0;
        s_corrSequence     = 1;
        WriteCard(card);
    }

    void EmitBootReady(uint16_t durationMs)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_BOOT_READY, "EEStartup");
        card.duration_ms = durationMs;
        WriteCard(card);
    }

    void EmitAssemblyLoad(const char* simpleName)
    {
        if (simpleName == nullptr)
            return;

        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for ClrAssemblyLoad:
        //   SourceHash = FNV-1a("Default")           (load context name)
        //   TargetHash = FNV-1a(<simple name>)       (no version/culture/PKT)
        //   CorrelationId = 0                        (each load is independent)
        FillCommon(card, GHOST_OP_CLR_ASSEMBLY_LOAD, simpleName);
        card.source_hash    = GhostFnv1aUpper("Default");
        card.correlation_id = 0;
        WriteCard(card);
    }

    void EmitJitCompile(const char* moduleSimpleName,
                        const char* methodName,
                        uint32_t    nativeCodeSize,
                        uint32_t    elapsedMs)
    {
        if (moduleSimpleName == nullptr || methodName == nullptr)
            return;

        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for ClrJitCompile:
        //   SourceHash    = FNV-1a(<module simple name>)
        //   TargetHash    = FNV-1a(<method name>)
        //   Detail        = native code size, saturating
        //   DurationMs    = elapsed ms, saturating
        //   CorrelationId = 0 (each compile is independent)
        FillCommon(card, GHOST_OP_CLR_JIT_COMPILE, methodName);
        card.source_hash    = GhostFnv1aUpper(moduleSimpleName);
        card.correlation_id = 0;
        card.detail         = nativeCodeSize > 0xFFFFu ? (uint16_t)0xFFFFu
                                                      : (uint16_t)nativeCodeSize;
        card.duration_ms    = elapsedMs > 0xFFFFu ? (uint16_t)0xFFFFu
                                                  : (uint16_t)elapsedMs;
        WriteCard(card);
    }

    void Shutdown()
    {
        if (s_sinkLockInit)
        {
            CrstHolder lock(&s_sinkLock);
            if (s_sink != nullptr)
            {
                fflush(s_sink);
                fclose(s_sink);
                s_sink = nullptr;
            }
        }
    }
}
