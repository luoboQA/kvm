#include <hypercalls/hp_open.h>
#include <mm/kmalloc.h>
#include <mm/translate.h>
#include <mm/uaccess.h>
#include <syscalls/sys_open.h>
#include <utils/errno.h>

int sys_open(const char *path) {
    // 1. 检查用户传入的路径指针是否有效
    if(!access_string_ok(path)) return -EFAULT;
    
    // 2. 将用户空间的字符串复制到内核空间
    void *dst = copy_str_from_user(path);
    if(dst == 0) return -ENOMEM;
    
    // 3. 触发超级调用，传递物理地址
    int fd = hp_open(physical(dst));
    
    // 4. 释放内核缓冲区
    kfree(dst);
    
    return fd;
}
