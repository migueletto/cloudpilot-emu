/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
        Copyright (c) 2000-2001 Palm, Inc. or its subsidiaries.
        All rights reserved.

        This file is part of the Palm OS Emulator.

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.
\* ===================================================================== */

#include "EmPalmOS.h"

#include "Byteswapping.h"
#include "Debugger.h"
#include "EmBankDRAM.h"    // EmBankDRAM::SetLong
#include "EmBankMapped.h"  // EmBankMapped::SetLong
#include "EmBankROM.h"     // EmBankROM::SetLong
#include "EmBankSRAM.h"    // EmBankSRAM::SetLong
#include "EmCPU68K.h"      // gCPU68K, gStackHigh, etc.
#include "EmCommon.h"
#include "EmLowMem.h"
#include "EmMemory.h"       // CEnableFullAccess
#include "EmPalmStructs.h"  // EmAliasCardHeaderType
#include "EmPatchMgr.h"     // EmPatchMgr
#include "EmSession.h"      // gSession->Reset
#include "EmSystemState.h"
#include "Logging.h"
#include "Miscellaneous.h"
#include "Miscellaneous.h"  // GetSystemCallContext
#include "ROMStubs.h"
#include "UAE.h"  // CHECK_STACK_POINTER_DECREMENT

#include "Palm.h"
#include <stdarg.h>

#define LOG_FUNCTION_CALLS 0

namespace {
    constexpr int MIN_CYCLES_BETWEEN_EVENTS = 10000;
    constexpr int EVENT_QUEUE_SIZE = 20;
}  // namespace

static emuptr gBigROMEntry;

EmThreadSafeQueue<PenEvent> EmPalmOS::penEventQueue{EVENT_QUEUE_SIZE};
EmThreadSafeQueue<KeyboardEvent> EmPalmOS::keyboardEventQueue{EVENT_QUEUE_SIZE};
EmThreadSafeQueue<PenEvent> EmPalmOS::penEventQueueIncoming{EVENT_QUEUE_SIZE};
EmThreadSafeQueue<KeyboardEvent> EmPalmOS::keyboardEventQueueIncoming{EVENT_QUEUE_SIZE};
uint64 EmPalmOS::lastEventPromotedAt{0};
LocalID EmPalmOS::dbForLaunch{0};
bool EmPalmOS::postNilEvent{false};

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::Initialize
 *
 * DESCRIPTION: Standard initialization function.  Responsible for
 *				initializing this sub-system when a new session is
 *				created.  Will be followed by at least one call to
 *				Reset or Load.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmPalmOS::Initialize(void) {
    EmAssert(gCPU68K);

    gCPU68K->InstallHookException(kException_SysCall, HandleTrap15);
    gCPU68K->InstallHookJSR_Ind(HandleJSR_Ind);

    gBigROMEntry = EmMemNULL;

    ClearQueues();
    dbForLaunch = 0;
    postNilEvent = false;

    EmPatchMgr::Initialize();
}

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::Reset
 *
 * DESCRIPTION:	Standard reset function.  Sets the sub-system to a
 *				default state.  This occurs not only on a Reset (as
 *				from the menu item), but also when the sub-system
 *				is first initialized (Reset is called after Initialize)
 *				as well as when the system is re-loaded from an
 *				insufficient session file.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmPalmOS::Reset(void) {
    EmPatchMgr::Reset();

    ClearQueues();
    dbForLaunch = 0;
    postNilEvent = false;
}

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::Dispose
 *
 * DESCRIPTION:	Standard dispose function.  Completely release any
 *				resources acquired or allocated in Initialize and/or
 *				Load.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

void EmPalmOS::Dispose(void) {
    EmPatchMgr::Dispose();

    ClearQueues();
}

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::HandleTrap15
 *
 * DESCRIPTION: Handle a trap. Traps are of the format TRAP $F / $Axxx.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

