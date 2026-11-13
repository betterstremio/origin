/* Memory on Windows: what the machine can still give, and this process's working set. */
#include "platform/mem.h"
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#include <psapi.h>
#include <stdlib.h>

int plat_mem_free_mb(void) {
  const char *e = getenv("SB_MEMFREE");
  if (e)
    return atoi(e); /* development: act like a device short of memory */
  MEMORYSTATUSEX m;
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? (int)(m.ullAvailPhys >> 20) : -1;
}
int win_private_mb(void) { /* memory only this process holds (the working set also counts the DLLs' shared pages) */
  PROCESS_MEMORY_COUNTERS_EX c;
  c.cb = sizeof c;
  return GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&c, sizeof c)
             ? (int)(c.PrivateUsage >> 20)
             : -1;
}
void plat_mem_self(int *rssMb, int *swapMb) {
  PROCESS_MEMORY_COUNTERS_EX c;
  c.cb = sizeof c;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&c, sizeof c)) {
    *rssMb = (int)(c.WorkingSetSize >> 20);
    *swapMb = -1;
  } /* (Windows does not say how much of it is paged out) */
  else {
    *rssMb = -1;
    *swapMb = -1;
  }
}
