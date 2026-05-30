#include <hypercalls/hp_open.h>
#include <mm/kmalloc.h>
#include <mm/translate.h>
#include <syscalls/sys_execve.h>
#include <utils/string.h>

#define MSR_STAR 0xc0000081 /* legacy mode SYSCALL target */
#define MSR_LSTAR 0xc0000082 /* long mode SYSCALL target */
#define MSR_CSTAR 0xc0000083 /* compat mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084

/* Guest内核的入口函数，Hypervisor会加载Guest内核二进制文件到内存，并给Guest内核传递用户程序的参数，
复制到虚拟机的栈中，使得Guest内核可以像普通 C 程序一样访问 argc 和 argv，然后进入执行循环, vCPU会逐条执行Guest的指令，
等待Guest退出 ，退出后可以通过 vm->run 结构体获取退出原因和相关信息，根据 vCPU 的退出原因进行处理*/

// 注册 syscall 处理函数，设置 MSR 寄存器，使得当 Guest 内核执行 syscall 指令时能够跳转到我们指定的 syscall_entry 函数来处理系统调用
int register_syscall() {
  asm(
    // 1. 设置 STAR MSR (用户态/内核态段选择子)
    "xor rax, rax;"
    "mov rdx, 0x00200008;"    // 内核代码段: 0x08, 用户代码段: 0x1b, STAR 的高 32 位存储内核代码段选择子，低 32 位存储用户代码段选择子
    "mov ecx, %[msr_star];"   // MSR_STAR = 0xc0000081
    "wrmsr;"

    // 2. 设置 SYSCALL_MASK (执行 syscall 时清除的 EFLAGS 位)
    "mov eax, %[fmask];"      // 0x3f7fd5
    "xor rdx, rdx;"
    "mov ecx, %[msr_fmask];"  // MSR_SYSCALL_MASK = 0xc0000084
    "wrmsr;"

    // 3. 设置 LSTAR MSR (syscall 的目标地址)
    "lea rax, [rip + syscall_entry];"  // syscall_entry 的地址，kernel/syscalls/syscall_entry.S，由global导出，最后链接到内核镜像中
    "mov rdx, %[base] >> 32;"          // 高 32 位
    "mov ecx, %[msr_syscall];"         // MSR_LSTAR = 0xc0000082
    "wrmsr;"
    : // 无输出
    : [msr_star]"i"(MSR_STAR),
      [fmask]"i"(0x3f7fd5),
      [msr_fmask]"i"(MSR_SYSCALL_MASK),
      [base]"i"(KERNEL_BASE_OFFSET),
      [msr_syscall]"i"(MSR_LSTAR)
    : "rax", "rdx", "rcx");
  return 0;
}

// 切换到用户态，准备用户程序的执行环境，包括设置分页、初始化内存分配器、注册系统调用处理函数，并最终调用 sys_execve 来执行用户程序
void switch_user(uint64_t argc, char *argv[]) {
  int total_len = (argv[argc - 1] + strlen(argv[argc - 1]) + 1) - (char*) argv;
  /* temporary area for putting user-accessible data */
  char *s = kmalloc(total_len, MALLOC_PAGE_ALIGN);
  uint64_t sp = physical(s);
  add_trans_user((void*) sp, (void*) sp, PROT_RW); /* sp is page aligned */

  /* copy strings and argv onto user-accessible area */
  for(int i = 0; i < argc; i++)
    argv[i] = (char*) (argv[i] - (char*) argv + sp);
  memcpy(s, argv, total_len);
  sys_execve(argv[0], (char**) sp, (char**) (sp + argc * sizeof(char*)));
}

int kernel_main(void* addr, uint64_t len, uint64_t argc, char *argv[]) {
  init_pagetable();
  /* new paging enabled! */
  init_allocator((void*) ((uint64_t) addr | KERNEL_BASE_OFFSET), len);
  if(register_syscall() != 0) return 1;
  switch_user(argc, argv);
  return 0;
}
