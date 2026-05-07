/*
 * ghost_punch.h — C header for ghost-protocol/v1 punch-card emitters.
 *
 * This is the source of truth for non-managed runtimes (CoreCLR fork, native
 * profilers, etc.). It mirrors the C# reference implementation in
 *   Ghost.Protocol/GhostPunchCard.cs
 *   Ghost.Protocol/GhostOpCodes.cs
 * byte-for-byte. Drift is prevented by:
 *   - the static_asserts below (caught at C/C++ compile time), and
 *   - GhostNativeHeaderLayoutTests in Ghost.Protocol.Conformance, which parses
 *     this file and checks every offset against the canonical wire layout.
 *
 * Wire invariants (see Ghost.Protocol/SPEC.md and docs/PUNCHCARD_SPEC.md):
 *   - exactly 64 bytes, little-endian, packed (no implicit padding)
 *   - field offsets locked; new fields only into the reserved tail
 *   - emitters MUST stamp OriginRuntime into the top 2 bits of extra_detail
 */

#ifndef GHOST_PUNCH_H_
#define GHOST_PUNCH_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define GHOST_PACK_PUSH __pragma(pack(push, 1))
#  define GHOST_PACK_POP  __pragma(pack(pop))
#elif defined(__GNUC__) || defined(__clang__)
#  define GHOST_PACK_PUSH _Pragma("pack(push, 1)")
#  define GHOST_PACK_POP  _Pragma("pack(pop)")
#else
#  define GHOST_PACK_PUSH
#  define GHOST_PACK_POP
#endif

/* ---------------- Wire layout (64 bytes, LE, packed) ---------------- */

GHOST_PACK_PUSH
typedef struct ghost_punch_card {
    /*  0 */ uint32_t tick;
    /*  4 */ uint8_t  op_code;
    /*  5 */ uint8_t  magnitude;
    /*  6 */ uint32_t source_hash;
    /* 10 */ uint32_t target_hash;
    /* 14 */ uint16_t floor_index;
    /* 16 */ uint64_t correlation_id;
    /* 24 */ uint32_t parent_hash;
    /* 28 */ uint16_t detail;
    /* 30 */ uint16_t duration_ms;
    /* 32 */ uint8_t  flags;
    /* 33 */ uint16_t process_id;        /* unaligned by spec — do not pad */
    /* 35 */ uint16_t thread_id;         /* unaligned by spec — do not pad */
    /* 37 */ uint16_t threadpool_queue;  /* unaligned by spec — do not pad */
    /* 39 */ uint16_t corr_sequence;     /* unaligned by spec — do not pad */
    /* 41 */ uint8_t  gc_pressure;
    /* 42 */ uint32_t allocated_kb;
    /* 46 */ uint16_t payload_size;
    /* 48 */ uint64_t parent_correlation_id;
    /* 56 */ uint32_t stack_sample_hash;
    /* 60 */ uint16_t extra_detail;       /* top 2 bits = OriginRuntime */
    /* 62 */ uint16_t reserved;           /* lo byte = service_id, hi = parasite_marker */
} ghost_punch_card;
GHOST_PACK_POP

/* Compile-time guarantee that offsets match the wire spec exactly. */
#define GHOST_OFFSET_ASSERT(field, expected)                                   \
    typedef char ghost_offset_##field##_must_be_##expected                     \
        [(offsetof(ghost_punch_card, field) == (expected)) ? 1 : -1]

GHOST_OFFSET_ASSERT(tick,                   0);
GHOST_OFFSET_ASSERT(op_code,                4);
GHOST_OFFSET_ASSERT(magnitude,              5);
GHOST_OFFSET_ASSERT(source_hash,            6);
GHOST_OFFSET_ASSERT(target_hash,           10);
GHOST_OFFSET_ASSERT(floor_index,           14);
GHOST_OFFSET_ASSERT(correlation_id,        16);
GHOST_OFFSET_ASSERT(parent_hash,           24);
GHOST_OFFSET_ASSERT(detail,                28);
GHOST_OFFSET_ASSERT(duration_ms,           30);
GHOST_OFFSET_ASSERT(flags,                 32);
GHOST_OFFSET_ASSERT(process_id,            33);
GHOST_OFFSET_ASSERT(thread_id,             35);
GHOST_OFFSET_ASSERT(threadpool_queue,      37);
GHOST_OFFSET_ASSERT(corr_sequence,         39);
GHOST_OFFSET_ASSERT(gc_pressure,           41);
GHOST_OFFSET_ASSERT(allocated_kb,          42);
GHOST_OFFSET_ASSERT(payload_size,          46);
GHOST_OFFSET_ASSERT(parent_correlation_id, 48);
GHOST_OFFSET_ASSERT(stack_sample_hash,     56);
GHOST_OFFSET_ASSERT(extra_detail,          60);
GHOST_OFFSET_ASSERT(reserved,              62);