Bool EmPalmOS::HandleTrap15(ExceptionNumber) { return EmPalmOS::HandleSystemCall(true); }

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::HandleJSR_Ind
 *
 * DESCRIPTION: Check for SYS_TRAP_FAST calls.
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

Bool EmPalmOS::HandleJSR_Ind(emuptr oldpc, emuptr dest) {
    Bool handledIt = false;  // Default to calling ROM.

    //	inline asm SysTrapFast(Int trapNum)
    //	{
    //			MOVE.L	struct(LowMemType.fixed.globals.sysDispatchTableP), A1
    //			MOVE.L	((trapNum-sysTrapBase)*4)(A1), A1
    //			JSR 	(A1)	// call it
    //	}
    //
    //	#define SYS_TRAP_FAST(trapNum)
    //		FIVEWORD_INLINE(
    //			0x2278, 0x0122,
    //			0x2269, (trapNum-sysTrapBase)*4,
    //			0x4e91)

    if (EmMemGet16(oldpc) == 0x4e91 && EmMemGet16(oldpc - 4) == 0x2269 &&
        EmMemGet16(oldpc - 8) == 0x2278) {
        handledIt = EmPalmOS::HandleSystemCall(false);
    } else {
        if (gBigROMEntry == EmMemNULL) {
            emuptr base = EmBankROM::GetMemoryStart();

            // Check every romDelta (4K) for up to maxBigROMOffset
            //(256K) till we find the "Big" ROM

            const UInt32 romDelta = 4 * 1024L;           // BigROM starts on a 4K boundary
            const UInt32 maxBigROMOffset = 256 * 1024L;  // Give up looking past here
            UInt16 loops = maxBigROMOffset / romDelta;   // How many loops to do
            emuptr bP = base + romDelta;

            while (loops--) {
                // Ignore older card headers that might have been
                // programmed in at a lower address (hdrVersion < 3)

                EmAliasCardHeaderType<PAS> cardHdr(bP);

                UInt32 signature = cardHdr.signature;
                UInt16 hdrVersion = cardHdr.hdrVersion;

                if (signature == sysCardSignature && hdrVersion >= 3) {
                    gBigROMEntry = cardHdr.resetVector;
                    break;
                }

                bP += romDelta;
            }

            // See if we found it (may have hdrVersion < 3)

            if (gBigROMEntry == EmMemNULL) {
                EmAliasCardHeaderType<PAS> smallCardHdr(base);
                UInt32 bigROMOffset;

                if (smallCardHdr.hdrVersion == 2) {
                    bigROMOffset = smallCardHdr.bigROMOffset & 0x000FFFFF;
                } else {
                    bigROMOffset = 0x3000;
                }

                EmAliasCardHeaderType<PAS> bigCardHdr(base + bigROMOffset);
                gBigROMEntry = bigCardHdr.resetVector;
            }
        }

        if (dest == gBigROMEntry) {
            EmAssert(gSession);
            gSession->ScheduleReset(EmSession::ResetType::sys);
        }
    }

    return handledIt;
}

/***********************************************************************
 *
 * FUNCTION:	EmPalmOS::HandleSystemCall
 *
 * DESCRIPTION: .
 *
 * PARAMETERS:	None.
 *
 * RETURNED:	Nothing.
 *
 ***********************************************************************/

extern "C" {
#include "selectors.h"
#include "trapArgs.c"
};

static uint32_t stackp;
static uint32_t stack[256];
static uint32_t stackt[256];
static uint32_t stacksel[256];

static trap_t allTraps[0x10000];

static int allTrapsInited = 0;

