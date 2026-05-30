#include <hypercalls/hypercall.h>

// 发送 hypercall 请求给 Hypervisor，port 是 hypercall 编号，data 是传递的数据，返回值是 hypercall 的结果
int hypercall(uint16_t port, uint32_t data) {
  int ret = 0;
  asm(
    "mov dx, %[port];"
    "mov eax, %[data];"
    "out dx, eax;"
    "in eax, dx;"
    "mov %[ret], eax;"
    : [ret] "=r"(ret)
    : [port] "r"(port), [data] "r"(data)
    : "rax", "rdx"
    );
  return ret;
}
