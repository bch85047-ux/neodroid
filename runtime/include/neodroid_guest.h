#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace neodroid {

struct GuestRunResult {
    bool ok{false};
    int exit_code{-1};
    std::string output;   // guest 写到 stdout/stderr 的内容
    std::string error;    // 运行时失败原因
};

// 加载并执行一个静态 AArch64 ELF 镜像（Linux ABI 子集）。
// 机制：拷贝到匿名内存 -> 把每个 svc #0 重写成绝对 thunk ->
// thunk 设置返回地址并跳到 trampoline -> C++ 分发器处理 syscall。
// 由宿主注入沙箱路径（iOS 的 tmp/Documents 目录），用于 guest 路径映射。
void set_sandbox_paths(const char *tmp_dir, const char *docs_dir);

GuestRunResult run_guest_elf(const std::vector<std::byte>& elf_image);

}  // namespace neodroid