static void allTrapsInit(void) {
  uint32_t trap, selector, i;

  for (i = 0; i < 0x10000; i++) {
    allTraps[i] = {0};
    allTraps[i].name = "unknown";
  }

  for (i = 0; trapArgs[i].name; i++) {
    trap = trapArgs[i].trap;
    selector = trapArgs[i].selector;
    if (selector == (uint32_t)-1) {
      allTraps[trap] = trapArgs[i];
    } else {
      if (allTraps[trap].trap == 0) {
        allTraps[trap].trap = trap;
        allTraps[trap].name = "dispatch";
        allTraps[trap].capsel = selector < 256 ? 256 : selector + 256;
        allTraps[trap].numsel = 1;
        allTraps[trap].selectors = (trap_t *)calloc(allTraps[trap].capsel, sizeof(trap_t));
        allTraps[trap].selectors[selector] = trapArgs[i];
      } else {
        if (selector >= allTraps[trap].capsel) {
          allTraps[trap].capsel = selector + 256;
          if (allTraps[trap].selectors) {
            allTraps[trap].selectors = (trap_t *)realloc(allTraps[trap].selectors, allTraps[trap].capsel * sizeof(trap_t));
          } else {
            allTraps[trap].selectors = (trap_t *)calloc(allTraps[trap].capsel, sizeof(trap_t));
          }
        }
        allTraps[trap].selectors[selector] = trapArgs[i];
        allTraps[trap].numsel++;
      }
    }
  }
}

typedef union {
  uint32_t t;
  uint8_t c[4];
} creator_id_t;

static char *id2s(uint32_t ID, char *s) {
  creator_id_t id;

  id.t = ID;
  s[0] = id.c[3];
  s[1] = id.c[2];
  s[2] = id.c[1];
  s[3] = id.c[0];
  s[4] = 0;

  return s;
}

static char *param_value(uint32_t type, uint32_t ptr, uint32_t size, uint32_t value, char *aux, uint32_t len) {
  char str[128], sid[8];
  uint8_t red, green, blue;
  uint16_t etype;
  int16_t x, y, dx, dy;
  uint32_t j;
  int32_t sig;
  uint32_t usig;

  if (ptr) {
    if (value) {
      switch (type) {
        case T_VOID:
          snprintf(aux, len - 1, "0x%08X", value);
          break;
        case T_STR:
          for (j = 0; j < sizeof(str) - 1; j++) {
            str[j] = EmMemGet8(value + j);
            if (str[j] == 0) break;
          }
          str[j] = 0;
          snprintf(aux, len - 1, "\"%s\"", str);
          break;
        case T_RGB:
          red   = EmMemGet8(value + 1);
          green = EmMemGet8(value + 2);
          blue  = EmMemGet8(value + 3);
          snprintf(aux, len - 1, "rgb{%d,%d,%d}", red, green, blue);
        break;
        case T_EVT:
          etype = EmMemGet16(value);
          snprintf(aux, len - 1, "event{%d 0x%04X}", etype, etype);
          break;
        case T_RCT:
          x = EmMemGet16(value);
          y = EmMemGet16(value + 2);
          dx = EmMemGet16(value + 4);
          dy = EmMemGet16(value + 6);
          snprintf(aux, len - 1, "rect{%d,%d,%d,%d}", x, y, dx, dy);
          break;
        case T_SIG:
          sig = 0;
          if (!(value % 2))
          switch (size) {
            case 1: sig = (int8_t)EmMemGet8(value); break;
            case 2: sig = (int16_t)EmMemGet16(value); break;
            case 4: sig = (int32_t)EmMemGet32(value); break;
	    default: sig = 0; break;
          }
          snprintf(aux, len - 1, "int{%d}", sig);
          break;
        case T_USIG:
          usig = 0;
          if (!(value % 2))
          switch (size) {
            case 1: usig = EmMemGet8(value); break;
            case 2: usig = EmMemGet16(value); break;
            case 4: usig = EmMemGet32(value); break;
	    default: usig = 0; break;
          }
          snprintf(aux, len - 1, "uint{%u}", usig);
          break;
        case T_HEX:
          usig = 0;
          if (!(value % 2))
          switch (size) {
            case 1:
              usig = EmMemGet8(value);
              snprintf(aux, len - 1, "uint{0x%02X}", usig);
	      break;
            case 2:
              usig = EmMemGet16(value);
              snprintf(aux, len - 1, "uint{0x%04X}", usig);
	      break;
            case 4:
              usig = EmMemGet32(value);
              snprintf(aux, len - 1, "uint{0x%08X}", usig);
	      break;
          }
          break;
        case T_ID:
          usig = 0;
          if (!(value % 2))
          usig = EmMemGet32(value);
          id2s(usig, sid);
          snprintf(aux, len - 1, "'%s'", sid);
          break;
        default:
          snprintf(aux, len - 1, "type_%u", type);
          break;
      }
    } else {
      snprintf(aux, len - 1, "NULL");
    }
  } else {
    switch (type) {
      case T_SIG:  snprintf(aux, len - 1, "%d", (int32_t)value); break;
      case T_USIG: snprintf(aux, len - 1, "%u", value); break;
      case T_CHAR:
        if (value >= 0x20 && value < 0x7F) {
          snprintf(aux, len - 1, "'%c'", (char)value);
        } else {
          snprintf(aux, len - 1, "0x%02X", value);
        }
        break;
      case T_WCHR:
        if (value >= 0x20 && value < 0x7F) {
          snprintf(aux, len - 1, "'%c'", (char)value);
        } else {
          snprintf(aux, len - 1, "0x%04X", value);
        }
        break;
      case T_ID:
        id2s(value, sid);
        snprintf(aux, len - 1, "'%s'", sid);
        break;
      case T_HEX:
        switch (size) {
          case 1: snprintf(aux, len - 1, "0x%02X", value); break;
          case 2: snprintf(aux, len - 1, "0x%04X", value); break;
          case 4: snprintf(aux, len - 1, "0x%08X", value); break;
        }
        break;
    }
  }

  return aux;
}

