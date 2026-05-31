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
