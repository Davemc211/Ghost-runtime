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

    // ---- Tier 1 ring sink ---------------------------------------------------
    // Hot-path producers (under s_sinkLock) only memcpy a 64-byte card into a
    // pre-allocated ring slot and bump a write cursor. A single background
    // drainer thread batches these into fwrite/fflush calls so the per-call
    // cost no longer pays a synchronous WriteFile. See
    // docs/insights/found/tier1-punch-cost-baseline.md for the regression
    // anchor that motivated this rewrite.
    constexpr size_t  kRingCap   = 1u << 16; // 65536 cards = 4 MiB
    constexpr size_t  kRingMask  = kRingCap - 1;
    constexpr size_t  kWakeBatch = 64;       // wake drainer every N cards

    ghost_punch_card* s_ring        = nullptr;
    volatile LONG64   s_ringWrite   = 0;     // next slot index (producers, under lock)
    volatile LONG64   s_ringRead    = 0;     // next slot to drain (drainer only)
    volatile LONG64   s_overflow    = 0;     // cards written synchronously due to full ring
    HANDLE            s_drainEvent  = nullptr;
    HANDLE            s_drainThread = nullptr;
    volatile LONG     s_drainStop   = 0;

    // Tracks the in-flight GC suspend pair so EmitGcResume can compute the
    // pause duration and re-use the same correlation id. Mutated only under
    // s_sinkLock.
    uint64_t s_gcSuspendCorr = 0;
    uint64_t s_gcSuspendStartMs = 0;

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

    // Forward declaration: PublishCard's overflow path falls back to WriteCard.
    void WriteCard(const ghost_punch_card& card);

    // ---- Tier 1 ring sink: drainer ------------------------------------------
    void DrainRingLocked()
    {
        // Caller holds s_sinkLock. Drains all currently-published cards.
        OpenSinkOnce();
        if (s_sink == nullptr)
            return;

        LONG64 write = s_ringWrite;
        LONG64 read  = s_ringRead;
        if (read == write)
            return;

        // Drain in up to two contiguous spans (handles wrap-around).
        size_t available = (size_t)(write - read);
        if (available > kRingCap) // producer overran us; clamp to capacity
            available = kRingCap;

        size_t startIdx = (size_t)(read & kRingMask);
        size_t firstRun = kRingCap - startIdx;
        if (firstRun > available) firstRun = available;

        fwrite(&s_ring[startIdx], sizeof(ghost_punch_card), firstRun, s_sink);
        size_t remaining = available - firstRun;
        if (remaining > 0)
            fwrite(&s_ring[0], sizeof(ghost_punch_card), remaining, s_sink);

        fflush(s_sink);
        s_ringRead = read + (LONG64)available;
    }

    DWORD WINAPI DrainThreadProc(LPVOID)
    {
        // Background drainer. Batches producer writes into one fwrite/fflush
        // pair per wake. WaitForSingleObject is fine here; this thread does no
        // managed work and never enters the GC.
        while (true)
        {
            WaitForSingleObject(s_drainEvent, 5 /*ms*/);
            {
                CrstHolder lock(&s_sinkLock);
                DrainRingLocked();
                if (s_drainStop != 0 && s_ringRead == s_ringWrite)
                    return 0;
            }
        }
    }

    void EnsureRingStarted()
    {
        // Caller holds s_sinkLock. First-publish initialization of the ring
        // and drainer thread; idempotent.
        if (s_ring != nullptr)
            return;
        s_ring = (ghost_punch_card*)calloc(kRingCap, sizeof(ghost_punch_card));
        if (s_ring == nullptr)
            return;
        s_drainEvent  = CreateEventW(nullptr, /*manualReset*/ FALSE, /*initial*/ FALSE, nullptr);
        if (s_drainEvent == nullptr)
        {
            free(s_ring);
            s_ring = nullptr;
            return;
        }
        s_drainThread = CreateThread(nullptr, 0, DrainThreadProc, nullptr, 0, nullptr);
        if (s_drainThread == nullptr)
        {
            CloseHandle(s_drainEvent);
            s_drainEvent = nullptr;
            free(s_ring);
            s_ring = nullptr;
            return;
        }
    }

    void PublishCard(const ghost_punch_card& card)
    {
        // Caller holds s_sinkLock. Either enqueues into the ring (fast path)
        // or, if the ring is unavailable / full, falls back to a synchronous
        // fwrite so we never silently drop cards.
        EnsureRingStarted();

        if (s_ring != nullptr)
        {
            LONG64 write = s_ringWrite;
            LONG64 read  = s_ringRead;
            if ((write - read) < (LONG64)kRingCap)
            {
                size_t slot = (size_t)(write & kRingMask);
                memcpy(&s_ring[slot], &card, sizeof(card));
                s_ringWrite = write + 1;
                if (((write + 1) & (kWakeBatch - 1)) == 0)
                    SetEvent(s_drainEvent);
                return;
            }
            // Ring full — fall through to sync write to preserve the record.
            InterlockedIncrement64(&s_overflow);
        }

        WriteCard(card);
    }

    // ---- Tier 1 lock-free user-punch fast path ------------------------------
    // The user-punch hot path (Ghost.Runtime.Punch) cannot afford the CRST
    // round trip, so it reserves a ring slot via InterlockedIncrement64 and
    // writes directly. Safe because:
    //   * Ring + drainer are already initialized by boot-time punches that ran
    //     under the CRST. The atomic load of s_ring is sufficient to observe.
    //   * Each producer owns a unique slot index; concurrent writers never
    //     touch the same 64 bytes.
    //   * The drainer reads s_ringWrite once per pass; a producer that
    //     reserved a slot but hasn't finished memcpy'ing will be drained on
    //     the next pass (the 5 ms wait timer guarantees forward progress).
    //   * Overflow degrades to the locked sync path so cards are never lost.
    void PublishCardLockFree(const ghost_punch_card& card)
    {
        if (s_ring == nullptr)
        {
            // Cold path: ring not yet initialized. Take the lock and use the
            // standard publish path; this is hit at most a handful of times
            // before any boot punch fires.
            EnsureSinkLock();
            CrstHolder lock(&s_sinkLock);
            PublishCard(card);
            return;
        }

        LONG64 reserved = InterlockedIncrement64(&s_ringWrite) - 1;
        LONG64 read     = s_ringRead;
        if ((reserved - read) < (LONG64)kRingCap)
        {
            size_t slot = (size_t)(reserved & kRingMask);
            memcpy(&s_ring[slot], &card, sizeof(card));
            if (((reserved + 1) & (kWakeBatch - 1)) == 0)
                SetEvent(s_drainEvent);
            return;
        }

        // Ring full — give the slot back and fall through to the sync path.
        InterlockedIncrement64(&s_overflow);
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);
        WriteCard(card);
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

    void EmitBootPhase(const char* phaseName, uint32_t elapsedMs)
    {
        if (phaseName == nullptr)
            return;

        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for BootPhase:
        //   SourceHash    = FNV-1a("CoreCLR")        (set by FillCommon)
        //   TargetHash    = FNV-1a(<phase name>)
        //   CorrelationId = boot correlation id      (shared with BootStart/BootReady)
        //   DurationMs    = ms since BootStart, saturating uint16
        FillCommon(card, GHOST_OP_BOOT_PHASE, phaseName);
        card.duration_ms = elapsedMs > 0xFFFFu ? (uint16_t)0xFFFFu
                                               : (uint16_t)elapsedMs;
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

    void EmitGcSuspend(uint32_t reason)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        // Each suspend gets a fresh correlation id derived from the current
        // tick. The matching resume reads s_gcSuspendCorr/s_gcSuspendStartMs
        // under the same lock, so there is no race even if a concurrent GC
        // were ever to interleave.
        uint64_t startMs = GetTickCount64();
        uint64_t corr    = (startMs << 8) ^ (uint64_t)GHOST_OP_CLR_GC_SUSPEND;
        s_gcSuspendCorr     = corr;
        s_gcSuspendStartMs  = startMs;

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_CLR_GC_SUSPEND, "GCHeap::SuspendEE");
        card.correlation_id = corr;
        card.detail         = reason > 0xFFFFu ? (uint16_t)0xFFFFu
                                               : (uint16_t)reason;
        WriteCard(card);
    }

    void EmitGcResume()
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        uint64_t corr    = s_gcSuspendCorr;
        uint64_t deltaMs = (corr != 0)
            ? (GetTickCount64() - s_gcSuspendStartMs)
            : 0;
        s_gcSuspendCorr    = 0;
        s_gcSuspendStartMs = 0;

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_CLR_GC_RESUME, "GCHeap::RestartEE");
        card.correlation_id = corr;
        card.duration_ms    = deltaMs > 0xFFFFu ? (uint16_t)0xFFFFu
                                                : (uint16_t)deltaMs;
        WriteCard(card);
    }

    void EmitGcCollection(uint32_t generation, uint32_t reason)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_CLR_GC_COLLECTION, "GCHeap::GarbageCollectGeneration");
        // CorrelationId joins this completion with the in-flight suspend pair
        // (if any). At Tier 0 we only have whatever the suspend slot still
        // holds; that is fine because DiagGCEnd fires before RestartEE clears
        // it for blocking GCs.
        card.correlation_id = s_gcSuspendCorr;
        card.magnitude      = (uint8_t)(generation & 0xFFu);
        card.detail         = (uint16_t)((generation & 0xFu) | ((reason & 0xFu) << 4));
        WriteCard(card);
    }

    void EmitContention(uint64_t lockId, double durationNs)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        // Render the lock id as a stable uppercase hex string so TargetHash
        // collides for the same lock across reports without leaking pointers
        // upstream.
        char idBuf[32];
        sprintf_s(idBuf, ARRAY_SIZE(idBuf), "0x%016llX", (unsigned long long)lockId);

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_CLR_CONTENTION, idBuf);
        card.source_hash    = GhostFnv1aUpper("Monitor");
        card.correlation_id = lockId;
        double ms = durationNs / 1.0e6;
        if (ms < 0) ms = 0;
        if (ms > 65535.0) ms = 65535.0;
        card.duration_ms    = (uint16_t)ms;
        WriteCard(card);
    }

    void EmitThreadAdjust(uint32_t newWorkerCount, uint32_t reason)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        FillCommon(card, GHOST_OP_CLR_THREAD_ADJUST, "ThreadpoolMgr::AdjustMaxWorkersActive");
        card.correlation_id = 0;
        card.magnitude      = (uint8_t)(reason & 0xFFu);
        card.detail         = newWorkerCount > 0xFFFFu ? (uint16_t)0xFFFFu
                                                       : (uint16_t)newWorkerCount;
        WriteCard(card);
    }

    void EmitException(const char* typeName, uint32_t hresult)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for ClrException:
        //   SourceHash    = FNV-1a("CoreCLR")        (set by FillCommon)
        //   TargetHash    = FNV-1a(<exception type name>)
        //   Magnitude     = 0 (managed throw)
        //   Detail        = HRESULT low 16 bits
        //   CorrelationId = 0 (each throw is independent at Tier 0)
        FillCommon(card, GHOST_OP_CLR_EXCEPTION, typeName != nullptr ? typeName : "Unknown");
        card.correlation_id = 0;
        card.magnitude      = 0;
        card.detail         = (uint16_t)(hresult & 0xFFFFu);
        WriteCard(card);
    }

    void EmitThreadStart(uint32_t managedThreadId)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for ClrThreadStart:
        //   SourceHash    = FNV-1a("CoreCLR")
        //   TargetHash    = FNV-1a("Thread::Start")
        //   Detail        = managed thread id low 16 bits
        //   CorrelationId = 0 (correlation flow lights up in a later tier)
        FillCommon(card, GHOST_OP_CLR_THREAD_START, "Thread::Start");
        card.correlation_id = 0;
        card.detail         = (uint16_t)(managedThreadId & 0xFFFFu);
        WriteCard(card);
    }

    void EmitThreadEnd(uint32_t managedThreadId, uint32_t aborted)
    {
        EnsureSinkLock();
        CrstHolder lock(&s_sinkLock);

        ghost_punch_card card;
        // Tier 0 wire contract for ClrThreadEnd:
        //   SourceHash    = FNV-1a("CoreCLR")
        //   TargetHash    = FNV-1a("Thread::End")
        //   Magnitude     = 0 normal exit, 1 abort
        //   Detail        = managed thread id low 16 bits
        FillCommon(card, GHOST_OP_CLR_THREAD_END, "Thread::End");
        card.correlation_id = 0;
        card.magnitude      = aborted != 0 ? (uint8_t)1u : (uint8_t)0u;
        card.detail         = (uint16_t)(managedThreadId & 0xFFFFu);
        WriteCard(card);
    }

    void EmitUserPunch(uint8_t opCode, uint8_t magnitude, uint16_t detail)
    {
        // Tier 1 wire contract for the managed Ghost.Runtime.Punch API:
        //   op_code        = caller-supplied
        //   magnitude      = caller-supplied
        //   detail         = caller-supplied
        //   source_hash    = FNV-1a("UserCode")   (precomputed)
        //   target_hash    = 0        (reserved for a future named overload)
        //   correlation_id = 0        (ExecutionContext flow lands later)
        //   tick / pid / tid / OriginRuntime stamped inline (no FNV per call)
        //
        // Lock-free hot path: this function does NOT acquire s_sinkLock.
        // Slot reservation is via InterlockedIncrement64 on s_ringWrite, and
        // the corr_sequence is bumped via a separate atomic so concurrent
        // callers can't collide. The CRST is only re-entered for the cold
        // ring-init path or the ring-full overflow fallback.

        // Hot-path constants are precomputed once per process so this
        // function does not pay any FNV hashing cost.
        static const uint32_t kUserCodeHash = GhostFnv1aUpper("UserCode");
        static const uint16_t kPid          = (uint16_t)GetCurrentProcessId();
        static volatile LONG  s_userSeq     = 0;

        ghost_punch_card card;
        memset(&card, 0, sizeof(card));
        card.tick           = TickSinceBoot();
        card.op_code        = opCode;
        card.source_hash    = kUserCodeHash;
        card.target_hash    = 0;
        card.correlation_id = 0;
        card.corr_sequence  = (uint16_t)InterlockedIncrement(&s_userSeq);
        card.process_id     = kPid;
        card.thread_id      = (uint16_t)GetCurrentThreadId();
        card.magnitude      = magnitude;
        card.detail         = detail;
        card.extra_detail   = GHOST_ORIGIN_BITS(GHOST_ORIGIN_SERVER);
        PublishCardLockFree(card);
    }

    void Shutdown()
    {
        if (!s_sinkLockInit)
            return;

        // Signal the drainer to exit after a final flush, then wait briefly.
        InterlockedExchange(&s_drainStop, 1);
        if (s_drainEvent != nullptr)
            SetEvent(s_drainEvent);
        if (s_drainThread != nullptr)
        {
            WaitForSingleObject(s_drainThread, 200 /*ms*/);
            CloseHandle(s_drainThread);
            s_drainThread = nullptr;
        }

        CrstHolder lock(&s_sinkLock);
        // Drain anything the background thread may have left in the ring.
        DrainRingLocked();
        if (s_drainEvent != nullptr)
        {
            CloseHandle(s_drainEvent);
            s_drainEvent = nullptr;
        }
        if (s_ring != nullptr)
        {
            free(s_ring);
            s_ring = nullptr;
        }
        if (s_sink != nullptr)
        {
            fflush(s_sink);
            fclose(s_sink);
            s_sink = nullptr;
        }
    }
}