static char *spaces(uint32_t n) {
  static char buf[256];
  uint32_t i;

  for (i = 0; i < n && i < 256; i++) {
    buf[i] = ' ';
  }
  buf[i] = 0;

  return buf;
}

static void dumpMemory(uint32_t addr, uint32_t offset, uint32_t len) {
  char sbuf[1024], abuf[32], *p, *e;
  uint32_t i, j, n;
  uint8_t b;

  p = sbuf;
  sprintf(p, "%08X: ", offset);
  n = strlen(p);
  p += n;
  e = p + 1024 - n - 4;

  for (i = 0, j = 0; i < len && p < e; i++) {
    if (j) {
      *p = ' ';
      p++;
    }
    b = EmMemGet8(addr + i);
    sprintf(p, "%02X", b);
    abuf[j] = (b >= 32 && b < 127) ? b : '.';
    p += 2;
    j++;
    if (j == 16) {
      *p = 0;
      abuf[j] = 0;
      fprintf(stderr, "%s %s\n", sbuf, abuf);
      p = sbuf;
      sprintf(p, "%08X: ", offset+i+1);
      n = strlen(p);
      p += n;
      e = p + 1024 - n - 4;
      j = 0;
    }
  }
  *p = 0;
  abuf[j] = 0;

  if (j) {
    for (; j < 16; j++) {
      *p++ = ' ';
      *p++ = ' ';
      *p++ = ' ';
      *p = 0;
    }
    fprintf(stderr, "%s %s\n", sbuf, abuf);
  }
}

static uint32_t log_dbID = 0;
static uint32_t log_dbRef = 0;
static FILE *log_f = NULL;
extern const char *traceSyscalls;

static char hex(uint8_t n) {
  n &= 0x0F;
  return n < 10 ? '0' + n : 'A' + n - 10;
}

static void log(char *fmt, ...) {
  char tmp[1024], buf[1024];
  uint32_t i, j;
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp)-1, fmt, ap);
  va_end(ap);

  for (i = 0, j = 0; tmp[i] && j < sizeof(buf)-5; i++) {
    if (tmp[i] >= 32) {
      buf[j++] = tmp[i];
    } else if (tmp[i+1]) {
      buf[j++] = '<';
      buf[j++] = hex((tmp[i] >> 4) & 0x0F);
      buf[j++] = hex(tmp[i] & 0x0F);
      buf[j++] = '>';
    }
  }
  buf[j] = 0;

  fprintf(log_f, "%s\n", buf);
}

