#include <fcntl.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "debug.h"
#include "definition.h"
#include "hypercall.h"

#define PS_LIMIT (0x200000) // page size limit, 2MB, map the first 2MB memory to guest, which is enough for our kernel and user program
#define KERNEL_STACK_SIZE (0x4000) // 内核栈 16KB
/*
 * setup_paging() and init_pagetable() in kernel/mm/translate.c uses 5 pages in total
 */
#define PAGE_TABLE_SIZE (0x5000) // 页表占用 5页 = 20KB
#define MAX_KERNEL_SIZE (PS_LIMIT - PAGE_TABLE_SIZE - KERNEL_STACK_SIZE) // 内核最大占用空间大约2MB, 2MB - 20KB - 16KB = 2MB - 36KB
#define MEM_SIZE (PS_LIMIT * 0x2) // 总共 4MB 物理内存
/*
地址范围	大小	内容
0 ~ MAX_KERNEL_SIZE	~2MB	内核代码及数据
MAX_KERNEL_SIZE ~ +0x5000	20KB	页表（PML4/PDP/PD）
之后 ~0x4000	16KB	内核栈
2MB ~ 4MB	2MB	空闲内存（用户程序用）

物理地址
0x000000 ┌─────────────────┐
         │                 │
         │  内核代码        │ ← kernel.bin (从地址 0 开始)
         │  (kernel.bin)   │
         │                 │
         │   ...           │
         │                 │
0x1F5000 ├─────────────────┤ ← MAX_KERNEL_SIZE (约2MB - 20KB)
         │  PML4 页 (4KB)  │ ← pml4_addr
0x1F6000 ├─────────────────┤
         │  PDP 页 (4KB)   │ ← pdp_addr
0x1F7000 ├─────────────────┤
         │  PD 页 (4KB)    │ ← pd_addr
0x1F8000 ├─────────────────┤
         │  内核栈 (16KB)  │
0x1FC000 ├─────────────────┤
         │                 │
         │  空闲内存        │ ← 内核堆起始，用户程序、用户堆等都从这里开始分配
         │                 │
0x200000 ├─────────────────┤ ← PS_LIMIT (2MB)
         │                 │
         │  更多空闲内存    │ ← 用户程序、堆等
         │                 │
0x400000 └─────────────────┘ ← MEM_SIZE (4MB)
*/

// 把内核二进制文件读进内存 并返回内容和大小
void read_file(const char *filename, uint8_t** content_ptr, size_t* size_ptr) {
  FILE *f = fopen(filename, "rb");
  if(f == NULL) error("Open file '%s' failed.\n", filename);
  // 定位到文件末尾，获取文件大小
  if(fseek(f, 0, SEEK_END) < 0) pexit("fseek(SEEK_END)");
  // ftell 函数返回当前文件位置指针相对于文件开头的偏移量，即文件大小
  size_t size = ftell(f);
  if(size == 0) error("Empty file '%s'.\n", filename);
  if(fseek(f, 0, SEEK_SET) < 0) pexit("fseek(SEEK_SET)");

  uint8_t *content = (uint8_t*) malloc(size);
  if(content == NULL) error("read_file: Cannot allocate memory\n");
  // 从文件中读取 size 字节的数据到 content 指向的内存区域
  if(fread(content, 1, size, f) != size) error("read_file: Unexpected EOF\n");

  fclose(f);
  *content_ptr = content;
  *size_ptr = size;
}

/* set rip = entry point
 * set rsp = PS_LIMIT (the max address can be used)
 *
 * set rdi = PS_LIMIT (start of free (unpaging) physical pages)
 * set rsi = MEM_SIZE - rdi (total length of free pages)
 * Kernel could use rdi and rsi to initialize its memory allocator.
 */
