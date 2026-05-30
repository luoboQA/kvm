#!/usr/bin/env python3

"""
生成用户程序 orw.elf，接受一个参数，它实现了一个简单的文件读取功能（open → read → write）
"""
from pathlib import Path
from pwn import asm, context, make_elf, shellcraft
import os

def generate(filename, data):
    elf = make_elf(data)           # 从机器码生成 ELF
    path = os.path.join(Path().absolute(), filename)
    open(path, 'wb').write(elf)    # 写入文件
    os.chmod(path, 0o755)          # 设置可执行权限

context.arch = 'amd64' # 64位 x86 架构

generate('orw.elf', asm(
    f'''
    mov rdi, [rsp]  /* rdi = argc（栈顶第一个值） */
    cmp rdi, 2     /* 判断参数个数是否至少为 2 */
    jb exit
    mov rdi, [rsp + 16] /* argv[1] */
    /* 调用shellcraft 模块生成的汇编代码来实现 open、read 和 write 系统调用 */
    {shellcraft.open('rdi', 'O_RDONLY', 0)} /* 调用 open(argv[1], O_RDONLY) 打开文件，返回的文件描述符保存在 rax 中 */
    {shellcraft.read('rax', 'rsp', 0x1000)} /* 调用 read(rax, rsp, 0x1000) 从文件中读取最多 0x1000 字节的数据到栈顶，返回的实际读取字节数保存在 rax 中 */
    {shellcraft.write(1, 'rsp', 'rax')} /* 调用 write(1, rsp, rax) 将刚才读取的数据写到标准输出，rax 中的值表示实际写出的字节数 */
exit:
    xor rdi, rdi /* rdi = 0，表示正常退出状态码 */
    mov rax, 60 /* syscall: exit */
    syscall 

    /* should not reach here */
    hlt
    '''))
