//
//  NeoDroidBridge.mm
//  NeoDroid iOS P0 — Objective-C++ bridge into the C++ runtime.
//

#import "NeoDroidBridge.h"
#include "neodroid_runtime.h"

#include <sys/utsname.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <map>
#include <memory>
#include <vector>
#include <cstring>

namespace {

// Minimal in-process guest memory used by the P0 self-test.
class ProbeMemory final : public neodroid::GuestMemory {
public:
    void *map(neodroid::GuestRange range, neodroid::MemoryProtection) override {
        auto &block = blocks_[range.start];
        block.assign(range.size, std::byte{0});
        return block.data();
    }
    bool unmap(neodroid::GuestRange range) override {
        return blocks_.erase(range.start) > 0;
    }
    bool protect(neodroid::GuestRange, neodroid::MemoryProtection) override {
        return true;
    }
    bool copy_to_guest(neodroid::GuestAddress address, const void *source,
                       neodroid::GuestSize size) override {
        for (auto &[start, block] : blocks_) {
            if (address >= start && address + size <= start + block.size()) {
                std::memcpy(block.data() + (address - start), source, size);
                return true;
            }
        }
        return false;
    }
    bool copy_from_guest(void *destination, neodroid::GuestAddress address,
                         neodroid::GuestSize size) const override {
        for (const auto &[start, block] : blocks_) {
            if (address >= start && address + size <= start + block.size()) {
                std::memcpy(destination, block.data() + (address - start), size);
                return true;
            }
        }
        return false;
    }
    bool is_mapped(neodroid::GuestRange range) const override {
        return blocks_.find(range.start) != blocks_.end();
    }

private:
    std::map<neodroid::GuestAddress, std::vector<std::byte>> blocks_;
};

}  // namespace

NSString *NeoDroidArchitectureInfo(void) {
    struct utsname info{};
    uname(&info);
    std::ostringstream stream;
    stream << "machine=" << info.machine
           << " · sysname=" << info.sysname
           << " · page=" << getpagesize() << "B";
    return [NSString stringWithUTF8String:stream.str().c_str()];
}

NSString *NeoDroidRunSelfTest(void) {
    std::ostringstream out;

    neodroid::Runtime runtime({});
    auto memory = std::make_unique<ProbeMemory>();
    auto *memory_raw = memory.get();
    runtime.set_memory(std::move(memory));

    // 1. gettid fast path
    {
        neodroid::GuestRegisters regs{};
        regs.x[8] = static_cast<std::uint64_t>(neodroid::Syscall::GetTid);
        const auto result = runtime.dispatch_syscall(regs);
        out << (result == 1 ? "PASS" : "FAIL") << "|gettid fast-path|result=" << result << "\n";
    }

    // 2. clock_gettime fast path into guest memory
    {
        constexpr std::uint64_t slot = 0x4000;
        memory_raw->map({slot, 16}, neodroid::MemoryProtection::Write);
        neodroid::GuestRegisters regs{};
        regs.x[8] = static_cast<std::uint64_t>(neodroid::Syscall::ClockGetTime);
        regs.x[0] = 1;
        regs.x[1] = slot;
        const auto result = runtime.dispatch_syscall(regs);
        struct Timespec { std::int64_t sec; std::int64_t nsec; } ts{};
        memory_raw->copy_from_guest(&ts, slot, sizeof(ts));
        out << (result == 0 && ts.sec >= 0 ? "PASS" : "FAIL")
            << "|clock_gettime fast-path|sec=" << ts.sec << " nsec=" << ts.nsec << "\n";
    }

    // 3. unknown syscall must return ENOSYS
    {
        neodroid::GuestRegisters regs{};
        regs.x[8] = 9999;
        const auto result = runtime.dispatch_syscall(regs);
        out << (result == neodroid::kGuestEnosys ? "PASS" : "FAIL")
            << "|unknown syscall -> ENOSYS|result=" << result << "\n";
    }

    // 4. AArch64 native instruction execution (hot path, zero translation)
    {
#if defined(__aarch64__)
        std::uint64_t a = 0x12345678ULL, b = 0x87654321ULL, sum = 0;
        __asm__ volatile("add %0, %1, %2" : "=r"(sum) : "r"(a), "r"(b));
        out << (sum == 0x99999999ULL ? "PASS" : "FAIL")
            << "|AArch64 native add|0x" << std::hex << sum << std::dec << "\n";
#else
        out << "FAIL|AArch64 native add|not arm64\n";
#endif
    }

    // 5. SVC boundary reachable (gettid via inline svc)
    {
#if defined(__aarch64__)
        register long x8 __asm__("x8") = 178;  // __NR_gettid
        register long x0 __asm__("x0");
        __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
        out << (x0 > 0 ? "PASS" : "FAIL") << "|SVC syscall boundary|tid=" << x0 << "\n";
#else
        out << "FAIL|SVC syscall boundary|not arm64\n";
#endif
    }

    // 6. ELF64/AArch64 parser smoke test (synthesizes a minimal header)
    {
        std::vector<std::byte> fake(64, std::byte{0});
        const char magic[4] = {0x7f, 'E', 'L', 'F'};
        std::memcpy(fake.data(), magic, 4);
        fake[4] = std::byte{2};  // ELFCLASS64
        fake[5] = std::byte{1};  // ELFDATA2LSB
        // e_type @0x10, e_machine @0x12 = 183 (AArch64)
        fake[0x12] = std::byte{0xB7};
        bool parsed = false;
        try {
            static const neodroid::ElfLoader loader;
            const auto image = std::span<const std::byte>(fake.data(), fake.size());
            (void)loader.inspect("/p0/fake", image);
            parsed = true;
        } catch (...) {
            parsed = false;
        }
        out << (parsed ? "PASS" : "FAIL") << "|ELF64/AArch64 header parse|183=AArch64\n";
    }

    return [NSString stringWithUTF8String:out.str().c_str()];
}
// ---------------------------------------------------------------------------
// P1: 执行打包进 App 的 guest.elf（静态 AArch64 ELF，Linux ABI 子集）
// ---------------------------------------------------------------------------
#include "neodroid_guest.h"
#include <fstream>

NSString *NeoDroidRunGuestElf(void) {
    NSString *path = [[NSBundle mainBundle] pathForResource:@"guest" ofType:@"elf"];
    if (path == nil) {
        return @"[FAIL] 未找到 guest.elf 资源";
    }

    std::ifstream in([path UTF8String], std::ios::binary);
    if (!in) {
        return @"[FAIL] 无法打开 guest.elf";
    }
    std::vector<std::byte> image((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

    NSMutableString *report = [NSMutableString string];
    [report appendFormat:@"guest.elf: %lu bytes\n", (unsigned long)image.size()];

    const auto result = neodroid::run_guest_elf(image);

    if (!result.ok) {
        [report appendFormat:@"[FAIL] %s\n", result.error.c_str()];
        return report;
    }

    [report appendFormat:@"[OK] 退出码 %d\n", result.exit_code];
    [report appendString:@"----- guest 输出 -----\n"];
    if (result.output.empty()) {
        [report appendString:@"(无输出)\n"];
    } else {
        [report appendFormat:@"%s", result.output.c_str()];
    }

    // 释放 guest 内存映射
    // 注意：guest 运行后其映射仍保留在进程内，P1 阶段不做回收。
    return report;
}
