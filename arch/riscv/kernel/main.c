#include "sched.h"
#include "stdio.h"
#include "sched.h"
#include "mm.h"
#include "virtio.h"

int start_kernel() {
  puts("ZJU OSLAB 7 3210105812 3210106333 居圣桐 詹含蓓\n");
  
  slub_init();
  task_init();
  plic_init();
  virtio_disk_init();

  // 设置第一次时钟中断
  asm volatile("ecall");
  
  call_first_process();
  dead_loop();
  return 0;
}