static void print_params(trap_t *trap, uint32_t sp, char *buf, uint32_t len) {
  uint32_t value, idx, i;
  char aux[256];

  idx = 0;
  buf[0] = 0;
  for (i = 0; i < trap->nargs; i++) {
    if (trap->args[i].ptr) {
      value = EmMemGet32(sp + idx); idx += 4;
    } else {
      switch (trap->args[i].size) {
        case 1: value = EmMemGet8(sp + idx);  idx += 2; break;
        case 2: value = EmMemGet16(sp + idx); idx += 2; break;
        case 4: value = EmMemGet32(sp + idx); idx += 4; break;
        default: value = 0; break;
      }
    }
    param_value(trap->args[i].type, trap->args[i].ptr, trap->args[i].size, value, aux, sizeof(aux));
    if (i > 0) strncat(buf, ", ", len - strlen(buf) - 1);
    strncat(buf, aux, len - strlen(buf) - 1);
  }
}

void trapReturnHook(uint32_t pc, uint32_t sp) {
  char buf[1024], rbuf[256];
  uint32_t name, value, selector;
  uint16_t trap, i;
  char *s;

  if (stackp && pc == stack[stackp-1]) {
    value = 0;
    stackp--;
    trap = stackt[stackp];
    selector = stacksel[stackp];
    rbuf[0] = 0;

    if (allTraps[trap].rsize > 0) {
      strcpy(rbuf, ": ");
      if (allTraps[trap].rtype == T_VOID || allTraps[trap].rtype == T_STR) {
        value = gCPU->GetRegister(e68KRegID_A0);
      } else {
        value = gCPU->GetRegister(e68KRegID_D0);
      }
      switch (allTraps[trap].rsize) {
        case 1: value &= 0xFF; break;
        case 2: value &= 0xFFFF; break;
      }
      if (log_f) param_value(allTraps[trap].rtype, allTraps[trap].rptr, allTraps[trap].rsize, value, &rbuf[2], sizeof(rbuf) - 2);
    }

    if (log_f) {
      if (allTraps[trap].numsel == 0) {
        print_params(&allTraps[trap], sp, buf, sizeof(buf));
        log("0x%08X: trap 0x%04X    %s%s(%s)%s", pc, trap, spaces(stackp), allTraps[trap].name, buf, rbuf);
      } else {
        print_params(&allTraps[trap].selectors[selector], sp, buf, sizeof(buf));
	s = allTraps[trap].selectors[selector].name;
	if (s == NULL) s = "unknown";
        log("0x%08X: trap 0x%04X.%2d %s%s(%s)%s", pc, trap, selector, spaces(stackp), s, buf, rbuf);
      }
    }

    switch (trap) {
      case sysTrapDmDatabaseInfo:
        // Err DmDatabaseInfo(UInt16 cardNo, LocalID dbID, Char *nameP, ...
        if (traceSyscalls && log_f == NULL && log_dbID == 0 && value == 0) {
          name = EmMemGet32(sp + 6);
          if (name) {
            for (i = 0; i < sizeof(buf) - 1; i++) {
              buf[i] = EmMemGet8(name + i);
              if (buf[i] == 0) break;
            }
            buf[i] = 0;
            if (strcmp(buf, traceSyscalls) == 0) {
              log_dbID = EmMemGet32(sp + 2);
              fprintf(stdout, "\nMonitoring dbID 0x%08X for \"%s\"\n", log_dbID, traceSyscalls);
            }
          }
        }
        break;
      case sysTrapDmOpenDatabase:
        // DmOpenRef DmOpenDatabase(UInt16 cardNo, LocalID dbID, UInt16 mode)
        if (log_f == NULL && log_dbRef == 0 && log_dbID != 0 && value != 0) {
          if (EmMemGet32(sp + 2) == log_dbID) {
            log_dbRef = value;
            fprintf(stdout, "Monitoring dbRef 0x%08X for dbID 0x%08X\n", log_dbRef, log_dbID);
          }
        }
        break;
      case sysTrapSysAppStartup:
        // Err SysAppStartup(SysAppInfoPtr *appInfoPP, MemPtr *prevGlobalsP, MemPtr *globalsPtrP)
        if (log_f == NULL && log_dbRef != 0 && value == 0) {
          value = EmMemGet32(sp);    // SysAppInfoType **
          value = EmMemGet32(value); // SysAppInfoType *
          if (EmMemGet32(value + 16) == log_dbRef) {
            fprintf(stdout, "Logging system calls for dbRef 0x%08X dbID 0x%08X\n", log_dbRef, log_dbID);
            log_f = fopen("syscalls.txt", "w");
          }
        }
        break;
      case sysTrapSysAppExit:
        if (log_f) {
          fprintf(stdout, "Stop logging system calls for dbRef 0x%08X dbID 0x%08X\n", log_dbRef, log_dbID);
          fclose(log_f);
          log_dbID = 0;
          log_dbRef = 0;
          log_f = NULL;
        }
        break;
    }
  }
}