// 设置虚拟 CPU 的初始寄存器状态，包括指令指针 RIP、栈指针 RSP，以及 RDI 和 RSI 寄存器用于传递内核的内存信息
void setup_regs(VM *vm, size_t entry) {
  struct kvm_regs regs;
  // 通过 ioctl 系统调用获取虚拟机CPU的通用寄存器状态，存储在 regs 变量中
  if(ioctl(vm->vcpufd, KVM_GET_REGS, &regs) < 0) pexit("ioctl(KVM_GET_REGS)");
  regs.rip = entry;
  regs.rsp = PS_LIMIT; /* temporary stack */
  regs.rdi = PS_LIMIT; /* start of free pages */
  regs.rsi = MEM_SIZE - regs.rdi; /* total length of free pages */
  regs.rflags = 0x2;
  // 通过 ioctl 系统调用设置虚拟机CPU的通用寄存器状态，使用 regs 变量中设置的值
  if(ioctl(vm->vcpufd, KVM_SET_REGS, &regs) < 0) pexit("ioctl(KVM_SET_REGS)");
}

/* Maps:
 * 0 ~ 0x200000 -> 0 ~ 0x200000 with kernel privilege
 */
// 手工构建x86-64 的页表（PML4 → PDP → PD），并配置 CPU 进入长模式（64位模式）和系统调用（EFER），\
包括设置控制寄存器 CR0、CR4 和 EFER 寄存器的值
void setup_paging(VM *vm) {
  struct kvm_sregs sregs;
  if(ioctl(vm->vcpufd, KVM_GET_SREGS, &sregs) < 0) pexit("ioctl(KVM_GET_SREGS)");
  uint64_t pml4_addr = MAX_KERNEL_SIZE; // 页表起始地址
  uint64_t *pml4 = (void*) (vm->mem + pml4_addr); // // PML4 页表地址，指向内存中页表的起始位置

  uint64_t pdp_addr = pml4_addr + 0x1000; // PDP 页表地址，位于 PML4 之后的第一个页面，+4KB
  uint64_t *pdp = (void*) (vm->mem + pdp_addr); // PDP 页表地址，指向内存中页表的起始位置

  uint64_t pd_addr = pdp_addr + 0x1000; // PD 页表地址，位于 PDP 之后的第一个页面，+4KB
  uint64_t *pd = (void*) (vm->mem + pd_addr); // PD 页表地址，指向内存中页表的起始位置

  // PML4 的第 0 项：指向 PDP 页
  pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdp_addr;
  // PDP 的第 0 项：指向 PD 页
  pdp[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;
  /* PD 的第 0 项：映射物理地址 0 ~ 2MB 到虚拟地址 0 ~ 2MB，使用 2MB 大页 */
  pd[0] = PDE64_PRESENT | PDE64_RW | PDE64_PS; /* kernel only, no PED64_USER */

  // CR3 - 页表基址寄存器，指向 PML4 页表的物理地址
  sregs.cr3 = pml4_addr;
  // CR4 - 控制寄存器 4，启用 PAE（Physical Address Extension）以支持 64 位地址空间，并启用 SSE 指令集相关的功能
  sregs.cr4 = CR4_PAE;
  sregs.cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT; /* enable SSE instruction */
  // CR0 - 控制寄存器 0，启用分页（PG）和保护模式（PE），以及其他相关的控制位
  sregs.cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_WP | CR0_AM | CR0_PG;
  // EFER - 扩展功能寄存器，启用长模式（LME）和系统调用（SCE）
  sregs.efer = EFER_LME | EFER_LMA;
  sregs.efer |= EFER_SCE; /* enable syscall instruction */

  if(ioctl(vm->vcpufd, KVM_SET_SREGS, &sregs) < 0) pexit("ioctl(KVM_SET_SREGS)");
}

// 设置段寄存器的内容，包括代码段（CS）和数据段（DS、ES、FS、GS、SS），并配置它们的属性，如基址、界限、选择子、类型、特权级等
void setup_seg_regs(VM *vm) {
  struct kvm_sregs sregs;
  // 获取当前段寄存器状态，存储在 sregs 变量中
  /*
  在 64 位长模式下，不再通过"不同的段基址/界限"来区分，而是通过分页机制和特权级来实现内存隔离和保护
  32位模式实现分段式内存管理 取指令：CPU 自动用 选择子CS 的基址（0）+ RIP，访问数据：CPU 自动用 选择子 DS/ES/FS/GS/SS 的基址（0）+ 偏移量 (寄存器中的地址)
  64位模式实现平坦模型 使用分页机制，所有段寄存器的基址都被忽略，界限也被忽略，选择子只负责索引（找到描述符）和提供RPL（请求特权级）。\
  区分代码/数据段、提供真正的特权级（DPL/CPL）都在段描述符中
  取指令： 0 + RIP，访问数据：0 + 偏移量 (寄存器中的地址)
  */
  if(ioctl(vm->vcpufd, KVM_GET_SREGS, &sregs) < 0) pexit("ioctl(KVM_GET_SREGS)");
  struct kvm_segment seg = {
    .base = 0, // 段基址（64位模式下无用）
    .limit = 0xffffffff, // 段界限（64位模式下无用）
    .selector = 1 << 3, // 段选择子 = 0x08,GDT 是由 KVM 自动管理的，我们只是告诉 KVM 我们要使用哪个描述符
    .present = 1, /* Segment is present */
    .type = 0xb, /* Code segment */
    .dpl = 0, /* Kernel: level 0 */
    .db = 0, // 默认操作数大小（0=64位，1=32位）
    .s = 1, // 代码/数据段（非系统段）
    .l = 1, /* long mode */
    .g = 1 // 粒度（4KB 为单位）
  };
  sregs.cs = seg; // 设置代码段寄存器 CS 的内容
  seg.type = 0x3; /* Data segment */
  seg.selector = 2 << 3; // 0x10（索引 2）
  sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = seg; 
  if(ioctl(vm->vcpufd, KVM_SET_SREGS, &sregs) < 0) pexit("ioctl(KVM_SET_SREGS)");
}

/*
 * Switching to long mode usually done by kernel.
 * We put the task in hypervisor because we want our KVM be able to execute
 * normal x86-64 assembled code as well. Which let us easier to debug and test.
 *
 */
void setup_long_mode(VM *vm) {
  setup_paging(vm);
  setup_seg_regs(vm);
}

// 完成打开 KVM 设备到创建可运行虚拟机的全部工作 \
初始化 KVM 虚拟机，创建虚拟机实例，分配内存，设置内存映射，创建 vCPU，并配置寄存器状态以准备执行虚拟机代码 \
param code 虚拟机要执行的代码，len 代码长度
VM* kvm_init(uint8_t code[], size_t len) {
  // 打开 KVM 设备文件(/dev/kvm，是用户空间与 KVM 内核模块通信的入口)，获取文件描述符 kvmfd，用于创建 VM
  int kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if(kvmfd < 0) pexit("open(/dev/kvm)");

  int api_ver = ioctl(kvmfd, KVM_GET_API_VERSION, 0);
	if(api_ver < 0) pexit("KVM_GET_API_VERSION");
  if(api_ver != KVM_API_VERSION) {
    error("Got KVM api version %d, expected %d\n",
      api_ver, KVM_API_VERSION);
  }
  // 创建一个 VM 容器（相当于一个虚拟机实例），获取文件描述符 vmfd，用于后续的内存映射和 vCPU 创建
  int vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);
  if(vmfd < 0) pexit("ioctl(KVM_CREATE_VM)");
  // 使用 mmap 系统调用在用户空间分配一块内存区域，大小为 MEM_SIZE（4MB），用于作为虚拟机的物理内存，并将其映射到用户空间地址 mem
  void *mem = mmap(0,
    MEM_SIZE,
    PROT_READ | PROT_WRITE,
    MAP_SHARED | MAP_ANONYMOUS,
    -1, 0);
  if(mem == NULL) pexit("mmap(MEM_SIZE)");
  // 将之前分配的内存区域注册到 KVM 中，告诉 KVM 这块内存将被用作虚拟机的物理内存，并指定其大小和起始地址
  size_t entry = 0;
  memcpy((void*) mem + entry, code, len);
  struct kvm_userspace_memory_region region = {
    .slot = 0,
    .flags = 0,
    .guest_phys_addr = 0,
    .memory_size = MEM_SIZE,
    .userspace_addr = (size_t) mem
  };
  if(ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
    pexit("ioctl(KVM_SET_USER_MEMORY_REGION)");
  }
  // 创建一个 vCPU（虚拟 CPU），获取文件描述符 vcpufd，用于后续的寄存器配置和执行控制
  int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
  if(vcpufd < 0) pexit("ioctl(KVM_CREATE_VCPU)");
  // 获取 vCPU 的 mmap 大小，使用 mmap 系统调用将 vCPU 的运行状态映射到用户空间地址 run，以便后续获取 vCPU 的运行状态和执行结果
  size_t vcpu_mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
  // 将 vCPU 的运行状态映射到用户空间地址 run，以便后续获取 vCPU 的运行状态和执行结果
  struct kvm_run *run = (struct kvm_run*) mmap(0,
    vcpu_mmap_size,
    PROT_READ | PROT_WRITE,
    MAP_SHARED,
    vcpufd, 0);

  VM *vm = (VM*) malloc(sizeof(VM));
  *vm = (struct VM){
    .mem = mem,
    .mem_size = MEM_SIZE,
    .vcpufd = vcpufd,
    .run = run
  };
  // 完成vCPU寄存器的初始设置，包括指令指针、栈指针和内核内存信息，进入长模式准备执行虚拟机代码
  setup_regs(vm, entry);
  setup_long_mode(vm);

  return vm;
}

