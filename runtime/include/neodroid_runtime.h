#pragma once

#include "neodroid_elf.h"
#include "neodroid_syscalls.h"
#include "neodroid_types.h"
#include "neodroid_vfs.h"

#include <memory>
#include <unordered_map>

namespace neodroid {

class GuestMemory {
public:
    virtual ~GuestMemory() = default;
    virtual void* map(GuestRange range, MemoryProtection protection) = 0;
    virtual bool unmap(GuestRange range) = 0;
    virtual bool protect(GuestRange range, MemoryProtection protection) = 0;
    virtual bool copy_to_guest(GuestAddress address, const void* source,
                               GuestSize size) = 0;
    virtual bool copy_from_guest(void* destination, GuestAddress address,
                                 GuestSize size) const = 0;
    virtual bool is_mapped(GuestRange range) const = 0;
};

class ClockSource {
public:
    virtual ~ClockSource() = default;
    virtual std::uint64_t monotonic_ns() const = 0;
    virtual std::uint64_t realtime_ns() const = 0;
};

class FutexTable {
public:
    virtual ~FutexTable() = default;
    virtual SyscallResult wait(GuestAddress address, std::uint32_t expected,
                               std::uint64_t timeout_ns) = 0;
    virtual SyscallResult wake(GuestAddress address, std::int32_t count) = 0;
};

class Runtime {
public:
    struct Config {
        GuestSize guest_ram{kDefaultGuestRam};
        std::uint32_t vcpu_count{8};
        bool strict_manifest{true};
        bool enable_aot{true};
        bool enable_gpu{true};
    };

    explicit Runtime(Config config);

    SyscallResult dispatch_syscall(GuestRegisters& registers);
    SyscallResult fast_clock_gettime(GuestRegisters& registers);
    SyscallResult fast_gettid(GuestRegisters& registers);
    SyscallResult fast_futex(GuestRegisters& registers);
    SyscallResult fast_epoll(GuestRegisters& registers);

    LoadedElf load_program(std::string_view path);
    GuestPid spawn(std::string_view executable,
                   std::span<const std::string> argv,
                   std::span<const std::string> envp);

    [[nodiscard]] VirtualFileSystem& vfs() { return vfs_; }
    [[nodiscard]] GuestMemory& memory() { return *memory_; }
    [[nodiscard]] const ClockSource& clock() const { return *clock_; }
    [[nodiscard]] GuestTid current_tid() const { return current_tid_; }
    [[nodiscard]] GuestPid current_pid() const { return current_pid_; }

    void set_memory(std::unique_ptr<GuestMemory> memory);
    void set_clock(std::unique_ptr<ClockSource> clock);
    void set_futexes(std::unique_ptr<FutexTable> futexes);

private:
    Config config_;
    VirtualFileSystem vfs_;
    std::unique_ptr<GuestMemory> memory_;
    std::unique_ptr<ClockSource> clock_;
    std::unique_ptr<FutexTable> futexes_;
    ElfLoader elf_loader_;
    GuestPid current_pid_{1};
    GuestTid current_tid_{1};
    std::unordered_map<std::uint32_t, SyscallDescriptor> syscalls_;

    void install_fast_syscalls();
};

}  // namespace neodroid
