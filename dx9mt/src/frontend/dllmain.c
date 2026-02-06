#include <windows.h>

#include "dx9mt/log.h"
#include "dx9mt/runtime.h"

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
