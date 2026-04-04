/*
 * trace.h - VxD debug trace macro.
 *
 * VxD-only. Include AFTER DDK headers (basedef.h, vmm.h, debug.h, vxdwraps.h).
 * Out_Debug_String is provided by vxdwraps.h (requires WANTVXDWRAPS).
 * Do NOT include this header in helper or test application code.
 */
#ifndef VMODEM_TRACE_H
#define VMODEM_TRACE_H

/* VTRACE - emit a string to the kernel debug port (SoftICE / serial monitor).
 * Active in both free and checked builds; output is visible whenever a kernel
 * debug monitor is connected.  Cast away const: Out_Debug_String takes PCHAR. */
#define VTRACE(msg)  Out_Debug_String((char *)(msg))

#endif /* VMODEM_TRACE_H */
