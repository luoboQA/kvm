# KVM-Kernel Example

The source code are examples on my blog: [Learning KVM - implement your own kernel](https://david942j.blogspot.com/2018/10/note-learning-kvm-implement-your-own.html).

I've described how to implement a KVM-based hypervisor and the key points to implement a kernel on my blog.
You can leave comments in the blog or file issues here if you have questions or find any bug.

## Flow
```text
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

第一次访问某个线性地址:
┌─────────────────────────────────────────────────────────────────┐
│ 线性地址 → 查页表（遍历 PML4→PDP→PD→PT）→ 得到物理地址             │
│                     ↓                                           │
│               CPU 自动缓存到 TLB                                 │
└─────────────────────────────────────────────────────────────────┘

第二次访问同一个线性地址:
┌─────────────────────────────────────────────────────────────────┐
│ 线性地址 → 先查 TLB（命中！）→ 直接得到物理地址（不需要查页表）      │
└─────────────────────────────────────────────────────────────────┘

// ========== 阶段1：加载程序（页表映射）==========

// 1. 分配物理页、填写页表，直接使用真实物理内存
uint64_t phys_page = alloc_physical_page();  // 返回 0x200000

// 2. 把程序代码读入这个物理页
read_file_to_phys("orw.elf", phys_page);

// 3. 建立页表映射（线性地址 → 物理地址）
//    假设用户程序入口线性地址是 0x400000
pml4[0] = ...;      // 各级页表
pdp[0]  = ...;
pd[1]   = ...;      // 假设 PD 索引 1
pt[0]   = phys_page | 0x007;  // 0x400000 → 0x200000

// 4. 设置 CR3（告诉 CPU 页表位置，准备执行）
write_cr3(pml4_phys);


// ========== 阶段2：跳转执行（CPU 使用页表）==========

// 5. 跳转到入口点
rip = 0x400000;     // 程序入口线性地址

// 6. CPU 取指令时自动查页表
//    线性地址 0x400000 → 查页表 → 物理地址 0x200000
//    CPU 从物理地址 0x200000 读取指令执行


64位虚拟地址 (48位有效)
┌────────────┬───────────┬───────────┬───────────┬────────────┐
│   PML4     │   PDP     │    PD     │    PT     │   业内偏移  │
│   9位      │   9位     │   9位     │   9位      │   12位     │
└────────────┴───────────┴───────────┴───────────┴────────────┘

每个 PML4 条目覆盖的地址范围 = 2^(9+9+9+12) = 2^39 = 512GB
                                ↑ ↑ ↑ ↑
                              PDP PD PT 偏移
- PT 有 512 个条目，每个映射 4KB → 2MB (512×4KB)
- PD 有 512 个条目，每个指向 PT → 1GB (512×2MB)
- PDP 有 512 个条目，每个指向 PD → 512GB (512×1GB)
64位虚拟地址有 2^64 字节空间，但实际只用 48 位有效。PML4 有 512 个条目，每个条目覆盖 512GB 空间。32 位程序受限于 32 位寄存器，地址高位永远为 0，所以永远落在 PML4[0] 区域；64 位程序可以使用完整的 64 位地址，索引到任意 PML4 条目

场景	段转换	分页转换	页表格式
64位内核 + 64位用户	基址强制为0	64位页表	4级 (PML4→PDP→PD→PT)
64位内核 + 32位用户	基址有效 (但设为0)	64位页表	4级 (PML4→PDP→PD→PT)
32 位地址零扩展成 64 位后查表

段地址（段选择子）告诉 CPU 用哪个段；逻辑地址是段内的偏移量。
完整的地址 = 段地址 : 逻辑地址
           = 段选择子 : 偏移量

例如: 0x08:0x12345678
      └─┬─┘ └───┬───┘
    段地址   逻辑地址
   (选择子)   (偏移)
段地址(0x08) → 查 GDT → 找到段描述符 → 取出段基址(0x10000000)
                                        +
                              逻辑地址(0x12345678)
                                        =
                              线性地址(0x112345678)
线性地址: 0x112345678

写成二进制（48位有效）:
0x112345678 = 0000 0001 0001 0010 0011 0100 0101 0110 0111 1000
             └─────┬─────┘ └──┬──┘ └──┬──┘ └──┬──┘ └────┬────┘
               9位          9位     9位     9位      12位
               PML4索引      PDP索引  PD索引  PT索引    偏移

具体值:
PML4索引 = (0x112345678 >> 39) & 0x1FF = 0x02
PDP索引  = (0x112345678 >> 30) & 0x1FF = 0x048
PD索引   = (0x112345678 >> 21) & 0x1FF = 0x11A
PT索引   = (0x112345678 >> 12) & 0x1FF = 0x2B3
页内偏移  = 0x112345678 & 0xFFF = 0x678

查 PML4 页（第1级）
CR3 寄存器 = 0x00100000  (PML4 页的物理地址)

PML4 条目地址 = CR3 + PML4索引 × 8
             = 0x00100000 + 0x02 × 8
             = 0x00100010

PML4 条目内容 = 0x0000000000102007
                      │
                      ├── 低12位 = 0x007 (present=1, rw=1, user=1)
                      └── 高52位 = 0x0000000000102000 (PDP 页的物理地址)

取出 PDP 页基址 = 0x102000
查 PDP 页（第2级）
PDP 条目地址 = PDP页基址 + PDP索引 × 8
             = 0x102000 + 0x048 × 8
             = 0x102000 + 0x240
             = 0x102240

PDP 条目内容 = 0x0000000000103007
                      │
                      ├── 低12位 = 0x007 (present=1, rw=1, user=1)
                      └── 高52位 = 0x0000000000103000 (PD 页的物理地址)

取出 PD 页基址 = 0x103000
查 PD 页（第3级）
PD 条目地址 = PD页基址 + PD索引 × 8
            = 0x103000 + 0x11A × 8
            = 0x103000 + 0x8D0
            = 0x1038D0

PD 条目内容 = 0x0000000000104007
                      │
                      ├── 低12位 = 0x007 (present=1, rw=1, user=1, PS=0)
                      └── 高52位 = 0x0000000000104000 (PT 页的物理地址)

取出 PT 页基址 = 0x104000
查 PT 页（第4级）
PT 条目地址 = PT页基址 + PT索引 × 8
            = 0x104000 + 0x2B3 × 8
            = 0x104000 + 0x1598
            = 0x105598

PT 条目内容 = 0x0000000000200007
                      │
                      ├── 低12位 = 0x007 (present=1, rw=1, user=1)
                      └── 高52位 = 0x0000000000200000 (物理页基址)

取出物理页基址 = 0x200000
计算最终物理地址
物理地址 = 物理页基址 + 页内偏移
         = 0x200000 + 0x678
         = 0x200678
   

逻辑地址 = 段转换前的地址（程序直接给的）
线性地址 = 段转换后的地址（也叫虚拟地址）
物理地址 = 分页转换后的地址（真实内存）

程序给出的地址
      │
      ▼
┌─────────────────────────────────────────────┐
│  逻辑地址 (Logical Address)                  │
│  = 程序直接使用的地址                         │
│  例如: mov eax, [0x12345678] 中的 0x12345678 │
└─────────────────────────────────────────────┘
      │
      ▼ (段转换)
┌─────────────────────────────────────────────┐
│  线性地址 (Linear Address)                   │
│  = 段基址 + 逻辑地址                         │
│  = 也叫虚拟地址 (Virtual Address)            │
└─────────────────────────────────────────────┘
      │
      ▼ (分页转换)
┌─────────────────────────────────────────────┐
│  物理地址 (Physical Address)                 │
│  = 真正的内存位置                            │
└─────────────────────────────────────────────┘

32位程序:
    逻辑地址 → (段转换，基址=0) → 线性地址 → (分页) → 物理地址
                                   ↑
                        逻辑地址 = 线性地址（因为基址为0）
64位程序:
    逻辑地址 → (段转换，基址强制0) → 线性地址 → (分页) → 物理地址
    
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

4. 物理地址 = 物理页基址 + 0x000（页内偏移）

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
```
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