int check_iopl(VM *vm) {
  struct kvm_regs regs;
  struct kvm_sregs sregs;
  if(ioctl(vm->vcpufd, KVM_GET_REGS, &regs) < 0) pexit("ioctl(KVM_GET_REGS)");
  if(ioctl(vm->vcpufd, KVM_GET_SREGS, &sregs) < 0) pexit("ioctl(KVM_GET_SREGS)");
  // 检查当前 CPU 的 IOPL（I/O Privilege Level）是否允许执行 I/O 操作，\
  IOPL 由 RFLAGS 寄存器的第 12 和第 13 位组成，表示当前的特权级别
  return sregs.cs.dpl <= ((regs.rflags >> 12) & 3);
}

// 执行虚拟机代码，进入一个循环，不断调用 ioctl(vm->vcpufd, KVM_RUN, NULL) 来运行 vCPU，并根据 vCPU 的退出原因进行处理，\
包括处理 HALT、I/O 端口访问、失败入口、内部错误和关机，Hypervisor 在虚拟机每次需要宿主机服务时获得控制权，处理完成后再让虚拟机继续执行
void execute(VM* vm) {
  while(1) {
    ioctl(vm->vcpufd, KVM_RUN, NULL); // vCPU会逐条执行Guest的指令，等待Guest退出 ，退出后可以通过 vm->run 结构体获取退出原因和相关信息
    // 根据是否有dubug，选择是否输出当前 vCPU 的寄存器状态，用于调试
    dump_regs(vm->vcpufd);
    // 根据 vCPU 的退出原因进行处理，KVM_EXIT_HLT 表示虚拟机执行了 HLT 指令，KVM_EXIT_IO 表示虚拟机执行了 I/O 端口访问...
    switch (vm->run->exit_reason) {
    case KVM_EXIT_HLT:
      fprintf(stderr, "KVM_EXIT_HLT\n");
      return;
    case KVM_EXIT_IO:
      if(!check_iopl(vm)) error("KVM_EXIT_SHUTDOWN\n");
      if(vm->run->io.port & HP_NR_MARK) {
        if(hp_handler(vm->run->io.port, vm) < 0) error("Hypercall failed\n");
      }
      else error("Unhandled I/O port: 0x%x\n", vm->run->io.port);
      break;
    case KVM_EXIT_FAIL_ENTRY:
      error("KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason = 0x%llx\n",
        vm->run->fail_entry.hardware_entry_failure_reason);
    case KVM_EXIT_INTERNAL_ERROR:
      error("KVM_EXIT_INTERNAL_ERROR: suberror = 0x%x\n",
        vm->run->internal.suberror);
    case KVM_EXIT_SHUTDOWN:
      error("KVM_EXIT_SHUTDOWN\n");
    default:
      error("Unhandled reason: %d\n", vm->run->exit_reason);
    }
  }
}

