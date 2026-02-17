#include <windows.h>

#include "dx9mt/log.h"
#include "dx9mt/runtime.h"

static LONG WINAPI dx9mt_crash_handler(EXCEPTION_POINTERS *ep) {
  if (ep && ep->ExceptionRecord &&
      ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    DWORD tid = GetCurrentThreadId();
    DWORD eip = ep->ContextRecord->Eip;
    DWORD addr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
    DWORD *ebp;
    int i;

    /* BSShader factory NULL return crash fix:
     * Game's factory at 0xB55560 returns NULL when shader_tbl[29] is empty
     * and TLS slot 0 is uninitialized on the IO loading thread.
     * Caller does multiple vtable dereferences on the NULL result:
     *   0xB57AA9: mov eax,[esi] / mov edx,[eax+0x50] / add esp,4 /
     *             push edi / call edx / mov eax,[esi] / Release / call edx
     *   0xB57ABF: cmp eax,0x012021b8
     *   0xB57AC4: mov eax,[esi] / Release / sete / call edx
     *   0xB57AD2: cmp eax,0x01200508 / sete al / test al,al
     * Skip ALL vtable calls in one go: 0xB57AA9 -> 0xB57AD2.
     * Net stack effect of skipped code: add esp,4 only. */
    if (eip == 0x00B57AA9 && ep->ContextRecord->Esi == 0x00000000) {
      dx9mt_logf("PATCH",
                 "BSShader factory NULL skip tid=0x%04lx: eip=0x%08lx -> "
                 "0x00B57AD2 (skip all vtable calls, esi=0)",
                 (unsigned long)tid, (unsigned long)eip);
      ep->ContextRecord->Eip = 0x00B57AD2; /* past ALL vtable calls */
      ep->ContextRecord->Esp += 4;          /* skipped add esp,4 */
      ep->ContextRecord->Eax = 0;           /* cmp will fail -> sete=0 */
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* Fallback: any mov eax,[esi] crash with esi=0 near the same function.
     * Handles additional vtable dereferences we haven't mapped yet. */
    if (ep->ContextRecord->Esi == 0x00000000 &&
        eip >= 0x00B57A00 && eip < 0x00B57C00 &&
        !IsBadReadPtr((void *)eip, 2) &&
        *(unsigned char *)eip == 0x8b &&
        *((unsigned char *)eip + 1) == 0x06) {
      /* Scan forward for 'call edx' (ff d2) and skip past it */
      unsigned char *p = (unsigned char *)(eip + 2);
      int scan;
      DWORD skip_target = 0;
      for (scan = 0; scan < 30; scan++) {
        if (p[scan] == 0xff && p[scan + 1] == 0xd2) {
          skip_target = (DWORD)(p + scan + 2);
          break;
        }
      }
      if (skip_target) {
        dx9mt_logf("PATCH",
                   "BSShader NULL fallback tid=0x%04lx: eip=0x%08lx -> "
                   "0x%08lx (skip to past call edx)",
                   (unsigned long)tid, (unsigned long)eip,
                   (unsigned long)skip_target);
        ep->ContextRecord->Eip = skip_target;
        ep->ContextRecord->Eax = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }

    dx9mt_logf("CRASH",
               "ACCESS VIOLATION tid=0x%04lx eip=0x%08lx target_addr=0x%08lx",
               (unsigned long)tid, (unsigned long)eip, (unsigned long)addr);
    dx9mt_logf("CRASH",
               "  EAX=0x%08lx EBX=0x%08lx ECX=0x%08lx EDX=0x%08lx",
               (unsigned long)ep->ContextRecord->Eax,
               (unsigned long)ep->ContextRecord->Ebx,
               (unsigned long)ep->ContextRecord->Ecx,
               (unsigned long)ep->ContextRecord->Edx);
    dx9mt_logf("CRASH",
               "  ESI=0x%08lx EDI=0x%08lx EBP=0x%08lx ESP=0x%08lx",
               (unsigned long)ep->ContextRecord->Esi,
               (unsigned long)ep->ContextRecord->Edi,
               (unsigned long)ep->ContextRecord->Ebp,
               (unsigned long)ep->ContextRecord->Esp);

    /* dump code bytes around crash */
    if (!IsBadReadPtr((void *)(eip - 24), 48)) {
      unsigned char *code = (unsigned char *)(eip - 24);
      dx9mt_logf("CRASH",
                 "  code[-24..-9]: %02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x",
                 code[0], code[1], code[2], code[3], code[4], code[5], code[6],
                 code[7], code[8], code[9], code[10], code[11], code[12],
                 code[13], code[14], code[15]);
      dx9mt_logf("CRASH",
                 "  code[-8..+15]: %02x %02x %02x %02x %02x %02x %02x %02x "
                 "[%02x] %02x %02x %02x %02x %02x %02x %02x",
                 code[16], code[17], code[18], code[19], code[20], code[21],
                 code[22], code[23], code[24], code[25], code[26], code[27],
                 code[28], code[29], code[30], code[31]);
      dx9mt_logf("CRASH",
                 "  code[+8..+23]: %02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x",
                 code[32], code[33], code[34], code[35], code[36], code[37],
                 code[38], code[39], code[40], code[41], code[42], code[43],
                 code[44], code[45], code[46], code[47]);
    }

    /* dump stack from ESP */
    {
      DWORD *sp = (DWORD *)ep->ContextRecord->Esp;
      if (!IsBadReadPtr(sp, 64)) {
        dx9mt_logf("CRASH", "  stack[esp+0..+15]:");
        for (i = 0; i < 16; i += 4) {
          dx9mt_logf("CRASH", "    +%02x: 0x%08lx 0x%08lx 0x%08lx 0x%08lx",
                     i * 4, (unsigned long)sp[i], (unsigned long)sp[i + 1],
                     (unsigned long)sp[i + 2], (unsigned long)sp[i + 3]);
        }
      }
    }

    /* dump module map */
    {
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((void *)eip, &mbi, sizeof(mbi))) {
        dx9mt_logf("CRASH", "  crash module base=0x%08lx size=0x%08lx",
                   (unsigned long)mbi.AllocationBase,
                   (unsigned long)mbi.RegionSize);
      }
    }

    /* dump the factory function that returned NULL (call target from disasm) */
    {
      unsigned char *factory = (unsigned char *)0x00B55560;
      if (!IsBadReadPtr(factory, 64)) {
        dx9mt_logf("CRASH", "  factory@0x00B55560[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                   factory[0], factory[1], factory[2], factory[3],
                   factory[4], factory[5], factory[6], factory[7],
                   factory[8], factory[9], factory[10], factory[11],
                   factory[12], factory[13], factory[14], factory[15]);
        dx9mt_logf("CRASH", "  factory@0x00B55560[16..31]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                   factory[16], factory[17], factory[18], factory[19],
                   factory[20], factory[21], factory[22], factory[23],
                   factory[24], factory[25], factory[26], factory[27],
                   factory[28], factory[29], factory[30], factory[31]);
        dx9mt_logf("CRASH", "  factory@0x00B55560[32..47]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                   factory[32], factory[33], factory[34], factory[35],
                   factory[36], factory[37], factory[38], factory[39],
                   factory[40], factory[41], factory[42], factory[43],
                   factory[44], factory[45], factory[46], factory[47]);
        dx9mt_logf("CRASH", "  factory@0x00B55560[48..63]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                   factory[48], factory[49], factory[50], factory[51],
                   factory[52], factory[53], factory[54], factory[55],
                   factory[56], factory[57], factory[58], factory[59],
                   factory[60], factory[61], factory[62], factory[63]);
      }
    }

    /* dump EDI object fields (the 'this' pointer at crash) */
    {
      DWORD *obj = (DWORD *)ep->ContextRecord->Edi;
      if (!IsBadReadPtr(obj, 256)) {
        dx9mt_logf("CRASH", "  EDI obj vtable=0x%08lx", (unsigned long)obj[0]);
        dx9mt_logf("CRASH", "  EDI obj[0xB8..0xCC]: 0x%08lx 0x%08lx 0x%08lx 0x%08lx 0x%08lx",
                   (unsigned long)obj[0xB8/4], (unsigned long)obj[0xBC/4],
                   (unsigned long)obj[0xC0/4], (unsigned long)obj[0xC4/4],
                   (unsigned long)obj[0xC8/4]);
      }
    }

    /* dump the global variable the factory checks */
    {
      DWORD *global_ptr = (DWORD *)0x011F9508;
      if (!IsBadReadPtr(global_ptr, 4)) {
        DWORD global_val = *global_ptr;
        dx9mt_logf("CRASH", "  factory global [0x011F9508]=0x%08lx",
                   (unsigned long)global_val);
        if (global_val != 0 && !IsBadReadPtr((void *)global_val, 256)) {
          DWORD *tbl = (DWORD *)global_val;
          int j;
          for (j = 0; j < 64; j += 8) {
            dx9mt_logf("CRASH", "  mgr[%02x..%02x]: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                       j*4, (j+7)*4,
                       (unsigned long)tbl[j+0], (unsigned long)tbl[j+1],
                       (unsigned long)tbl[j+2], (unsigned long)tbl[j+3],
                       (unsigned long)tbl[j+4], (unsigned long)tbl[j+5],
                       (unsigned long)tbl[j+6], (unsigned long)tbl[j+7]);
          }
        }
      }
    }

    /* read all 35 shader table entries individually */
    {
      DWORD *tbl = (DWORD *)0x011F9548;
      dx9mt_logf("CRASH", "  shader_tbl[0x011F9548] entries (non-zero marked):");
      if (!IsBadReadPtr(tbl, 4)) {
        /* read entries one at a time to avoid any loop/access issues */
        dx9mt_logf("CRASH", "  tbl[ 0..4 ]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[0],(unsigned long)tbl[1],(unsigned long)tbl[2],
          (unsigned long)tbl[3],(unsigned long)tbl[4]);
        dx9mt_logf("CRASH", "  tbl[ 5..9 ]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[5],(unsigned long)tbl[6],(unsigned long)tbl[7],
          (unsigned long)tbl[8],(unsigned long)tbl[9]);
        dx9mt_logf("CRASH", "  tbl[10..14]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[10],(unsigned long)tbl[11],(unsigned long)tbl[12],
          (unsigned long)tbl[13],(unsigned long)tbl[14]);
        dx9mt_logf("CRASH", "  tbl[15..19]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[15],(unsigned long)tbl[16],(unsigned long)tbl[17],
          (unsigned long)tbl[18],(unsigned long)tbl[19]);
        dx9mt_logf("CRASH", "  tbl[20..24]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[20],(unsigned long)tbl[21],(unsigned long)tbl[22],
          (unsigned long)tbl[23],(unsigned long)tbl[24]);
        dx9mt_logf("CRASH", "  tbl[25..29]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[25],(unsigned long)tbl[26],(unsigned long)tbl[27],
          (unsigned long)tbl[28],(unsigned long)tbl[29]);
        dx9mt_logf("CRASH", "  tbl[30..34]=%08lx %08lx %08lx %08lx %08lx",
          (unsigned long)tbl[30],(unsigned long)tbl[31],(unsigned long)tbl[32],
          (unsigned long)tbl[33],(unsigned long)tbl[34]);
      }
      /* TLS */
      if (!IsBadReadPtr((void *)0x0126FD98, 4)) {
        DWORD tls_idx = *(DWORD *)0x0126FD98;
        DWORD tls_val = 0;
        dx9mt_logf("CRASH", "  TLS index=%lu", (unsigned long)tls_idx);
        /* read TLS slot value for this thread via TlsGetValue */
        tls_val = (DWORD)TlsGetValue(tls_idx);
        dx9mt_logf("CRASH", "  TLS slot[%lu]=0x%08lx (GetLastError=%lu)",
                   (unsigned long)tls_idx, (unsigned long)tls_val,
                   (unsigned long)GetLastError());
        if (tls_val != 0 && !IsBadReadPtr((void *)tls_val, 0x2B8)) {
          DWORD *tls_data = (DWORD *)tls_val;
          dx9mt_logf("CRASH", "  TLS data[0x2B0..0x2BC]: %08lx %08lx %08lx",
                     (unsigned long)tls_data[0x2B0/4],
                     (unsigned long)tls_data[0x2B4/4],
                     (unsigned long)tls_data[0x2B8/4]);
        }
      }
    }

    ebp = (DWORD *)ep->ContextRecord->Ebp;
    for (i = 0; i < 20 && ebp; i++) {
      DWORD ret_addr;
      if (IsBadReadPtr(ebp, 8))
        break;
      ret_addr = ebp[1];
      dx9mt_logf("CRASH", "  frame[%d] ret=0x%08lx", i, (unsigned long)ret_addr);
      ebp = (DWORD *)ebp[0];
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  char exe_path[MAX_PATH] = {0};
  const char *cmdline = NULL;
  DWORD pid = GetCurrentProcessId();
  DWORD exe_len = 0;

  (void)reserved;
  exe_len = GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof(exe_path));
  cmdline = GetCommandLineA();
  if (exe_len == 0 || exe_len >= (DWORD)sizeof(exe_path)) {
    lstrcpynA(exe_path, "<unknown>", sizeof(exe_path));
  }
  if (!cmdline) {
    cmdline = "<unknown>";
  }

  switch (reason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(instance);
    dx9mt_log_init();
    AddVectoredExceptionHandler(1, dx9mt_crash_handler);
    dx9mt_logf("dll", "PROCESS_ATTACH pid=%lu exe=%s cmd=%s",
               (unsigned long)pid, exe_path, cmdline);
    break;
  case DLL_PROCESS_DETACH:
    dx9mt_logf("dll", "PROCESS_DETACH pid=%lu exe=%s", (unsigned long)pid,
               exe_path);
    dx9mt_runtime_shutdown();
    break;
  default:
    break;
  }

  return TRUE;
}