static void trapHook(uint32_t pc, uint32_t sp, uint16_t trap, uint32_t nextpc) {
  char *s, buf[1024];
  uint32_t selector;

  if (!allTrapsInited) {
    allTrapsInit();
    allTrapsInited = 1;
  }

  switch (trap) {
    case sysTrapHwrDisableDataWrites:
    case sysTrapHwrEnableDataWrites:
    case sysTrapHwrDoze:
    case sysTrapHwrDelay:
    case sysTrapHwrDockStatus:
    case sysTrapSysDoze:
    case sysTrapSysTimerWrite:
    case sysTrapSysEvGroupSignal:
    case sysTrapSysEvGroupWait:
    case sysTrapSysDisableInts:
    case sysTrapSysRestoreStatus:
    case sysTrapSysResSemaphoreReserve:
    case sysTrapSysResSemaphoreRelease:
    case sysTrapSysSemaphoreWait:
    case sysTrapSysSemaphoreSignal:
    case sysTrapSysTaskSwitching:
    case sysTrapSysGetAppInfo:
    case sysTrapMemSemaphoreReserve:
    case sysTrapMemSemaphoreRelease:
    case sysTrapAlmDisplayAlarm:
    case sysTrapAttnDoEmergencySpecialEffects:
    case sysTrapEvtDequeueKeyEvent:
    case sysTrapEvtGetSysEvent:
    case sysTrapHwrIRQ6Handler:
    case sysTrapSysKernelClockTick:
      break;

    default:
      selector = gCPU->GetRegister(e68KRegID_D2);
      if (log_f) {
        if (allTraps[trap].numsel == 0) {
          print_params(&allTraps[trap], sp, buf, sizeof(buf));
          log("0x%08X: trap 0x%04X    %s%s(%s) ...", pc, trap, spaces(stackp), allTraps[trap].name, buf);
        } else {
          print_params(&allTraps[trap].selectors[selector], sp, buf, sizeof(buf));
	  s = allTraps[trap].selectors[selector].name;
	  if (s == NULL) s = "unknown";
          log("0x%08X: trap 0x%04X.%2d %s%s(%s) ...", pc, trap, selector, spaces(stackp), s, buf);
        }
      }
      stackt[stackp] = trap;
      stacksel[stackp] = selector;
      stack[stackp++] = nextpc;
      break;
  }
}