/* copy argv onto kernel's stack */
// 复制到虚拟机的栈中,使得内核可以像普通 C 程序一样访问 argc 和 argv
/*
高地址 ┌─────────────────┐
       │  字符串 2        │ ← 最先复制
       ├─────────────────┤
       │  字符串 1        │
       ├─────────────────┤
       │  字符串 0        │
       ├─────────────────┤ ← 16字节对齐
       │  argv[3] = NULL │
       ├─────────────────┤
       │  argv[2] 指针   │
       ├─────────────────┤
       │  argv[1] 指针   │
       ├─────────────────┤
       │  argv[0] 指针   │
       ├─────────────────┤
       │  argc = 3       │
低地址 └─────────────────┘
       ↑ RSP 指向这里
*/
void copy_argv(VM* vm, int argc, char *argv[]) {
  struct kvm_regs regs;
  if(ioctl(vm->vcpufd, KVM_GET_REGS, &regs) < 0) pexit("ioctl(KVM_GET_REGS)");
  char *sp = (char*)vm->mem + regs.rsp; // 获取虚拟机当前栈指针的值，计算出虚拟机内存中栈顶的地址，准备在这个地址上构建新的栈帧来存放 argc 和 argv
  char **copy = (char**) malloc(argc * sizeof(char*)); // 分配一个临时数组 copy，用于存储每个参数在虚拟机内存中的地址
#define STACK_ALLOC(sp, len) ({ sp -= len; sp; }) // 由高地址向低地址分配栈空间的宏，更新栈指针 sp 并返回分配的内存地址
  for(int i = argc - 1; i >= 0; i--) {
    int len = strlen(argv[i]) + 1;
    copy[i] = STACK_ALLOC(sp, len);
    memcpy(copy[i], argv[i], len);
  }
  // 调整栈指针，使其保持 16 字节对齐，以满足 x86-64 架构的调用约定要求
  sp = (char*) ((uint64_t) sp & -0x10);
  /* push argv */
  *(uint64_t*) STACK_ALLOC(sp, sizeof(char*)) = 0; /* argv[argc] = NULL */
  for(int i = argc - 1; i >= 0; i--)
    *(uint64_t*) STACK_ALLOC(sp, sizeof(char*)) = copy[i] - (char*)vm->mem;
  /* push argc */
  *(uint64_t*) STACK_ALLOC(sp, sizeof(uint64_t)) = argc;
  free(copy);
#undef STACK_ALLOC
  regs.rsp = sp - (char*) vm->mem;
  // 更新虚拟机的栈指针寄存器 RSP 的值，使其指向新的栈顶位置，准备让虚拟机内核代码能够正确访问 argc 和 argv
  if(ioctl(vm->vcpufd, KVM_SET_REGS, &regs) < 0) pexit("ioctl(KVM_SET_REGS)");
}

int main(int argc, char *argv[]) {
  if(argc < 3) {
    printf("Usage: %s kernel.bin user.elf [user_args...]\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  uint8_t *code;
  size_t len;
  read_file(argv[1], &code, &len);
  if(len > MAX_KERNEL_SIZE)
    error("Kernel size exceeded, %p > MAX_KERNEL_SIZE(%p).\n",
      (void*) len,
      (void*) MAX_KERNEL_SIZE);
  // Hypervisor加载Guest内核二进制文件到内存
  VM* vm = kvm_init(code, len);
  // 给Guest内核传递用户程序的参数，复制到虚拟机的栈中，使得Guest内核可以像普通 C 程序一样访问 argc 和 argv
  copy_argv(vm, argc - 2, &argv[2]);
  execute(vm);
}
