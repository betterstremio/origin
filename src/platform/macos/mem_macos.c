/* Memory on macOS: what the machine can still give (free, inactive and purgeable pages: what it hands out before it
   compresses or swaps), and this process's resident size. */
#include "platform/mem.h"
#include <mach/mach.h>
#include <stdlib.h>

int plat_mem_free_mb(void) {
  const char *e = getenv("SB_MEMFREE"); /* development: act like a TV short of memory */
  if (e)
    return atoi(e);
  vm_statistics64_data_t v;
  mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
  vm_size_t page;
  if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
      host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &n) != KERN_SUCCESS)
    return -1;
  return (int)(((uint64_t)v.free_count + v.inactive_count + v.purgeable_count) * page >> 20);
}
void plat_mem_self(int *rssMb, int *swapMb) {
  mach_task_basic_info_data_t i;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  *rssMb = task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&i, &n) == KERN_SUCCESS
               ? (int)(i.resident_size >> 20)
               : -1;
  *swapMb = -1;
}
