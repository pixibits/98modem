/*
 * trace.c - Debug trace support for the VMODEM VxD.
 *
 * The VTRACE macro (trace.h) calls Out_Debug_String, which is available in
 * both free and checked builds via vxdwraps.h.  This translation unit exists
 * to ensure the compile and link succeed for the trace module slot in the
 * build; all real work is done through the macro in the callers.
 */

#define WANTVXDWRAPS

#include <basedef.h>
#include <vmm.h>
#include <debug.h>
#include <vxdwraps.h>

#include "trace.h"

#pragma VxD_LOCKED_CODE_SEG

/* No runtime functions required; VTRACE macro handles everything inline. */