Bool EmPalmOS::HandleSystemCall(Bool fromTrap) {
    // ======================================================================
    //	First things first: if we need to break execution on the next call
    //	to a system function, make sure that happens.
    // ======================================================================

    EmAssert(gSession);
    EmAssert(gCPU68K);

    // If the system call is being made by a TRAP $F, the PC has already
    // been bumped past the opcode.  If being made with a JSR via the
    // SYS_TRAP_FAST macro, the PC has not been adjusted.  Determine a
    // "pcAdjust" value that allows us to get back to the start of the
    // instruction that got us here.

    int pcAdjust = fromTrap ? 2 : 0;

    // ======================================================================
    //	Determine what ROM function is about to be called, and determine
    //	the method by which it is being called.
    // ======================================================================

    SystemCallContext context;
    Bool gotFunction = GetSystemCallContext(gCPU->GetPC() - pcAdjust, context);

    // ======================================================================
    //	Validate the address for the ROM function we're about to call.
    // ======================================================================

    if (!gotFunction) {
        // We should never get here.  context.fError should always equal
        // one of those two error codes, and those two functions we call
        // should never return (they should throw exceptions).

        EmAssert(false);
    }

#ifdef ENABLE_DEBUGGER
    gDebugger.NotifyTrap(context.fTrapWord);
#endif

    CEnableFullAccess munge;

    UInt32 memSemaphoreIDP = EmLowMem_GetGlobal(memSemaphoreID);
    EmAliascj_xsmb<PAS> memSemaphoreID(memSemaphoreIDP);

    if (!gSession->IsNested() && memSemaphoreID.xsmuse == 0) {
        switch (context.fTrapWord) {
            case sysTrapHwrIRQ1Handler:
            case sysTrapHwrIRQ2Handler:
            case sysTrapHwrIRQ3Handler:
            case sysTrapHwrIRQ4Handler:
            case sysTrapHwrIRQ5Handler:
            case sysTrapHwrIRQ6Handler:
                DispatchNextEvent();
        }
    }

    // ======================================================================
    // If this trap is patched, let the patch handler handle the patch.
    // ======================================================================

    CallROMType result = EmPatchMgr::HandleSystemCall(context);

    // ======================================================================
    //	If we completely handled the function in head and tail patches, tell
    //	the profiler that we exited the function and get out of here.
    // ======================================================================

    trapHook(gCPU->GetPC() - 2, gCPU->GetSP(), context.fTrapWord, context.fNextPC);

    if (result == kSkipROM) {
        gCPU->SetPC(context.fNextPC);

        // Return true to say that everything has been handled.
        return true;
    }

    return false;
}

void EmPalmOS::QueuePenEvent(PenEvent evt) {
    if (!gSession->IsPowerOn()) return;

    if (penEventQueueIncoming.GetFree() == 0) penEventQueueIncoming.Get();

    penEventQueueIncoming.Put(evt);
}

void EmPalmOS::QueueKeyboardEvent(KeyboardEvent evt) {
    if (!gSession->IsPowerOn()) return;

    if (keyboardEventQueueIncoming.GetFree() == 0) keyboardEventQueueIncoming.Get();

    keyboardEventQueueIncoming.Put(evt);
}

bool EmPalmOS::HasPenEvent() { return penEventQueue.GetUsed() != 0; }

bool EmPalmOS::HasKeyboardEvent() { return keyboardEventQueue.GetUsed() != 0; }

PenEvent EmPalmOS::PeekPenEvent() { return HasPenEvent() ? penEventQueue.Peek() : PenEvent(); }

bool EmPalmOS::DispatchNextEvent() {
    uint64 systemCycles = gSession->GetSystemCycles();

    if (systemCycles - lastEventPromotedAt < MIN_CYCLES_BETWEEN_EVENTS ||
        !gSystemState.IsUIInitialized())
        return false;

    if (DispatchPenEvent() || DispatchKeyboardEvent()) {
        lastEventPromotedAt = systemCycles;
        Wakeup();

        return true;
    } else if (postNilEvent) {
        EvtWakeup();
        postNilEvent = false;
    }

    return false;
}

