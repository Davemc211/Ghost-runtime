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

        const char* envPath = getenv("GHOST_TIER0_PUNCH_FILE");
        const char* path = envPath != nullptr ? envPath : ".ghost/clr-tier0.punch";

        // Best-effort directory creation when using the default path. We don't
        // pull in mkdir helpers from the PAL just for this — if the directory
        // doesn't exist, fopen returns NULL and we silently disable.
        s_sink = fopen(path, "wb");
    }

    void WriteCard(const ghost_punch_card& card)
    {
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
        ghost_punch_card card;
        FillCommon(card, GHOST_OP_BOOT_START, "EEStartup");
        // BootStart resets the correlation sequence — it's the first event in
        // the boot scenario. corr_sequence is already 0 from FillCommon since
        // s_corrSequence was zero at construction; assert the invariant for
        // future readers.
        card.corr_sequence = 0;
        s_corrSequence = 1;
        WriteCard(card);
    }

    void EmitBootReady(uint16_t durationMs)
    {
        ghost_punch_card card;
        FillCommon(card, GHOST_OP_BOOT_READY, "EEStartup");
        card.duration_ms = durationMs;
        WriteCard(card);
    }

    void Shutdown()
    {
        if (s_sink != nullptr)
        {
            fflush(s_sink);
            fclose(s_sink);
            s_sink = nullptr;
        }
    }
}