typedef char ghost_punch_card_must_be_64_bytes
    [(sizeof(ghost_punch_card) == 64) ? 1 : -1];

/* ---------------- OriginRuntime (top 2 bits of extra_detail) ---------------- */

#define GHOST_ORIGIN_UNKNOWN 0u
#define GHOST_ORIGIN_SERVER  1u
#define GHOST_ORIGIN_CLIENT  2u
#define GHOST_ORIGIN_MIXED   3u

#define GHOST_ORIGIN_BITS(origin)        ((uint16_t)(((uint32_t)(origin) & 0x3u) << 14))
#define GHOST_EXTRA_DETAIL_PAYLOAD(xd)   ((uint16_t)((xd) & 0x3FFFu))
#define GHOST_EXTRA_DETAIL_ORIGIN(xd)    ((uint8_t) (((xd) >> 14) & 0x3u))

/* Tail-byte helpers for the reserved word at offset 62. */
#define GHOST_RESERVED_PACK(svc, parasite) \
    ((uint16_t)(((uint16_t)(svc) & 0xFFu) | (((uint16_t)(parasite) & 0xFFu) << 8)))
#define GHOST_RESERVED_SERVICE_ID(r)      ((uint8_t)((r) & 0xFFu))
#define GHOST_RESERVED_PARASITE_MARKER(r) ((uint8_t)(((r) >> 8) & 0xFFu))

/* ---------------- Flags byte (offset 32) ---------------- */

#define GHOST_FLAG_HOT_CODE       0x01u
#define GHOST_FLAG_HAS_SAMPLE     0x02u
#define GHOST_FLAG_ANOMALY        0x04u
#define GHOST_FLAG_ASYNC          0x08u
#define GHOST_FLAG_RETRY          0x10u
#define GHOST_FLAG_USER_INITIATED 0x20u
#define GHOST_FLAG_AUTHENTICATED  0x40u
#define GHOST_FLAG_FATAL          0x80u

/* ---------------- Tier 0 opcodes (System 0x0_, CLR 0xC_) ---------------- */
/* Full catalog lives in Ghost.Protocol/GhostOpCodes.cs — Tier 0 publishes
 * only the System and CLR slots the CoreCLR fork is allowed to emit. */

#define GHOST_OP_HEARTBEAT          0x00u
#define GHOST_OP_GENERAL            0x01u
#define GHOST_OP_LOG_WARNING        0x02u
#define GHOST_OP_LOG_ERROR          0x03u
#define GHOST_OP_LOG_CRITICAL       0x04u
#define GHOST_OP_BOOT_START         0x05u
#define GHOST_OP_BOOT_PHASE         0x06u
#define GHOST_OP_BOOT_READY         0x07u

#define GHOST_OP_CLR_GC_SUSPEND     0xC0u
#define GHOST_OP_CLR_GC_RESUME      0xC1u
#define GHOST_OP_CLR_GC_COLLECTION  0xC2u
#define GHOST_OP_CLR_JIT_COMPILE    0xC3u
#define GHOST_OP_CLR_CONTENTION     0xC4u
#define GHOST_OP_CLR_THREAD_ADJUST  0xC5u
#define GHOST_OP_CLR_ASSEMBLY_LOAD  0xC6u

/* High nibble of op_code is the category (see GhostPunchCard.Category). */
#define GHOST_CATEGORY(op) ((uint8_t)((op) >> 4))

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GHOST_PUNCH_H_ */