bool EmPalmOS::DispatchKeyboardEvent() {
    if (keyboardEventQueueIncoming.GetUsed() == 0) return false;

    if (keyboardEventQueue.GetFree() == 0) keyboardEventQueue.Get();
    keyboardEventQueue.Put(keyboardEventQueueIncoming.Get());

    return true;
}

bool EmPalmOS::DispatchPenEvent() {
    if (penEventQueueIncoming.GetUsed() == 0) return false;

    if (penEventQueue.GetFree() == 0) penEventQueue.Get();
    penEventQueue.Put(penEventQueueIncoming.Get());

    return true;
}

void EmPalmOS::Wakeup() {
    if (gSystemState.OSMajorVersion() >= 4) {
        EvtWakeupWithoutNilEvent();
    } else {
        EvtWakeup();
        EmLowMem::ClearNilEvent();
    }
}

void EmPalmOS::ClearQueues() {
    penEventQueue.Clear();
    keyboardEventQueue.Clear();
    penEventQueueIncoming.Clear();
    keyboardEventQueueIncoming.Clear();

    lastEventPromotedAt = gSession->GetSystemCycles();
}

void EmPalmOS::InjectSystemEvent(CallROMType& callROM) {
    callROM = kExecuteROM;

    // Set the return value (Err) to zero in case we return
    // "true" (saying that we handled the trap).

    m68k_dreg(regs, 0) = 0;

    // If the low-memory global "idle" is true, then we're being
    // called from EvtGetEvent or EvtGetPen, in which case we
    // need to check if we need to post some events.

    if (EmLowMem::GetEvtMgrIdle()) {
        // If we're in the middle of calling a Palm OS function ourself,
        // and we are somehow at the point where the system is about to
        // doze, then just return now.  Don't let it doze!  Interrupts are
        // off, and HwrDoze will never return!

        if (gSession->IsNested()) {
            m68k_dreg(regs, 0) = 4;
            callROM = kSkipROM;
            return;
        }

        EmAssert(gSession);

        if (HasKeyboardEvent()) {
            KeyboardEvent evt = keyboardEventQueue.Get();

            if (evt.GetKey() != 0) EvtEnqueueKey(evt.GetKey(), 0, 0);
        } else if (HasPenEvent()) {
            PenEvent evt = penEventQueue.Get();
            PointType point;

            if (evt.isPenDown()) {
                uint32 scaleNum;
                uint32 scaleDen;

                gSession->GetDevice().DigitizerScale(scaleNum, scaleDen);

                point.x = (evt.getX() * scaleNum) / scaleDen;
                point.y = (evt.getY() * scaleNum) / scaleDen;

                if (gSession->GetDevice().HasCustomDigitizerTransform()) {
                    PenScreenToRaw(&point);
                } else {
                    TransformPenCoordinates(point.x, point.y);
                }
            } else {
                point.x = point.y = -1;
            }

            EvtEnqueuePenPoint(&point);
        }
    }
}

void EmPalmOS::InjectUIEvent() {
    if (dbForLaunch != 0) SysUIAppSwitch(0, dbForLaunch, sysAppLaunchCmdNormalLaunch, 0);
    dbForLaunch = 0;
}

bool EmPalmOS::LaunchAppByName(const string& name) {
    if (!gSystemState.IsUIInitialized() || gSession->IsCpuStopped()) return false;

    LocalID id = DmFindDatabase(0, name.c_str());
    if (id == 0) return false;

    // We need to make sure that SysUIAppSwitch is called on the UI thread. EvtGetEvent is
    // hooked to do the actual call to InjectUIEvent, and we just inject a null event here
    // in order to make sure that the event loop is cycled.
    //
    // EDIT: Actually, after 5da779c9515a I am not sure that this is strictly necessary, the
    // crashes may also have been caused by my own blunder. Nevertheless, it is probably a good
    // idea to do the actual call to SysUIAppSwitch in a well defined place.
    dbForLaunch = id;
    postNilEvent = true;

    return true;
}

bool EmPalmOS::HasPendingAppForLaunch() { return dbForLaunch != 0; }
