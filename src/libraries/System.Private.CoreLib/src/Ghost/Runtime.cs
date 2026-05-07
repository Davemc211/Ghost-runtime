// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Ghost
{
    /// <summary>
    /// Tier 1 managed entry point for Ghost punch emission. The first slice
    /// routes through a QCall to <c>NativeRuntimeEventSource_LogGhostUserPunch</c>,
    /// which forwards to <c>GhostTier0::EmitUserPunch</c> in the runtime fork.
    /// A subsequent Tier 1 slice replaces this QCall transition with a JIT
    /// intrinsic that performs an inline ring write, targeting the
    /// gameplan's &lt;= 15 ns per-punch gate.
    /// </summary>
    /// <remarks>
    /// Wire contract (Tier 1 slice 1):
    /// <list type="bullet">
    /// <item><description><c>op_code</c> = caller-supplied (any byte; see <c>Ghost.Protocol.GhostOpCodes</c>).</description></item>
    /// <item><description><c>magnitude</c> / <c>detail</c> = caller-supplied.</description></item>
    /// <item><description><c>source_hash</c> = FNV-1a("UserCode"), <c>target_hash</c> = 0.</description></item>
    /// <item><description><c>correlation_id</c> = 0; ExecutionContext propagation lands in a later slice.</description></item>
    /// <item><description>Origin is stamped <c>Server</c> by the runtime emitter.</description></item>
    /// </list>
    /// </remarks>
    public static partial class Runtime
    {
        /// <summary>
        /// Emit a Ghost punch from managed code.
        /// </summary>
        /// <param name="opCode">Punch opcode (see <c>Ghost.Protocol.GhostOpCodes</c>; <c>Custom1</c>/<c>Custom2</c> are reserved for application use).</param>
        /// <param name="magnitude">Caller-supplied magnitude byte.</param>
        /// <param name="detail">Caller-supplied detail word.</param>
        [CLSCompliant(false)]
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void Punch(byte opCode, byte magnitude, ushort detail)
        {
            LogGhostUserPunch(opCode, magnitude, detail);
        }

        [LibraryImport(RuntimeHelpers.QCall, EntryPoint = "NativeRuntimeEventSource_LogGhostUserPunch")]
        [SuppressGCTransition]
        private static partial void LogGhostUserPunch(byte opCode, byte magnitude, ushort detail);
    }
}
