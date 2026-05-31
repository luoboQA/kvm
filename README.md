# KVM-Kernel Example

The source code are examples on my blog: [Learning KVM - implement your own kernel](https://david942j.blogspot.com/2018/10/note-learning-kvm-implement-your-own.html).

I've described how to implement a KVM-based hypervisor and the key points to implement a kernel on my blog.
You can leave comments in the blog or file issues here if you have questions or find any bug.

## Flow
Hypervisor(宿主机用户程序)     Guest内核 (kernel.bin)                用户程序 (orw.elf)
─────────────────────────────────────────────────────────────────────────────────
KVM_RUN ──────→  kernel/entry.s 
                 entry.s:_start (RIP=0)
                 call kernel_main()
                     │
                     ├─ register_syscall()
                     ├─ switch_user()
                            │
                            └─ sys_execve()
                                  │
                                  ├─ sys_open("orw.elf")  ← 超级调用
                                  ├─ sys_read()           ← 超级调用
                                  ├─ load_binary()        ← 解析 ELF
                                  ├─ create_elf_info()    ← 设置用户栈
                                  │
                                  └─ asm("sysret")    ← 切换到用户态
                                      (内核暂停)             │
                                                            ↓
                                    (等待用户程序)     用户程序开始执行
                                                            │
                                                            ├─ open("/etc/os-release") ← 系统调用
                                                            │      │
                                                            │      ↓ (进入内核)
                                                            │   syscall_entry
                                                            │      │
                                                            │      └─ 超级调用 → Hypervisor
                                                            │
                                                            └─ sys_call (exit)
                                                                     │
                                                                     └─ 超级调用 exit()
                                                                              │ (内核收到退出)
                                                                              │
                                                                              ↓
                                                                       Hypervisor 退出

页表（page table）是个多级数组目录放在内存里的"地址本",他的每个条目 = 物理地址(52位) + 权限信息(12位)，每条指向下一级页表 或 最终物理页
而TLB (Translation Lookaside Buffer，翻译后备缓冲区) 是 CPU 内部的页表缓存
在任务/进程切换时，操作系统会切换页表。CPU 执行时通过 CR3 寄存器知道当前使用哪个页表
// 操作系统上下文切换 (简化)
void switch_to_process(process_t *next) {
    // 1. 保存当前进程状态
    current->cr3 = read_cr3();  // 保存当前页表地址
    
    // 2. 加载新进程的页表
    write_cr3(next->cr3);       // ← 切换页表！
    
    // 3. 恢复新进程的其他寄存器
    restore_registers(next);
    
    // 4. 返回，CPU 现在使用新页表
}
地址读取：
1. 程序使用虚拟地址 0x00007f1234567000

2. CPU 自动拆解：
   PML4索引 = 0x0FA
   PDP索引  = 0x123
   PD索引   = 0x456
   PT索引   = 0x789
   页内偏移  = 0x000

3. 查表 (CR3 指向 PML4 基址)：
   PML4[0x0FA] → 找到 PDP 页
   PDP[0x123]  → 找到 PD 页
   PD[0x456]   → 找到 PT 页
   PT[0x789]   → 找到物理页基址

4. 物理地址 = 物理页基址 + 0x000

初始状态：
CR3 = 0x0000000000100000  (PML4 页的物理地址)
虚拟地址 = 0x00007f1234567000

步骤 1: 提取索引
PML4索引 = (0x00007f1234567000 >> 39) & 0x1FF = 0x0FA
PDP索引  = (0x00007f1234567000 >> 30) & 0x1FF = 0x123
PD索引   = (0x00007f1234567000 >> 21) & 0x1FF = 0x456
PT索引   = (0x00007f1234567000 >> 12) & 0x1FF = 0x789
页内偏移  = 0x00007f1234567000 & 0xFFF = 0x000

步骤 2: 读取 PML4 项
地址 = CR3 + 0x0FA * 8 = 0x100000 + 0x7D0 = 0x1007D0
内容 = 0x0000000000102007  (二进制: ... 0010 0000 0000 0111)
      └─ addr = 0x102000  (高 52 位)
      └─ flags = 0x007    (低 12 位: present=1, rw=1, user=1)

步骤 3: 读取 PDP 项
地址 = 0x102000 + 0x123 * 8 = 0x102000 + 0x918 = 0x102918
内容 = 0x0000000000103007  (addr=0x103000, flags=0x007)

步骤 4: 读取 PD 项
地址 = 0x103000 + 0x456 * 8 = 0x103000 + 0x22B0 = 0x1052B0
内容 = 0x0000000000104007  (addr=0x104000, flags=0x007)

步骤 5: 读取 PT 项
地址 = 0x104000 + 0x789 * 8 = 0x104000 + 0x3C48 = 0x107C48
内容 = 0x0000000000200007  (addr=0x200000, flags=0x007)

步骤 6: 计算物理地址
物理页基址 = 0x200000
物理地址 = 0x200000 + 0x000 = 0x200000
## Dir

### Hypervisor

The KVM-based hypervisor, its role is like qemu-system.

### Kernel

A extremely simple kernel, supports few syscalls.

### User

Simple ELF(s) for testing our kernel.
Pre-built user program was provided, and you can re-generate by the following commands:
```sh
$ pip3 install pwntools
$ user/gen.py
```
NOTE: You have to install Python 3.x in advance.

## Setup

### Check KVM support

Check if your CPU supports virtualization:
```
$ egrep '(vmx|svm)' /proc/cpuinfo
```
NOTE: CPUs in a VM might not support virtualization (i.e. no nested virtualization).
For example, EC2 on AWS doesn't support using KVM.

### Install KVM device

Check if the KVM device exists:
```
$ ls -la /dev/kvm
```

If `/dev/kvm` is not found, you can enable it (on Ubuntu) with:
```
$ sudo apt install qemu-kvm
```

If you are not root, you need to add yourself into the `kvm` group to have permission for accessing `/dev/kvm`.
```
$ sudo usermod -a -G kvm `whoami`
```
Remember to logout and login to have the group changing effective.


## How to run

```sh
$ git clone https://github.com/david942j/kvm-kernel-example
$ cd kvm-kernel-example && make
$ hypervisor/hypervisor.elf kernel/kernel.bin user/orw.elf /etc/os-release
# NAME="Ubuntu"
# VERSION="18.04.1 LTS (Bionic Beaver)"
# ID=ubuntu
# ID_LIKE=debian
# PRETTY_NAME="Ubuntu 18.04.1 LTS"
# VERSION_ID="18.04"
# HOME_URL="https://www.ubuntu.com/"
# SUPPORT_URL="https://help.ubuntu.com/"
# BUG_REPORT_URL="https://bugs.launchpad.net/ubuntu/"
# PRIVACY_POLICY_URL="https://www.ubuntu.com/legal/terms-and-policies/privacy-policy"
# VERSION_CODENAME=bionic
# UBUNTU_CODENAME=bionic
# +++ exited with 0 +++
```

### Environment

I only tested the code on Ubuntu 18.04, but I expect it to work on all KVM-supported x86 Linux distributions. Please file an issue if you find it's not true.
